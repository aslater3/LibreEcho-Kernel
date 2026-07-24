/*
 * Low-cost audio spectrum envelope for the MT8163 Cortex-A7.
 *
 * Twelve overlapping constant-skirt-gain band-pass filters cover the useful
 * speaker programme range on a roughly logarithmic scale.  Coefficients are
 * precomputed for 48 kHz, Q=2.0 and stored as signed Q28 values, so the hot
 * path performs no floating-point work.  Each input sample costs three
 * bounded 64-bit multiplies per band.
 */
#include "audio_visualizer.h"

#include <limits.h>
#include <string.h>

#define COEFFICIENT_SHIFT 28
#define FILTER_INPUT_SHIFT 8
#define INITIAL_NOISE_FLOOR 8U
#define LEVEL_FLOOR_LOG2_Q8 (3U << 8)
#define LEVEL_RANGE_LOG2_Q8 (11U << 8)

struct band_coefficients {
	int32_t b0;
	int32_t a1;
	int32_t a2;
};

/*
 * Band centres: 63, 100, 160, 250, 400, 630, 1000, 1600, 2500,
 * 4000, 6500 and 11000 Hz.  b2 is always -b0 and b1 is zero.
 */
static const struct band_coefficients coefficients[AUDIO_VISUALIZER_BANDS] = {
	{   552280, 535748133, -267330895 },
	{   875563, 535073942, -266684331 },
	{  1398102, 533957576, -265639252 },
	{  2177926, 532229946, -264079605 },
	{  3466846, 529210959, -261501763 },
	{  5416440, 524250312, -257602575 },
	{  8482662, 515457723, -251470132 },
	{ 13263318, 499192030, -241908821 },
	{ 19966900, 470564724, -228501656 },
	{ 29826162, 413283421, -208783132 },
	{ 42472068, 297976029, -183491321 },
	{ 53319021,  56156658, -161797414 },
};

static int32_t clamp_i64_to_i32(int64_t value)
{
	if (value > INT32_MAX)
		return INT32_MAX;
	if (value < INT32_MIN)
		return INT32_MIN;
	return (int32_t)value;
}

static int64_t round_q28(int64_t value)
{
	const int64_t half = (int64_t)1 << (COEFFICIENT_SHIFT - 1);

	if (value >= 0)
		return (value + half) >> COEFFICIENT_SHIFT;
	return -((-value + half) >> COEFFICIENT_SHIFT);
}

static uint32_t magnitude_i32(int32_t value)
{
	if (value >= 0)
		return (uint32_t)value;
	if (value == INT32_MIN)
		return (uint32_t)INT32_MAX + 1U;
	return (uint32_t)-value;
}

/* A monotonic, division-only Q8 log2 approximation is sufficient for LED
 * levels.  Its fractional term is linear within each octave. */
static uint32_t log2_q8(uint32_t value)
{
	uint32_t base = 1;
	uint32_t whole = 0;

	if (value == 0)
		return 0;
	while (value >= (base << 1) && whole < 30U) {
		base <<= 1;
		++whole;
	}
	return (whole << 8) + (((value - base) << 8) / base);
}

static uint8_t normalize_level(struct audio_visualizer_band *band,
			       uint32_t magnitude)
{
	uint32_t gated;
	uint32_t logarithmic;
	uint32_t raw;
	uint32_t difference;

	/*
	 * The floor follows falling energy in about 0.3 seconds, but rises only
	 * for a sustained signal substantially above it.  This removes fixed
	 * point/filter residue without teaching the floor to swallow music.
	 */
	if (magnitude < band->noise_floor) {
		difference = band->noise_floor - magnitude;
		band->noise_floor -= (difference + 7U) / 8U;
	} else {
		difference = magnitude - band->noise_floor;
		band->noise_floor += difference / 4096U;
	}
	if (magnitude <= band->noise_floor + INITIAL_NOISE_FLOOR)
		gated = 0;
	else
		gated = magnitude - band->noise_floor;

	logarithmic = log2_q8(gated);
	if (logarithmic <= LEVEL_FLOOR_LOG2_Q8) {
		raw = 0;
	} else {
		raw = ((logarithmic - LEVEL_FLOOR_LOG2_Q8) * 255U) /
			LEVEL_RANGE_LOG2_Q8;
		if (raw > 255U)
			raw = 255U;
	}

	/*
	 * Fast attack and roughly 100 ms release at 2048 frames/period.  The LED
	 * daemon performs a final small amount of display smoothing, so keeping a
	 * long envelope here makes percussion visibly trail the music.
	 */
	if (raw > band->level) {
		difference = raw - band->level;
		band->level = (uint8_t)(band->level +
			(3U * difference + 3U) / 4U);
	} else if (raw < band->level) {
		difference = band->level - raw;
		band->level = (uint8_t)(band->level -
			(difference + 2U) / 3U);
	}
	return band->level;
}

void audio_visualizer_init(struct audio_visualizer *visualizer)
{
	unsigned int band;

	memset(visualizer, 0, sizeof(*visualizer));
	for (band = 0; band < AUDIO_VISUALIZER_BANDS; ++band)
		visualizer->bands[band].noise_floor = INITIAL_NOISE_FLOOR;
}

void audio_visualizer_reset(struct audio_visualizer *visualizer)
{
	audio_visualizer_init(visualizer);
}

void audio_visualizer_process(struct audio_visualizer *visualizer,
			      const int16_t *samples, size_t frames,
			      size_t stride,
			      uint8_t levels[AUDIO_VISUALIZER_BANDS])
{
	uint64_t sums[AUDIO_VISUALIZER_BANDS] = { 0 };
	size_t frame;
	unsigned int band;

	if (!visualizer || !samples || !levels || frames == 0 || stride == 0)
		return;

	for (frame = 0; frame < frames; ++frame) {
		int32_t input =
			(int32_t)samples[frame * stride] << FILTER_INPUT_SHIFT;
		int32_t delayed_input = visualizer->input_2;

		for (band = 0; band < AUDIO_VISUALIZER_BANDS; ++band) {
			struct audio_visualizer_band *state =
				&visualizer->bands[band];
			const struct band_coefficients *filter =
				&coefficients[band];
			int64_t accumulator =
				(int64_t)filter->b0 *
					(input - delayed_input) +
				(int64_t)filter->a1 * state->output_1 +
				(int64_t)filter->a2 * state->output_2;
			int32_t output = clamp_i64_to_i32(
				round_q28(accumulator));

			state->output_2 = state->output_1;
			state->output_1 = output;
			sums[band] += magnitude_i32(output);
		}
		visualizer->input_2 = visualizer->input_1;
		visualizer->input_1 = input;
	}

	for (band = 0; band < AUDIO_VISUALIZER_BANDS; ++band) {
		uint32_t magnitude = (uint32_t)(
			(sums[band] / frames) >> FILTER_INPUT_SHIFT);

		levels[band] = normalize_level(&visualizer->bands[band],
					      magnitude);
	}
}
