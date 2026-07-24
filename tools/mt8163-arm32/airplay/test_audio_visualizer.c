#define _DEFAULT_SOURCE

#include "audio_visualizer.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_FRAMES 2048U
#define TEST_PI 3.14159265358979323846

static const unsigned int centres[AUDIO_VISUALIZER_BANDS] = {
	63, 100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6500, 11000
};

static void make_tone(int16_t *samples, unsigned int frequency,
		      unsigned int period, int amplitude)
{
	size_t frame;
	size_t offset = (size_t)period * TEST_FRAMES;

	for (frame = 0; frame < TEST_FRAMES; ++frame) {
		double phase = 2.0 * TEST_PI * frequency *
			(offset + frame) / AUDIO_VISUALIZER_RATE;

		samples[frame] = (int16_t)lround(sin(phase) * amplitude);
	}
}

static unsigned int strongest_band(const uint8_t *levels)
{
	unsigned int strongest = 0;
	unsigned int band;

	for (band = 1; band < AUDIO_VISUALIZER_BANDS; ++band)
		if (levels[band] > levels[strongest])
			strongest = band;
	return strongest;
}

static int test_silence(void)
{
	struct audio_visualizer visualizer;
	int16_t samples[TEST_FRAMES] = { 0 };
	uint8_t levels[AUDIO_VISUALIZER_BANDS];
	unsigned int period;
	unsigned int band;

	audio_visualizer_init(&visualizer);
	memset(levels, 0xff, sizeof(levels));
	for (period = 0; period < 8; ++period)
		audio_visualizer_process(&visualizer, samples, TEST_FRAMES, 1,
					 levels);
	for (band = 0; band < AUDIO_VISUALIZER_BANDS; ++band)
		if (levels[band] != 0) {
			fprintf(stderr, "silence leaked into band %u: %u\n",
				band, levels[band]);
			return 1;
		}
	return 0;
}

static int test_band_centres(void)
{
	struct audio_visualizer visualizer;
	int16_t samples[TEST_FRAMES];
	uint8_t levels[AUDIO_VISUALIZER_BANDS];
	unsigned int expected;
	unsigned int period;
	unsigned int strongest;

	for (expected = 0; expected < AUDIO_VISUALIZER_BANDS; ++expected) {
		audio_visualizer_init(&visualizer);
		memset(levels, 0, sizeof(levels));
		for (period = 0; period < 8; ++period) {
			make_tone(samples, centres[expected], period, 12000);
			audio_visualizer_process(&visualizer, samples,
						 TEST_FRAMES, 1, levels);
		}
		strongest = strongest_band(levels);
		if ((strongest + 1U < expected || strongest > expected + 1U) ||
		    levels[expected] < 140) {
			unsigned int band;

			fprintf(stderr,
				"%u Hz selected band %u level %u, expected %u\n",
				centres[expected], strongest, levels[strongest],
				expected);
			for (band = 0; band < AUDIO_VISUALIZER_BANDS; ++band)
				fprintf(stderr, " %u", levels[band]);
			fputc('\n', stderr);
			return 1;
		}
	}
	return 0;
}

static int test_attack_and_decay(void)
{
	struct audio_visualizer visualizer;
	int16_t samples[TEST_FRAMES];
	uint8_t levels[AUDIO_VISUALIZER_BANDS] = { 0 };
	uint8_t peak;
	unsigned int period;

	audio_visualizer_init(&visualizer);
	make_tone(samples, 1000, 0, 8000);
	audio_visualizer_process(&visualizer, samples, TEST_FRAMES, 1, levels);
	peak = levels[6];
	if (peak < 100) {
		fprintf(stderr, "attack too slow: %u\n", peak);
		return 1;
	}
	memset(samples, 0, sizeof(samples));
	audio_visualizer_process(&visualizer, samples, TEST_FRAMES, 1, levels);
	if (levels[6] == 0 || levels[6] >= peak) {
		fprintf(stderr, "decay is not gradual: %u -> %u\n",
			peak, levels[6]);
		return 1;
	}
	if (levels[6] > (unsigned int)peak * 17U / 20U) {
		fprintf(stderr, "decay is too slow for a reactive display: %u -> %u\n",
			peak, levels[6]);
		return 1;
	}
	for (period = 0; period < 96; ++period)
		audio_visualizer_process(&visualizer, samples, TEST_FRAMES, 1,
					 levels);
	if (levels[6] != 0) {
		fprintf(stderr, "decay did not reach zero: %u\n", levels[6]);
		return 1;
	}
	return 0;
}

int main(void)
{
	if (test_silence() || test_band_centres() || test_attack_and_decay())
		return 1;
	puts("audio visualizer analyzer: ok");
	return 0;
}
