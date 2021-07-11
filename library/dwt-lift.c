/*-----------------------------

MIT License

Copyright (c) 2021 Alexander Brandt

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

-------------------------------

 [dwt-lift.c]
 - Alexander Brandt 2021
-----------------------------*/

#include <assert.h>
#include <math.h>
#include <string.h>

#include "ako.h"
#include "dwt.h"
#include "misc.h"


extern void DevPrintf(const char* format, ...);
extern void DevSaveGrayPgm(size_t width, size_t height, size_t in_pitch, const int16_t* data,
                           const char* filename_format, ...);


static void sLift1d(int16_t q, float g, size_t len, const int16_t* in, int16_t* out)
{
	// Highpass
	// Haar:  hp[i] = odd[i] - (even[i] + odd[i]) / 2
	// CDF53: hp[i] = odd[i] - (even[i] + even[i + 1]) / 2
	// 97DD:  hp[i] = odd[i] - (-(even[i - 1] + even[i + 2]) + 9 * (even[i] + even[i + 1])) / 16

#if (AKO_WAVELET == 1)
	// Haar
	for (size_t i = 0; i < len; i++)
		out[len + i] = in[(i * 2) + 1] - ((in[(i * 2)] + in[(i * 2) + 1]) / 2);
#endif

#if (AKO_WAVELET == 2)
	// CDF53
	for (size_t i = 0; i < len; i++)
	{
		if (i < len - 2)
			out[len + i] = in[(i * 2) + 1] - ((in[(i * 2)] + in[(i * 2) + 2]) / 2);
		else
			out[len + i] = in[(i * 2) + 1] - ((in[(i * 2)] + in[(i * 2)]) / 2); // Fake last value
	}
#endif

#if (AKO_WAVELET == 3)
	// 97DD
	if (len > 4) // Needs 4 samples
	{
		for (size_t i = 0; i < len; i++)
		{
			int16_t even_i = in[(i * 2)];
			int16_t even_il1 = (i >= 1) ? in[(i * 2) - 2] : even_i;
			int16_t even_ip1 = (i <= len - 2) ? in[(i * 2) + 2] : even_i;
			int16_t even_ip2 = (i <= len - 4) ? in[(i * 2) + 4] : even_ip1;

			out[len + i] = in[(i * 2) + 1] - (-(even_il1 + even_ip2) + 9 * (even_i + even_ip1)) / 16;
		}
	}
	else // Fallback to CDF53
	{
		for (size_t i = 0; i < len; i++)
		{
			if (i < len - 2)
				out[len + i] = in[(i * 2) + 1] - ((in[(i * 2)] + in[(i * 2) + 2]) / 2);
			else
				out[len + i] = in[(i * 2) + 1] - ((in[(i * 2)] + in[(i * 2)]) / 2); // Fake last value
		}
	}
#endif

	// Lowpass
	// CDF53, 97DD: lp[i] = even[i] + (hp[i] + hp[i - 1]) / 4
	// Haar:        lp[i] = (even[i] + odd[i]) / 2
	for (size_t i = 0; i < len; i++)
	{
#if (AKO_WAVELET != 1)
		// CD53, 97DD
		if (i > 0)
			out[i] = in[(i * 2)] + ((out[len + i] + out[len + i - 1]) / 4);
		else
			out[i] = in[(i * 2)] + ((out[len + i] + out[len + i]) / 4); // Fake first value
#else
		// Haar
		out[i] = (in[(i * 2)] + in[(i * 2) + 1]) / 2;
#endif
	}

	// Degrade highpass
	for (size_t i = 0; i < len; i++)
	{
		// Noise gate
		const float value = (float)out[len + i];
		if ((value / (float)q) > (-g / (float)q) && (value / (float)q) < (g / (float)q))
			out[len + i] = 0;

		// Quantization
		out[len + i] = out[len + i] / q;
	}
}


static inline void s2dToLinearH(size_t w, size_t h, size_t in_pitch, const int16_t* in, int16_t* out)
{
	for (size_t r = 0; r < h; r++)
	{
		for (size_t c = 0; c < w; c++)
		{
			*out = in[(r * in_pitch) + c];
			out = out + 1;
		}
	}
}


static inline void s2dToLinearV(size_t w, size_t h, size_t in_pitch, const int16_t* in, int16_t* out)
{
	for (size_t c = 0; c < w; c++)
	{
		for (size_t r = 0; r < h; r++)
		{
			*out = in[c + (r * in_pitch)];
			out = out + 1;
		}
	}
}


static inline void sLift2d(size_t ch, int16_t q, float g, size_t current_w, size_t current_h, size_t target_w,
                           size_t target_h, size_t in_pitch, int16_t* aux, int16_t* a, int16_t* b)
{
	// Rows (from buffer A to B)
	{
		const size_t fake_last_col = (target_w * 2) - current_w;
		const size_t fake_last_row = (target_h * 2) - current_h;

		int16_t* in = a;
		int16_t* out = b;

		for (size_t row = 0; row < current_h; row++)
		{
			int16_t* temp = aux;
			for (size_t i = 0; i < current_w; i++)
				temp[i] = in[i];
			if (fake_last_col != 0)
				temp[(target_w * 2) - 1] = in[current_w - 1];

			sLift1d(q, g, target_w, aux, out);

			out = out + (target_w * 2);
			in = in + current_w + in_pitch;
		}

		if (fake_last_row != 0)
			memcpy(out, out - (target_w * 2), sizeof(int16_t) * (target_w * 2));
	}

	// Columns (from buffer B to A)
	for (size_t col = 0; col < (target_w * 2); col++)
	{
		int16_t* temp = b + col;
		for (size_t i = 0; i < (target_h * 2); i++)
		{
			aux[i] = *temp;
			temp = temp + (target_w * 2);
		}

		sLift1d(q, g, target_h, aux, aux + (target_h * 2));

		temp = a + col;
		for (size_t i = 0; i < (target_h * 2); i++)
		{
			*temp = aux[(target_h * 2) + i];
			temp = temp + (target_w * 2);
		}
	}
}


