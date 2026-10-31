#pragma once

#include "adjustments.h"
#include "color-lookup.h"

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

typedef struct {
	AdjustmentKind kind;
	/* X6 keeps three Float32 curves, applied to the luminance when luminance is set like Wz */
	float red[ADJUSTMENT_CURVE_LIMIT], green[ADJUSTMENT_CURVE_LIMIT], blue[ADJUSTMENT_CURVE_LIMIT];
	int curve_length;
	/* Photo Filter keeps the luminance of each pixel like tU */
	gboolean luminance, preserve_luminosity;
	guint8 lookup[3][256];
	double luminance_terms[3 * 256], sampled_curves[3 * 256];
	/* rk maps each hue level to Float32 hue, saturation and lightness changes */
	float hue_map[256 * 4];
	double hue_lift, hue_keep;
	const double *saturation_stretches;
	gboolean colorize;
	/* IK converts linear light to YUV, scales it and converts back */
	double linear[256], to_yuv[16], from_yuv[16], vibrance_lightness, vibrance_chroma, vibrance_saturation;
	/* j_ mixes red, green, blue and a constant into each channel */
	double mixer[12];
	/* jg weighs six hue ranges into a gray, which a tint colors through its hue and strength */
	double gray_weights[6], tint_hue, tint_middle, tint_strength, tint_shadows, tint_highlights;
	gboolean tinted;
	/* w8 keeps Float32 multipliers and offsets of cyan, magenta and yellow for each of the nine ranges */
	float selective[9 * 6];
	/* pP shifts hue, saturation and lightness of colors near a Lab range */
	double replace_low[3], replace_high[3], replace_shift[3], replace_fuzziness;
	/* jy keeps the profile sampled on the lookup grid */
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
