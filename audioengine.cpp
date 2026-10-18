/*
	Copyright (C) 2019-2021 Doug McLain

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "audioengine.h"
#include <QDebug>
#include <cmath>
#include <cstring>

#if defined (Q_OS_MACOS) || defined(Q_OS_IOS)
#define MACHAK 1
#else
#define MACHAK 0
#endif

AudioEngine::AudioEngine(QString in, QString out) :
	m_outputdevice(out),
	m_inputdevice(in),
	m_out(nullptr),
	m_in(nullptr),
	m_srm(1)
{
	m_audio_out_temp_buf_p = m_audio_out_temp_buf;
	memset(m_aout_max_buf, 0, sizeof(float) * 200);
	m_aout_max_buf_p = m_aout_max_buf;
	m_aout_max_buf_idx = 0;
	m_aout_gain = 100;
	m_volume = 1.0f;

	// Kaiser-windowed sinc low-pass at 16 kHz for the 8 kHz -> 16 kHz
	// interpolator: flat to ~3.4 kHz, more than 70 dB down above 4.2 kHz,
	// so no images of the 8 kHz signal appear in the 4-8 kHz band.
	const double PI = 3.14159265358979323846;
	const double fc = 3800.0 / 16000.0;
	const double beta = 8.0;
	auto bessel_i0 = [](double x) {
		double sum = 1.0, term = 1.0;
		for(int k = 1; k < 50; ++k){
			const double t = x / (2.0 * k);
			term *= t * t;
			sum += term;
			if(term < sum * 1e-12){
				break;
			}
		}
		return sum;
	};
	double h[UP_TAPS];
	double sum = 0.0;
	for(int n = 0; n < UP_TAPS; ++n){
		const double x = n - UP_HIST;
		const double sinc = (n == UP_HIST) ? 2.0 * fc : std::sin(2.0 * PI * fc * x) / (PI * x);
		const double r = x / UP_HIST;
		h[n] = sinc * bessel_i0(beta * std::sqrt(1.0 - r * r)) / bessel_i0(beta);
		sum += h[n];
	}
	// Zero stuffing halves the level, so each polyphase branch needs gain 2
	for(int n = 0; n < UP_TAPS; ++n){
		m_up_taps[n] = (float)(2.0 * h[n] / sum);
	}
	memset(m_up_hist, 0, sizeof(m_up_hist));
}

AudioEngine::~AudioEngine()
{
}

QStringList AudioEngine::discover_audio_devices(uint8_t d)
{
	QStringList list;
	QList<QAudioDevice> devices;

	if(d){
		devices = QMediaDevices::audioOutputs();
	}
	else{
		devices = QMediaDevices::audioInputs();
	}

	for (QList<QAudioDevice>::ConstIterator it = devices.constBegin(); it != devices.constEnd(); ++it ) {
		//fprintf(stderr, "Playback device name = %s\n", (*it).deviceName().toStdString().c_str());fflush(stderr);
		list.append((*it).description());
	}

	return list;
}

void AudioEngine::init()
{
	QAudioFormat format;
	format.setSampleRate(8000);
	format.setChannelCount(1);
	format.setSampleFormat(QAudioFormat::Int16);

	// Playback at 16 kHz (wideband decoders), capture stays at 8 kHz
	QAudioFormat outformat = format;
	outformat.setSampleRate(OUTPUT_SAMPLE_RATE);

	m_agc = true;

	QList<QAudioDevice> devices = QMediaDevices::audioOutputs();
	if(devices.size() == 0){
        qDebug() << "No audio playback hardware found";
	}
	else{
		QAudioDevice device(QMediaDevices::defaultAudioOutput());
		for (QList<QAudioDevice>::ConstIterator it = devices.constBegin(); it != devices.constEnd(); ++it ) {

            //qDebug() << "Playback device name = " << (*it).description();
            //qDebug() << (*it).supportedSampleFormats();
            //qDebug() << (*it).preferredFormat();
            //qDebug() << (*it).minimumSampleRate();
            //qDebug() << (*it).maximumSampleRate();


			if((*it).description() == m_outputdevice){
				device = *it;
			}
		}
		if (!device.isFormatSupported(outformat)) {
            qWarning() << "Current audio format not supported by playback device";
        }

        qDebug() << "Playback device: " << device.description() << "SR: " << outformat.sampleRate();

        try{
            m_out = new QAudioSink(device, outformat, this);
        }
        catch (const std::exception& e) {
            qDebug() << "Exception in constructor:" << e.what();
        }

		m_out->setBufferSize(2560); // 80 ms at 16 kHz
		connect(m_out, SIGNAL(stateChanged(QAudio::State)), this, SLOT(handleStateChanged(QAudio::State)));
	}

	devices = QMediaDevices::audioInputs();

    if(devices.size() == 0){
        qDebug() <<  "No audio capture hardware found";
	}
	else{
		QAudioDevice device(QMediaDevices::defaultAudioInput());
		for (QList<QAudioDevice>::ConstIterator it = devices.constBegin(); it != devices.constEnd(); ++it ) {
            //qDebug() << "Capture device name = " << (*it).description();
            //qDebug() << (*it).supportedSampleFormats();
            //qDebug() << (*it).preferredFormat();
            //qDebug() << (*it).minimumSampleRate();
            //qDebug() << (*it).maximumSampleRate();

			if((*it).description() == m_inputdevice){
				device = *it;
			}
		}
		if (!device.isFormatSupported(format)) {
            qWarning() << "Current audio format not supported by capture device";
        }

		int sr = 8000;
		if(MACHAK){
			sr = device.preferredFormat().sampleRate();
			m_srm = (float)sr / 8000.0;
		}
		format.setSampleRate(sr);
        m_in = new QAudioSource(device, format, this);
        qDebug() << "Capture device: " <<  device.description() << " SR: " << sr << " resample factor: " << m_srm;
	}
}

void AudioEngine::start_capture()
{
	m_audioinq.clear();
	if(m_in != nullptr){
		m_indev = m_in->start();
		if(MACHAK) m_srm = (float)(m_in->format().sampleRate()) / 8000.0;
		connect(m_indev, SIGNAL(readyRead()), SLOT(input_data_received()));
	}
}

void AudioEngine::stop_capture()
{
	if(m_in != nullptr){
		m_indev->disconnect();
		m_in->stop();
	}
}

void AudioEngine::start_playback()
{
	if (m_out) {
		// Only start if stopped or suspended - IdleState and ActiveState mean already started
		if (m_out->state() == QAudio::StoppedState || m_out->state() == QAudio::SuspendedState) {
			memset(m_up_hist, 0, sizeof(m_up_hist));
			m_outdev = m_out->start();
		}
	}
}

void AudioEngine::stop_playback()
{
	memset(m_up_hist, 0, sizeof(m_up_hist));
	if (m_out) {
		//m_outdev->reset();
		m_out->reset();
		m_out->stop();
	}
}

void AudioEngine::input_data_received()
{
	QByteArray data = m_indev->readAll();

	if (data.size() > 0){
/*
		fprintf(stderr, "AUDIOIN: ");
		for(int i = 0; i < len; ++i){
			fprintf(stderr, "%02x ", (uint8_t)data.data()[i]);
		}
		fprintf(stderr, "\n");
		fflush(stderr);
*/
		if(MACHAK){
			std::vector<int16_t> samples;
			for(int i = 0; i < data.size(); i += 2){
				samples.push_back(((data.data()[i+1] << 8) & 0xff00) | (data.data()[i] & 0xff));
			}
			for(float i = 0; i < (float)data.size()/2; i += m_srm){
				m_audioinq.enqueue(samples[i]);
			}
		}
		else{
			for(int i = 0; i < data.size(); i += (2 * m_srm)){
				m_audioinq.enqueue(((data.data()[i+1] << 8) & 0xff00) | (data.data()[i] & 0xff));
			}
		}
	}
}