void DwtTransform(const struct AkoSettings* s, size_t tile_w, size_t tile_h, size_t channels, size_t planes_space,
                  int16_t* aux_memory, int16_t* in, int16_t* out)
{
#if (AKO_WAVELET == 0)
	for (size_t ch = 0; ch < channels; ch++)
	{
		int16_t* temp = in + ((tile_w * tile_h) + planes_space) * ch;
		memcpy(out, temp, sizeof(int16_t) * tile_w * tile_h);
		out = out + tile_w * tile_h;
	}
	return;
#endif

	size_t lift = 0;

	size_t current_w = tile_w;
	size_t current_h = tile_h;
	size_t target_w = tile_w;
	size_t target_h = tile_h;

	int16_t* output_cursor = out + TileTotalLength(tile_w, tile_h) * channels; // Output end

	const size_t total_lifts = TileTotalLifts(tile_w, tile_h);
	float q = 0.0f;
	float g = 0.0f;

	// Highpasses
	while (target_w > 2 && target_h > 2)
	{
		current_w = target_w;
		current_h = target_h;
		target_w = (target_w % 2 == 0) ? (target_w / 2) : ((target_w + 1) / 2);
		target_h = (target_h % 2 == 0) ? (target_h / 2) : ((target_h + 1) / 2);

		// We are doing it in reverse, the decoder wants YUV channels in such order,
		// and for us (the encoder) this mean to proccess them as VUY...
		for (size_t ch = (channels - 1); ch < channels; ch--) // ... and yes, underflows
		{
			// Quantization/Noise gate
			{
				float user_g = s->noise_gate[ch];
				float user_q = s->quantization[ch];
				user_g = (user_g < 0.0f) ? 0.0f : user_g;
				user_q = (user_q < 1.0f) ? 1.0f : user_q;

				// Mk1
				float mk1_q = user_q;
				float mk1_g = user_g;
				for (size_t i = 0; i < lift; i++)
				{
					mk1_q = mk1_q / 2.0f;
					mk1_g = mk1_g / 2.0f;
				}

				// Mk3
				float mk3_q = powf(user_q, 1.0f - (float)lift / ((float)total_lifts - 1.0f));
				float mk3_g = powf(user_g + 1.0f, 1.0f - (float)lift / ((float)total_lifts - 1.0f)) - 1.0f;

				// Some cautions
				q = mk1_q;
				g = mk3_g;
				q = (q < 1.0f) ? 1.0 : q;
				g = (g < 0.0f) ? 0.0 : g;
			}

			// Lift
			int16_t* lp = in + ((tile_w * tile_h) + planes_space) * ch;

			if (lift == 0)
			{
				// The first lift needs a buffer 'b' of the same size than the input,
				// and we don't have one, but right now 'out' is empty
				int16_t* buffer_b = out + (tile_w * tile_h) * ch;
				sLift2d(ch, (int16_t)q, g, tile_w, tile_h, target_w, target_h, 0, aux_memory, lp, buffer_b);
			}
			else
			{
				// Following lifts are one quarter of size, thus they fit in the last
				// half of the input
				sLift2d(ch, (int16_t)q, g, current_w, current_h, target_w, target_h, current_w, aux_memory, lp,
				        lp + (current_w * current_h * 2));
			}

			// Developers, developers, developers
			DevSaveGrayPgm(target_w * 2, target_h * 2, target_w * 2, lp, "/tmp/lift-ch%zu-%zu.pgm", ch, lift);

			if (ch == 0)
			{
				//	DevPrintf("###\t - Lift %zu, %zux%zu <- %zux%zu px\n", lift, target_w, target_h, current_w,
				// current_h);
				// DevPrintf("###\t - Lift %zu, q: %.2f, g: %.2f\n", lift, q, g);
			}

			// Emit
			// Up to here the only thing we did was to modify 'lp', now it holds four
			// quadrants: LP, B, C and D, where the last three are highpass coefficients
			// (in classic nomeclanture those highpasses are HL, LH and HH)

			// We need to output the highpasses, LP remains in place for the following
			// lift step

			output_cursor = output_cursor - (target_w * target_h) * 3; // Three highpasses...

			s2dToLinearV(target_w, target_h, (target_w * 2),
			             lp + target_w * target_h * 2,               //
			             output_cursor + (target_w * target_h) * 0); // C

			s2dToLinearV(target_w, target_h, (target_w * 2),
			             lp + target_w,                              //
			             output_cursor + (target_w * target_h) * 1); // B

			s2dToLinearV(target_w, target_h, (target_w * 2),
			             lp + target_w + target_h * (target_w * 2),  //
			             output_cursor + (target_w * target_h) * 2); // D

			output_cursor = output_cursor - 1; // One head...
			*output_cursor = (int16_t)q;
		}

		lift = lift + 1;
	}

	// Lowpasses (one per-channel)
	for (size_t ch = (channels - 1); ch < channels; ch--)
	{
		output_cursor = output_cursor - (target_w * target_h); // ..And one lowpass

		int16_t* lp = in + ((tile_w * tile_h) + planes_space) * ch;
		s2dToLinearH(target_w, target_h, (target_w * 2), lp, output_cursor);
	}

	assert((output_cursor - out) == 0);
}
