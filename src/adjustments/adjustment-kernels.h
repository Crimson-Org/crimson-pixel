#pragma once

#include "adjustments.h"
#include "color-lookup.h"
#include "color/color-spaces.h"

typedef enum {
	ADJUSTMENT_CURVES,
	ADJUSTMENT_HUE_SATURATION,
	ADJUSTMENT_VIBRANCE,
	ADJUSTMENT_SELECTIVE_COLOR,
	ADJUSTMENT_BLACK_WHITE,
	ADJUSTMENT_MIXER,
	ADJUSTMENT_REPLACE_COLOR,
	ADJUSTMENT_COLOR_LOOKUP
} AdjustmentKind;

#define ADJUSTMENT_CURVE_LIMIT 1024

/* Hue map entries hold the hue, saturation and lightness change and an unused place */
#define HUE_MAP_STRIDE 4
enum { HUE_MAP_HUE, HUE_MAP_SATURATION, HUE_MAP_LIGHTNESS };
/* The mixer keeps red, green, blue and the constant for each output */
#define MIXER_PARTS 4
/* Selective color keeps a multiplier for cyan, magenta and yellow, then an offset for each */
#define SELECTIVE_INKS_MIXED 3
#define SELECTIVE_ENTRY (2 * SELECTIVE_INKS_MIXED)
/* Vibrance works in linear light, with this gamma */
#define VIBRANCE_GAMMA 2.4

typedef struct {
	AdjustmentKind kind;
	/* Curves keep three single precision curves, applied to the luminance when luminance is set */
	float red[ADJUSTMENT_CURVE_LIMIT], green[ADJUSTMENT_CURVE_LIMIT], blue[ADJUSTMENT_CURVE_LIMIT];
	int curve_length;
	/* Photo Filter can keep the luminance of each pixel */
	gboolean luminance, preserve_luminosity;
	guint8 lookup[RGB_CHANNELS][CURVE_LEVELS];
	double luminance_terms[RGB_CHANNELS * CURVE_LEVELS], sampled_curves[RGB_CHANNELS * CURVE_LEVELS];
	/* Hue and saturation maps each hue level to single precision hue, saturation and lightness changes */
	float hue_map[CURVE_LEVELS * HUE_MAP_STRIDE];
	double hue_lift, hue_keep;
	const double *saturation_stretches;
	gboolean colorize;
	/* Vibrance converts linear light to YUV, scales it and converts back */
	double linear[CURVE_LEVELS], to_yuv[MATRIX_VALUES], from_yuv[MATRIX_VALUES], vibrance_lightness, vibrance_chroma, vibrance_saturation;
	/* The channel mixer mixes red, green, blue and a constant into each channel */
	double mixer[RGB_CHANNELS * MIXER_PARTS];
	/* Black and white weighs six hue ranges into a gray, which a tint colors through its hue and strength */
	double gray_weights[GRAY_WEIGHTS_COUNT], tint_hue, tint_middle, tint_strength, tint_shadows, tint_highlights;
	gboolean tinted;
	/* Selective color keeps single precision multipliers and offsets of cyan, magenta and yellow for each of the
	 * nine ranges */
	float selective[SELECTIVE_RANGES_COUNT * SELECTIVE_ENTRY];
	/* Replace color shifts hue, saturation and lightness of colors near a Lab range */
	double replace_low[RGB_CHANNELS], replace_high[RGB_CHANNELS], replace_shift[RGB_CHANNELS], replace_fuzziness;
	/* Color lookup keeps the profile sampled on the lookup grid */
	double color_lookup[COLOR_LOOKUP_SAMPLES];
} Adjustment;

gboolean build_adjustment(const char *key, const Record *descriptor, Adjustment *adjustment);

void prepare_adjustment(Adjustment *adjustment);

/* Recent colors and their results for kernels that work on whole colors, kept by one thread over many rows */
#define ADJUSTMENT_CACHE_BITS 12

typedef struct {
	const Adjustment *owner;
	guint32 keys[1 << ADJUSTMENT_CACHE_BITS], colors[1 << ADJUSTMENT_CACHE_BITS];
} AdjustmentCache;

void apply_adjustment(const Adjustment *adjustment, AdjustmentCache *cache, guint8 *pixels, gsize length);
