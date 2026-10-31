#include <limits.h>
#include <math.h>
#include <string.h>

#include "adjustment-kernels.h"
#include "color/color-spaces.h"

/* ============================== Tables ========================================== */

static guint8 round_to_byte(float value) {
	if (isnan(value))
		return 0;
	return MIN(G_MAXUINT8, (gint32)(0.5 + G_MAXUINT8 * (double)value));
}

static double sample_curve(const float *curve, int length, double position) {
	int index = (int)position;
	double fraction = position - index;
	return curve[index] * (1 - fraction) + curve[index + (index == length - 1 ? 0 : 1)] * fraction;
}

static void fill_curve_lookups(Adjustment *adjustment) {
	const float *curves[] = {adjustment->red, adjustment->green, adjustment->blue};
	double last = (adjustment->curve_length - 1) * CURVE_END_PULL, unit = BYTE_UNIT;
	const double weights[RGB_CHANNELS] = {LUMA_RED, LUMA_GREEN, LUMA_BLUE};
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		for (int index = 0; index < CURVE_LEVELS; index++) {
			adjustment->luminance_terms[channel * CURVE_LEVELS + index] = index * unit * weights[channel];
			adjustment->sampled_curves[channel * CURVE_LEVELS + index] =
				sample_curve(curves[channel], adjustment->curve_length, last * (index * unit));
		}
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		for (int index = 0; index < CURVE_LEVELS; index++)
			adjustment->lookup[channel][index] =
				adjustment->curve_length == CURVE_LEVELS
					? round_to_byte(curves[channel][index])
					: wrap_byte(unit_to_byte(sample_curve(curves[channel], adjustment->curve_length, last * (index * unit))));
}

/* The gamma encoding at the end of vibrance, wrap_byte(pow(value, 1 / VIBRANCE_GAMMA) * 255), never falls as the value
 * grows, so
 * each byte starts at a fixed value; these thresholds are found once by bisecting the real formula, which keeps
 * the result exact while a pixel needs a short search instead of a power */
static double encoding_thresholds[CURVE_LEVELS];

/* The byte at the start of each of these equal steps of the value, from which the search rarely moves */
enum { ENCODING_BUCKETS = 1 << 16 };

static guint8 encoding_starts[ENCODING_BUCKETS + 1];

static guint8 encode_gamma_slowly(double value) {
	return wrap_byte(unit_to_byte(pow(value, 1 / VIBRANCE_GAMMA)));
}

static void find_encoding_thresholds(void) {
	static gsize ready;
	if (!g_once_init_enter(&ready))
		return;
	encoding_thresholds[0] = 0;
	for (int level = 1; level < CURVE_LEVELS; level++) {
		double low = encoding_thresholds[level - 1], high = 1;
		while (nextafter(low, INFINITY) < high) {
			double middle = low + (high - low) / 2;
			if (middle <= low || middle >= high)
				middle = nextafter(low, INFINITY);
			if (encode_gamma_slowly(middle) >= level)
				high = middle;
			else
				low = middle;
		}
		encoding_thresholds[level] = high;
	}
	int level = 0;
	for (int bucket = 0; bucket <= ENCODING_BUCKETS; bucket++) {
		while (level < G_MAXUINT8 && (double)bucket / ENCODING_BUCKETS >= encoding_thresholds[level + 1])
			level++;
		encoding_starts[bucket] = level;
	}
	g_once_init_leave(&ready, 1);
}

static guint8 encode_gamma(double value) {
	if (isnan(value))
		return 0;
	/* Scaling by a power of two is exact, so the bucket never starts above the value */
	int level = encoding_starts[(int)(value * ENCODING_BUCKETS)];
	while (level < G_MAXUINT8 && value >= encoding_thresholds[level + 1])
		level++;
	return level;
}

void prepare_adjustment(Adjustment *adjustment) {
	if (adjustment->kind == ADJUSTMENT_CURVES)
		fill_curve_lookups(adjustment);
	if (adjustment->kind == ADJUSTMENT_VIBRANCE)
		find_encoding_thresholds();
}