bool AudioEngine::playback_drained()
{
	if((m_out == nullptr) || (m_out->state() == QAudio::StoppedState)){
		return true;
	}
	return m_out->bytesFree() >= m_out->bufferSize();
}

void AudioEngine::upsample(const int16_t *in, size_t n, int16_t *out)
{
	// x = filter history followed by the new samples
	float x[UP_HIST + MAX_OUT_SAMPLES / 2];
	memcpy(x, m_up_hist, sizeof(m_up_hist));
	for(size_t i = 0; i < n; ++i){
		x[UP_HIST + i] = in[i];
	}

	for(size_t i = 0; i < n; ++i){
		const float *p = x + UP_HIST + i; // p[-k] is input sample i-k
		float even = 0.0f, odd = 0.0f;
		for(int k = 0; k <= UP_HIST; ++k){
			even += m_up_taps[2 * k] * p[-k];
		}
		for(int k = 0; k < UP_HIST; ++k){
			odd += m_up_taps[2 * k + 1] * p[-k];
		}
		for(int j = 0; j < 2; ++j){
			float v = std::floor(0.5f + (j ? odd : even));
			if(v > 32767.0f) v = 32767.0f;
			if(v < -32767.0f) v = -32767.0f;
			out[2 * i + j] = (int16_t)v;
		}
	}

	memcpy(m_up_hist, x + n, sizeof(m_up_hist));
}

