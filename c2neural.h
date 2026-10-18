/*
	Copyright (C) 2026 DroidStar contributors

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

#ifndef C2NEURAL_H
#define C2NEURAL_H

#include <cstdint>

struct c2f_decoder;

/*
 * Neural (FARGAN) decoder for Codec2 3200 frames, from Codec2-mod-neural
 * (https://github.com/M17-Project/Codec2-mod-neural).
 *
 * It replaces codec2_decode() in 3200 bps mode: it takes one 8-byte frame and
 * returns 20 ms of speech at 16 kHz (320 samples), twice the bandwidth of the
 * classic decoder. Play it with AudioEngine::write_wideband().
 *
 * The output lags the classic decoder by 10 ms, and the first 10 ms of a
 * stream are silent.
 *
 * gain scales the output, followed by the same "ear protection" limiter as
 * the classic decoder, so both decoders can be matched in loudness.
 */
class C2Neural
{
public:
	static const int BYTES_PER_FRAME = 8;
	static const int SAMPLES_PER_FRAME = 320;
	static const int OUT_SAMPLE_RATE = 16000;

	explicit C2Neural(float gain = 1.0f);
	~C2Neural();
	C2Neural(const C2Neural &) = delete;
	C2Neural &operator=(const C2Neural &) = delete;

	bool valid() const { return m_dec != nullptr; }
	// Forget all history; call at the start of every transmission.
	void reset();
	// One Codec2 3200 frame (8 bytes) -> 320 samples at 16 kHz.
	void decode(int16_t *pcm, const uint8_t *bits);

private:
	c2f_decoder *m_dec;
	float m_gain;
};

#endif // C2NEURAL_H