/* ============================== Applying ======================================== */

static void sample_curves_at(const Adjustment *adjustment, double position, double channels[RGB_CHANNELS]) {
	const int count = adjustment->curve_length;
	int index = (int)position;
	double fraction = position - index;
	int next = index + (index == count - 1 ? 0 : 1);
	channels[0] = adjustment->red[index] * (1 - fraction) + adjustment->red[next] * fraction;
	channels[1] = adjustment->green[index] * (1 - fraction) + adjustment->green[next] * fraction;
	channels[2] = adjustment->blue[index] * (1 - fraction) + adjustment->blue[next] * fraction;
}

static void transform_luminance_curves(const Adjustment *adjustment, guint8 *pixel) {
	const double *terms = adjustment->luminance_terms;
	double luminance = terms[pixel[0]] + terms[CURVE_LEVELS + pixel[1]] + terms[2 * CURVE_LEVELS + pixel[2]];
	double channels[RGB_CHANNELS];
	sample_curves_at(adjustment, (adjustment->curve_length - 1) * CURVE_END_PULL * luminance, channels);
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		pixel[channel] = wrap_byte(unit_to_byte(channels[channel]));
}

static void transform_preserving_curves(const Adjustment *adjustment, guint8 *pixel) {
	const double unit = BYTE_UNIT, *sampled = adjustment->sampled_curves;
	double red = pixel[0] * unit, green = pixel[1] * unit, blue = pixel[2] * unit;
	double channels[RGB_CHANNELS] = {sampled[pixel[0]], sampled[CURVE_LEVELS + pixel[1]],
									 sampled[2 * CURVE_LEVELS + pixel[2]]};
	double before = get_luma(red, green, blue);
	double after = get_luma(channels[0], channels[1], channels[2]);
	if (before > after) {
		double lift = (before - after) / (1 - after);
		for (int channel = 0; channel < RGB_CHANNELS; channel++)
			channels[channel] += lift * (1 - channels[channel]);
	} else if (after == 0)
		channels[0] = channels[1] = channels[2] = 0;
	else {
		double scale = before / after;
		for (int channel = 0; channel < RGB_CHANNELS; channel++)
			channels[channel] = scale * channels[channel];
	}
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		pixel[channel] = wrap_byte(unit_to_byte(channels[channel]));
}

static void transform_black_white(const Adjustment *adjustment, guint8 *pixel) {
	const double unit = BYTE_UNIT, strength = adjustment->tint_strength, middle = adjustment->tint_middle;
	const double shadows = adjustment->tint_shadows, highlights = adjustment->tint_highlights;
	double hsl[RGB_CHANNELS], change = 0, rgb[RGB_CHANNELS];
	convert_rgb_to_hsl(pixel[0] * unit, pixel[1] * unit, pixel[2] * unit, hsl);
	for (int range = 0; range < GRAY_WEIGHTS_COUNT; range++)
		change += MIN(1, 1.7 * (1 - hsl[2])) * hsl[1] * adjustment->gray_weights[range] *
				  weigh_hue_range(hsl[0], range * (1.0 / HUE_SECTORS));
	double gray = MAX(0, MIN(1, hsl[2] * (1 + change)));
	if (adjustment->tinted) {
		double lightness = gray < shadows	   ? gray * (.5 / middle)
						   : gray < highlights ? gray + strength * (.5 - middle)
											   : 1 - (1 - gray) * .5 / (1 - middle);
		convert_hsl_to_rgb(adjustment->tint_hue,
						   MIN(1, strength + 3 * strength * fabs(gray - .5 * (shadows + highlights))), lightness, rgb);
	} else
		convert_hsl_to_rgb(0, 0, gray, rgb);
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		pixel[channel] = wrap_byte(unit_to_byte(rgb[channel]));
}

