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

#include <cmath>
#include <cstring>
#include "c2neural.h"
#include "c2fargan.h"

static_assert(C2F_BYTES_PER_FRAME == C2Neural::BYTES_PER_FRAME, "unexpected Codec2-mod-neural frame size");
static_assert(C2F_SAMPLES_PER_FRAME == C2Neural::SAMPLES_PER_FRAME, "unexpected Codec2-mod-neural output size");
static_assert(C2F_SAMPLE_RATE == C2Neural::OUT_SAMPLE_RATE, "unexpected Codec2-mod-neural sample rate");

C2Neural::C2Neural(float gain) :
	m_dec(c2f_create()),
	m_gain(gain)
{
}

C2Neural::~C2Neural()
{
	if(m_dec){
		c2f_destroy(m_dec);
	}
}

void C2Neural::reset()
{
	if(m_dec){
		c2f_reset(m_dec);
	}
}

void C2Neural::decode(int16_t *pcm, const uint8_t *bits)
{
	if(!m_dec){
		std::memset(pcm, 0, SAMPLES_PER_FRAME * sizeof(int16_t));
		return;
	}

	float out[SAMPLES_PER_FRAME];
	c2f_decode_float(m_dec, out, bits);

	float max_sample = 0.0f;
	for(int i = 0; i < SAMPLES_PER_FRAME; ++i){
		out[i] *= 32768.0f * m_gain;
		if(out[i] > max_sample){
			max_sample = out[i];
		}
	}

	// Ear protection as in the classic decoder: if the frame is x dB over
	// the set point, reduce it by 2x dB.
	const float over = max_sample / 30000.0f;
	const float limit = (over > 1.0f) ? 1.0f / (over * over) : 1.0f;

	for(int i = 0; i < SAMPLES_PER_FRAME; ++i){
		float s = std::floor(0.5f + out[i] * limit);
		if(s > 32767.0f) s = 32767.0f;
		if(s < -32767.0f) s = -32767.0f;
		pcm[i] = (int16_t)s;
	}
}