void AudioEngine::write(int16_t *pcm, size_t s, float fade_from, float fade_to)
{
	// 8 kHz input: upsample to the 16 kHz output in blocks process_audio() can take
	const size_t max_in = MAX_OUT_SAMPLES / 2;
	int16_t wide[MAX_OUT_SAMPLES];
	m_maxlevel = 0;
	for(size_t a = 0; a < s; a += max_in){
		const size_t n = (s - a < max_in) ? (s - a) : max_in;
		upsample(pcm + a, n, wide);
		const float f0 = fade_from + (fade_to - fade_from) * (float)a / (float)s;
		const float f1 = fade_from + (fade_to - fade_from) * (float)(a + n) / (float)s;
		write_block(wide, 2 * n, f0, f1);
	}
}

void AudioEngine::write_wideband(int16_t *pcm, size_t s, float fade_from, float fade_to)
{
	// 16 kHz input goes straight out. Forget the interpolator history, so
	// switching back to 8 kHz audio does not replay old samples.
	memset(m_up_hist, 0, sizeof(m_up_hist));
	m_maxlevel = 0;
	for(size_t a = 0; a < s; a += MAX_OUT_SAMPLES){
		const size_t n = (s - a < (size_t)MAX_OUT_SAMPLES) ? (s - a) : (size_t)MAX_OUT_SAMPLES;
		const float f0 = fade_from + (fade_to - fade_from) * (float)a / (float)s;
		const float f1 = fade_from + (fade_to - fade_from) * (float)(a + n) / (float)s;
		write_block(pcm + a, n, f0, f1);
	}
}

void AudioEngine::write_block(int16_t *pcm, size_t s, float fade_from, float fade_to)
{
/*
	fprintf(stderr, "AUDIOOUT: ");
	for(int i = 0; i < s; ++i){
		fprintf(stderr, "%04x ", (uint16_t)pcm[i]);
	}
	fprintf(stderr, "\n");
	fflush(stderr);
*/
	if(m_agc){
		process_audio(pcm, s);
	}

	// Optional linear fade, applied after AGC so the AGC cannot undo it.
	if((fade_from != 1.0f) || (fade_to != 1.0f)){
		for(size_t i = 0; i < s; ++i){
			const float g = fade_from + (fade_to - fade_from) * (float)(i + 1) / (float)s;
			pcm[i] = static_cast<int16_t>(pcm[i] * g);
		}
	}

	size_t l = m_outdev->write((const char *) pcm, sizeof(int16_t) * s);

	if (l*2 < s){
		qDebug() << "AudioEngine::write() " << s << ":" << l << ":" << (int)m_out->bytesFree() << ":" << m_out->bufferSize() << ":" << m_out->error();
	}

	for(uint32_t i = 0; i < s; ++i){
		if(pcm[i] > m_maxlevel){
			m_maxlevel = pcm[i];
		}
	}
}

uint16_t AudioEngine::read(int16_t *pcm, int s)
{
	m_maxlevel = 0;

	if(m_audioinq.size() >= s){
		for(int i = 0; i < s; ++i){
			pcm[i] = m_audioinq.dequeue();
			if(pcm[i] > m_maxlevel){
				m_maxlevel = pcm[i];
			}
		}
		return 1;
	}
	else if(m_in == nullptr){
		memset(pcm, 0, sizeof(int16_t) * s);
		return 1;
	}
	else{
		return 0;
	}
}