static void transform_selective_color(const Adjustment *adjustment, guint8 *pixel) {
	const float *ranges = adjustment->selective;
	const double unit = BYTE_UNIT;
	double red = pixel[0] * unit, green = pixel[1] * unit, blue = pixel[2] * unit, hsl[RGB_CHANNELS];
	convert_rgb_to_hsl(red, green, blue, hsl);
	double high = MAX(red, MAX(green, blue)), low = MIN(red, MIN(green, blue));
	double inks[SELECTIVE_INKS_MIXED] = {1 - red, 1 - green, 1 - blue}, changes[SELECTIVE_INKS_MIXED] = {0, 0, 0};
	/* An entry that multiplies by one and adds nothing changes nothing */
	const float unchanged[SELECTIVE_ENTRY] = {1, 1, 1, 0, 0, 0};
	for (int range = 0; range < SELECTIVE_RANGES_COUNT; range++) {
		const float *entry = ranges + range * SELECTIVE_ENTRY;
		if (!memcmp(entry, unchanged, sizeof unchanged))
			continue;
		double weight = range < SELECTIVE_WHITES
							? weigh_hue_range(hsl[0], range * (1.0 / HUE_SECTORS)) * hsl[1] * 2 * MIN(hsl[2], 1 - hsl[2])
						: range == SELECTIVE_WHITES	  ? MAX(0, low - .5) * 2
						: range == SELECTIVE_NEUTRALS ? 1 - (fabs(high - .5) + fabs(low - .5))
													  : MAX(0, .5 - high) * 2;
		for (int ink = 0; ink < SELECTIVE_INKS_MIXED; ink++)
			changes[ink] +=
				(MAX(0, MIN(1, inks[ink] * entry[ink] + entry[ink + SELECTIVE_INKS_MIXED])) - inks[ink]) * weight;
	}
	for (int ink = 0; ink < SELECTIVE_INKS_MIXED; ink++)
		pixel[ink] = (guint8)floor(unit_to_byte(1 - MAX(0, MIN(1, inks[ink] + changes[ink]))) + .5);
}

static double weigh_replaced_color(const Adjustment *adjustment, const double lab[RGB_CHANNELS]) {
	const double *low = adjustment->replace_low, *high = adjustment->replace_high;
	/* The lightness counts in hundredths and a and b in the Lab curve's steps */
	const double scales[RGB_CHANNELS] = {1 / PERCENT, 1 / 116.0, 1 / 116.0};
	double sum = 0;
	for (int channel = 0; channel < RGB_CHANNELS; channel++) {
		double value = lab[channel];
		double outside = (value < low[channel]	  ? low[channel] - value
						  : high[channel] < value ? high[channel] - value
												  : 0) *
						 scales[channel];
		sum += outside * outside;
	}
	double distance = sqrt(sum) * 1.35, fuzziness = adjustment->replace_fuzziness;
	return distance <= fuzziness ? MIN(1, 1.17 * (1 - distance * (1 / fuzziness))) : 0;
}

static void transform_replace_color(const Adjustment *adjustment, guint8 *pixel) {
	const double unit = BYTE_UNIT, *shift = adjustment->replace_shift;
	double lab[RGB_CHANNELS];
	convert_rgb_to_lab(pixel[0], pixel[1], pixel[2], lab);
	double weight = weigh_replaced_color(adjustment, lab);
	if (weight == 0)
		return;
	double rgb[RGB_CHANNELS] = {pixel[0] * unit, pixel[1] * unit, pixel[2] * unit}, hsl[RGB_CHANNELS], changed[RGB_CHANNELS];
	convert_rgb_to_hsl(rgb[0], rgb[1], rgb[2], hsl);
	double hue = 2 + hsl[0] + shift[0];
	convert_hsl_to_rgb(hue - trunc(hue), MAX(0, MIN(1, hsl[1] + shift[1])), MAX(0, MIN(1, hsl[2] + shift[2])), changed);
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		pixel[channel] = wrap_byte(unit_to_byte((1 - weight) * rgb[channel] + weight * changed[channel]));
}

static void transform_hue_saturation(const Adjustment *adjustment, guint8 *pixel) {
	const float *map = adjustment->hue_map;
	const double unit = BYTE_UNIT, levels = G_MAXUINT8;
	double red = pixel[0] * unit, green = pixel[1] * unit, blue = pixel[2] * unit;
	double low = MIN(red, MIN(green, blue)), high = MAX(red, MAX(green, blue));
	/* The position and the stretch index are never negative, so truncating floors them */
	double position = (get_rgb_hue(red, green, blue) + 5e-4) * levels;
	int whole = (int)position, first = whole * HUE_MAP_STRIDE;
	int second = MIN(G_MAXUINT8 * HUE_MAP_STRIDE, first + HUE_MAP_STRIDE);
	double fraction = position - whole;
	double hue = (1 - fraction) * map[first + HUE_MAP_HUE] + fraction * map[second + HUE_MAP_HUE];
	double saturation = (1 - fraction) * map[first + HUE_MAP_SATURATION] + fraction * map[second + HUE_MAP_SATURATION];
	double lightness = (1 - fraction) * map[first + HUE_MAP_LIGHTNESS] + fraction * map[second + HUE_MAP_LIGHTNESS];
	double share = -lightness, toward = low;
	if (0 < lightness) {
		share = lightness;
		toward = high;
	}
	double lift = adjustment->hue_lift + adjustment->hue_keep * share * toward;
	double keep = adjustment->hue_keep * (1 - share);
	red = lift + keep * red;
	green = lift + keep * green;
	blue = lift + keep * blue;
	high = MAX(red, MAX(green, blue));
	low = MIN(red, MIN(green, blue));
	double middle = (high + low) * .5, current = 0;
	if (high != low) {
		double range = high - low;
		current = middle > .5 ? range / (2 - (high + low)) : range / (high + low);
	}
	double result = saturation;
	if (!adjustment->colorize) {
		saturation = adjustment->saturation_stretches[(int)((1 + saturation) * ((SATURATION_STRETCHES - 1) / 2.0))];
		result = MIN(current * (1 + saturation), 1);
	}
	double rgb[RGB_CHANNELS];
	convert_hsl_to_rgb(hue, result, middle, rgb);
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		pixel[channel] = wrap_byte(unit_to_byte(rgb[channel]));
}

static void transform_vibrance(const Adjustment *adjustment, guint8 *pixel) {
	const double *to = adjustment->to_yuv, *from = adjustment->from_yuv;
	double red = adjustment->linear[pixel[0]], green = adjustment->linear[pixel[1]],
		   blue = adjustment->linear[pixel[2]];
	double yuv[MATRIX_SIDE];
	for (int row = 0; row < MATRIX_SIDE; row++) {
		const double *weights = to + row * MATRIX_SIDE;
		yuv[row] = weights[0] * red + weights[1] * green + weights[2] * blue + weights[3] * 1;
	}
	yuv[0] *= adjustment->vibrance_lightness;
	yuv[1] *= adjustment->vibrance_chroma;
	yuv[2] *= adjustment->vibrance_chroma;
	/* The chroma is scaled and then grown by the saturation, in that order */
	yuv[1] *= adjustment->vibrance_saturation;
	yuv[2] *= adjustment->vibrance_saturation;
	for (int row = 0; row < RGB_CHANNELS; row++) {
		const double *weights = from + row * MATRIX_SIDE;
		double value = weights[0] * yuv[0] + weights[1] * yuv[1] + weights[2] * yuv[2] + weights[3] * yuv[3];
		pixel[row] = encode_gamma(MAX(0, MIN(1, value)));
	}
}

static void transform_channel_mixer(const Adjustment *adjustment, guint8 *pixel) {
	const double *mix = adjustment->mixer, unit = BYTE_UNIT;
	double red = pixel[0] * unit, green = pixel[1] * unit, blue = pixel[2] * unit;
	for (int row = 0; row < RGB_CHANNELS; row++) {
		const double *parts = mix + row * MIXER_PARTS;
		double value = parts[MIXER_RED] * red + parts[MIXER_GREEN] * green + parts[MIXER_BLUE] * blue + parts[3];
		pixel[row] = wrap_byte(unit_to_byte(MAX(0, MIN(1, value))));
	}
}