uint16_t AudioEngine::read(int16_t *pcm)
{
	int s;
	m_maxlevel = 0;

	if(m_audioinq.size() >= 160){
		s = 160;
	}
	else{
		s = m_audioinq.size();
	}

	for(int i = 0; i < s; ++i){
		pcm[i] = m_audioinq.dequeue();
		if(pcm[i] > m_maxlevel){
			m_maxlevel = pcm[i];
		}
	}

	return s;
}

// process_audio() based on code from DSD https://github.com/szechyjs/dsd
void AudioEngine::process_audio(int16_t *pcm, size_t s)
{
	float aout_abs, max, gainfactor, gaindelta, maxbuf;

	for(size_t i = 0; i < s; ++i){
		m_audio_out_temp_buf[i] = static_cast<float>(pcm[i]);
	}

	// detect max level
	max = 0;
	m_audio_out_temp_buf_p = m_audio_out_temp_buf;

	for (size_t i = 0; i < s; i++){
		aout_abs = fabsf(*m_audio_out_temp_buf_p);

		if (aout_abs > max){
			max = aout_abs;
		}

		m_audio_out_temp_buf_p++;
	}

	*m_aout_max_buf_p = max;
	m_aout_max_buf_p++;
	m_aout_max_buf_idx++;

	if (m_aout_max_buf_idx > 24){
		m_aout_max_buf_idx = 0;
		m_aout_max_buf_p = m_aout_max_buf;
	}

	// lookup max history
	for (size_t i = 0; i < 25; i++){
		maxbuf = m_aout_max_buf[i];

		if (maxbuf > max){
			max = maxbuf;
		}
	}

	// determine optimal gain level
	if (max > static_cast<float>(0)){
		gainfactor = (static_cast<float>(30000) / max);
	}
	else{
		gainfactor = static_cast<float>(50);
	}

	if (gainfactor < m_aout_gain){
		m_aout_gain = gainfactor;
		gaindelta = static_cast<float>(0);
	}
	else{
		if (gainfactor > static_cast<float>(50)){
			gainfactor = static_cast<float>(50);
		}

		gaindelta = gainfactor - m_aout_gain;

		if (gaindelta > (static_cast<float>(0.05) * m_aout_gain)){
			gaindelta = (static_cast<float>(0.05) * m_aout_gain);
		}
	}

	gaindelta /= static_cast<float>(s); //160

	// adjust output gain
	m_audio_out_temp_buf_p = m_audio_out_temp_buf;

	for (size_t i = 0; i < s; i++){
		*m_audio_out_temp_buf_p = (m_aout_gain + (static_cast<float>(i) * gaindelta)) * (*m_audio_out_temp_buf_p);
		m_audio_out_temp_buf_p++;
	}

	m_aout_gain += (static_cast<float>(s) * gaindelta);
	m_audio_out_temp_buf_p = m_audio_out_temp_buf;

	for (size_t i = 0; i < s; i++){
		*m_audio_out_temp_buf_p *= m_volume;
		if (*m_audio_out_temp_buf_p > static_cast<float>(32760)){
			*m_audio_out_temp_buf_p = static_cast<float>(32760);
		}
		else if (*m_audio_out_temp_buf_p < static_cast<float>(-32760)){
			*m_audio_out_temp_buf_p = static_cast<float>(-32760);
		}
		pcm[i] = static_cast<int16_t>(*m_audio_out_temp_buf_p);
		m_audio_out_temp_buf_p++;
	}
}

void AudioEngine::handleStateChanged(QAudio::State newState)
{
	switch (newState) {
	case QAudio::ActiveState:
        //qDebug() << "AudioOut state active";
		break;
	case QAudio::SuspendedState:
        //qDebug() << "AudioOut state suspended";
		break;
	case QAudio::IdleState:
        //qDebug() << "AudioOut state idle";
		break;
	case QAudio::StoppedState:
        //qDebug() << "AudioOut state stopped";
		break;
	default:
		break;
	}
}