static void transform_color_lookup(const Adjustment *adjustment, guint8 *pixel) {
	lookup_color_pixel(adjustment->color_lookup, pixel);
}

typedef void (*PixelTransform)(const Adjustment *adjustment, guint8 *pixel);

#define CACHE_HASH 2654435761u

/* The red, green and blue bytes of a pixel as one number, red lowest */
static guint32 pack_rgb(const guint8 *pixel) {
	return pixel[0] | pixel[1] << CHAR_BIT | (guint32)pixel[2] << 2 * CHAR_BIT;
}

static void unpack_rgb(guint32 color, guint8 *pixel) {
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		pixel[channel] = color >> channel * CHAR_BIT;
}

/* Each of these kernels gives one result per color, so a cache of recent colors gives the same bytes; images reuse
 * colors heavily, while a miss costs only a hash and a store */
static void apply_cached(const Adjustment *adjustment, PixelTransform transform, AdjustmentCache *cache, guint8 *pixels,
						 gsize length) {
	if (cache->owner != adjustment) {
		memset(cache->keys, 0, sizeof cache->keys);
		cache->owner = adjustment;
	}
	for (gsize index = 0; index + ALPHA_CHANNEL < length; index += RGBA_CHANNELS) {
		guint8 *pixel = pixels + index;
		guint32 color = pack_rgb(pixel);
		/* Multiplying by this odd number spreads near colors over the cache */
		guint slot = (color * CACHE_HASH) >> (sizeof(guint32) * CHAR_BIT - ADJUSTMENT_CACHE_BITS);
		/* Keys are one more than the color, so an empty slot never matches black */
		if (cache->keys[slot] == color + 1) {
			unpack_rgb(cache->colors[slot], pixel);
			continue;
		}
		transform(adjustment, pixel);
		cache->keys[slot] = color + 1;
		cache->colors[slot] = pack_rgb(pixel);
	}
}

static PixelTransform get_pixel_transform(AdjustmentKind kind) {
	switch (kind) {
	case ADJUSTMENT_HUE_SATURATION:
		return transform_hue_saturation;
	case ADJUSTMENT_VIBRANCE:
		return transform_vibrance;
	case ADJUSTMENT_MIXER:
		return transform_channel_mixer;
	case ADJUSTMENT_BLACK_WHITE:
		return transform_black_white;
	case ADJUSTMENT_SELECTIVE_COLOR:
		return transform_selective_color;
	case ADJUSTMENT_REPLACE_COLOR:
		return transform_replace_color;
	case ADJUSTMENT_COLOR_LOOKUP:
		return transform_color_lookup;
	default:
		return NULL;
	}
}

static PixelTransform get_adjustment_transform(const Adjustment *adjustment) {
	PixelTransform transform = get_pixel_transform(adjustment->kind);
	if (transform)
		return transform;
	if (adjustment->luminance)
		return transform_luminance_curves;
	return adjustment->preserve_luminosity ? transform_preserving_curves : NULL;
}

void apply_adjustment(const Adjustment *adjustment, AdjustmentCache *cache, guint8 *pixels, gsize length) {
	PixelTransform transform = get_adjustment_transform(adjustment);
	if (transform) {
		AdjustmentCache local;
		if (!cache) {
			local.owner = NULL;
			cache = &local;
		}
		apply_cached(adjustment, transform, cache, pixels, length);
	} else {
		const guint8 *red = adjustment->lookup[0], *green = adjustment->lookup[1], *blue = adjustment->lookup[2];
		for (gsize index = 0; index + ALPHA_CHANNEL < length; index += RGBA_CHANNELS) {
			pixels[index] = red[pixels[index]];
			pixels[index + 1] = green[pixels[index + 1]];
			pixels[index + 2] = blue[pixels[index + 2]];
		}
	}
}
