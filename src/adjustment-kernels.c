#include <math.h>
#include <string.h>

#include "adjustment-kernels.h"
#include "color-spaces.h"

/* ============================== Tables ========================================== */

/* JavaScript stores numbers into a Uint8Array by truncating and wrapping, where a NaN becomes zero */
static guint8 store_byte(double value) {
	if (value >= 0 && value < 256)
		return (guint8)value;
	if (!isfinite(value))
		return 0;
	return (guint8)(gint64)fmod(trunc(value), 256);
}

static guint8 round_to_byte(float value) {
	if (isnan(value))
		return 0;
	return MIN(255, (gint32)(0.5 + 255 * (double)value));
}

static double sample_curve(const float *curve, int length, double position) {
	int index = (int)position;
	double fraction = position - index;
	return curve[index] * (1 - fraction) + curve[index + (index == length - 1 ? 0 : 1)] * fraction;
}

static void fill_curve_lookups(Adjustment *adjustment) {
	const float *curves[] = {adjustment->red, adjustment->green, adjustment->blue};
	double last = (adjustment->curve_length - 1) * .999999, unit = 1 / 255.0;
	const double weights[] = {.3, .59, .11};
	for (int channel = 0; channel < 3; channel++)
		for (int index = 0; index < 256; index++) {
			adjustment->luminance_terms[channel * 256 + index] = index * unit * weights[channel];
			adjustment->sampled_curves[channel * 256 + index] =
				sample_curve(curves[channel], adjustment->curve_length, last * (index * unit));
		}
	for (int channel = 0; channel < 3; channel++)
		for (int index = 0; index < 256; index++)
			adjustment->lookup[channel][index] =
				adjustment->curve_length == 256
					? round_to_byte(curves[channel][index])
					: store_byte(sample_curve(curves[channel], adjustment->curve_length, last * (index * unit)) * 255);
}

/* The gamma encoding at the end of IK, store_byte(pow(value, 1 / 2.4) * 255), never falls as the value grows, so
 * each byte starts at a fixed value; these thresholds are found once by bisecting the real formula, which keeps
 * the result exact while a pixel needs a short search instead of a power */
static double encoding_thresholds[256];

/* The byte at the start of each of these equal steps of the value, from which the search rarely moves */
enum { ENCODING_BUCKETS = 1 << 16 };

static guint8 encoding_starts[ENCODING_BUCKETS + 1];

static guint8 encode_gamma_slowly(double value) {
	return store_byte(pow(value, 1 / 2.4) * 255);
}

static void find_encoding_thresholds(void) {
	static gsize ready;
	if (!g_once_init_enter(&ready))
		return;
	encoding_thresholds[0] = 0;
	for (int level = 1; level < 256; level++) {
		double low = encoding_thresholds[level - 1], high = 1;
		while (nextafter(low, 2) < high) {
			double middle = low + (high - low) / 2;
			if (middle <= low || middle >= high)
				middle = nextafter(low, 2);
			if (encode_gamma_slowly(middle) >= level)
				high = middle;
			else
				low = middle;
		}
		encoding_thresholds[level] = high;
	}
	int level = 0;
	for (int bucket = 0; bucket <= ENCODING_BUCKETS; bucket++) {
		while (level < 255 && (double)bucket / ENCODING_BUCKETS >= encoding_thresholds[level + 1])
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
	while (level < 255 && value >= encoding_thresholds[level + 1])
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

static void sample_curves_at(const Adjustment *adjustment, double position, double channels[3]) {
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
	double luminance = terms[pixel[0]] + terms[256 + pixel[1]] + terms[512 + pixel[2]], channels[3];
	sample_curves_at(adjustment, (adjustment->curve_length - 1) * .999999 * luminance, channels);
	for (int channel = 0; channel < 3; channel++)
		pixel[channel] = store_byte(channels[channel] * 255);
}

static void transform_preserving_curves(const Adjustment *adjustment, guint8 *pixel) {
	const double unit = 1 / 255.0, *sampled = adjustment->sampled_curves;
	double red = pixel[0] * unit, green = pixel[1] * unit, blue = pixel[2] * unit;
	double channels[3] = {sampled[pixel[0]], sampled[256 + pixel[1]], sampled[512 + pixel[2]]};
	double before = .3 * red + .59 * green + .11 * blue;
	double after = .3 * channels[0] + .59 * channels[1] + .11 * channels[2];
	if (before > after) {
		double lift = (before - after) / (1 - after);
		for (int channel = 0; channel < 3; channel++)
			channels[channel] += lift * (1 - channels[channel]);
	} else if (after == 0)
		channels[0] = channels[1] = channels[2] = 0;
	else {
		double scale = before / after;
		for (int channel = 0; channel < 3; channel++)
			channels[channel] = scale * channels[channel];
	}
	for (int channel = 0; channel < 3; channel++)
		pixel[channel] = store_byte(channels[channel] * 255);
}

static void transform_black_white(const Adjustment *adjustment, guint8 *pixel) {
	const double unit = 1 / 255.0, strength = adjustment->tint_strength, middle = adjustment->tint_middle;
	const double shadows = adjustment->tint_shadows, highlights = adjustment->tint_highlights;
	double hsl[3], change = 0, rgb[3];
	convert_rgb_to_hsl(pixel[0] * unit, pixel[1] * unit, pixel[2] * unit, hsl);
	for (int range = 0; range < 6; range++)
		change += MIN(1, 1.7 * (1 - hsl[2])) * hsl[1] * adjustment->gray_weights[range] *
				  weigh_hue_range(hsl[0], range * (1 / 6.0));
	double gray = MAX(0, MIN(1, hsl[2] * (1 + change)));
	if (adjustment->tinted) {
		double lightness = gray < shadows	   ? gray * (.5 / middle)
						   : gray < highlights ? gray + strength * (.5 - middle)
											   : 1 - (1 - gray) * .5 / (1 - middle);
		convert_hsl_to_rgb(adjustment->tint_hue,
						   MIN(1, strength + 3 * strength * fabs(gray - .5 * (shadows + highlights))), lightness, rgb);
	} else
		convert_hsl_to_rgb(0, 0, gray, rgb);
	for (int channel = 0; channel < 3; channel++)
		pixel[channel] = store_byte(rgb[channel] * 255);
}

static void transform_selective_color(const Adjustment *adjustment, guint8 *pixel) {
	const float *ranges = adjustment->selective;
	const double unit = 1 / 255.0;
	double red = pixel[0] * unit, green = pixel[1] * unit, blue = pixel[2] * unit, hsl[3];
	convert_rgb_to_hsl(red, green, blue, hsl);
	double high = MAX(red, MAX(green, blue)), low = MIN(red, MIN(green, blue));
	double inks[3] = {1 - red, 1 - green, 1 - blue}, changes[3] = {0, 0, 0};
	for (int range = 0; range < 9; range++) {
		const float *entry = ranges + range * 6;
		if (entry[0] == 1 && entry[1] == 1 && entry[2] == 1 && entry[3] == 0 && entry[4] == 0 && entry[5] == 0)
			continue;
		double weight = range < 6	 ? weigh_hue_range(hsl[0], range * (1 / 6.0)) * hsl[1] * 2 * MIN(hsl[2], 1 - hsl[2])
						: range == 6 ? MAX(0, low - .5) * 2
						: range == 7 ? 1 - (fabs(high - .5) + fabs(low - .5))
									 : MAX(0, .5 - high) * 2;
		for (int ink = 0; ink < 3; ink++)
			changes[ink] += (MAX(0, MIN(1, inks[ink] * entry[ink] + entry[ink + 3])) - inks[ink]) * weight;
	}
	for (int ink = 0; ink < 3; ink++)
		pixel[ink] = (guint8)floor((1 - MAX(0, MIN(1, inks[ink] + changes[ink]))) * 255 + .5);
}

static double weigh_replaced_color(const Adjustment *adjustment, const double lab[3]) {
	const double *low = adjustment->replace_low, *high = adjustment->replace_high;
	const double scales[] = {1 / 100.0, 1 / 116.0, 1 / 116.0};
	double sum = 0;
	for (int channel = 0; channel < 3; channel++) {
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

/* JavaScript shifts a number by ToInt32, which makes a NaN zero */
static guint8 shift_byte(double value) {
	return isfinite(value) ? (guint8)(gint64)fmod(trunc(value), 4294967296.0) : 0;
}

static void transform_replace_color(const Adjustment *adjustment, guint8 *pixel) {
	const double unit = 1 / 255.0, *shift = adjustment->replace_shift;
	double lab[3];
	convert_rgb_to_lab(pixel[0], pixel[1], pixel[2], lab);
	double weight = weigh_replaced_color(adjustment, lab);
	if (weight == 0)
		return;
	double rgb[3] = {pixel[0] * unit, pixel[1] * unit, pixel[2] * unit}, hsl[3], changed[3];
	convert_rgb_to_hsl(rgb[0], rgb[1], rgb[2], hsl);
	double hue = 2 + hsl[0] + shift[0];
	convert_hsl_to_rgb(hue - trunc(hue), MAX(0, MIN(1, hsl[1] + shift[1])), MAX(0, MIN(1, hsl[2] + shift[2])), changed);
	for (int channel = 0; channel < 3; channel++)
		pixel[channel] = shift_byte(((1 - weight) * rgb[channel] + weight * changed[channel]) * 255);
}

static void transform_hue_saturation(const Adjustment *adjustment, guint8 *pixel) {
	const float *map = adjustment->hue_map;
	const double unit = 1 / 255.0, levels = 255;
	double red = pixel[0] * unit, green = pixel[1] * unit, blue = pixel[2] * unit;
	double low = MIN(red, MIN(green, blue)), high = MAX(red, MAX(green, blue));
	/* The position and the stretch index are never negative, so truncating floors them */
	double position = (get_rgb_hue(red, green, blue) + 5e-4) * levels;
	int whole = (int)position, first = whole * 4, second = MIN(255 * 4, first + 4);
	double fraction = position - whole;
	double hue = (1 - fraction) * map[first] + fraction * map[second];
	double saturation = (1 - fraction) * map[first + 1] + fraction * map[second + 1];
	double lightness = (1 - fraction) * map[first + 2] + fraction * map[second + 2];
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
		saturation = adjustment->saturation_stretches[(int)((1 + saturation) * 511.5)];
		result = MIN(current * (1 + saturation), 1);
	}
	double rgb[3];
	convert_hsl_to_rgb(hue, result, middle, rgb);
	for (int channel = 0; channel < 3; channel++)
		pixel[channel] = store_byte(rgb[channel] * 255);
}

static void transform_vibrance(const Adjustment *adjustment, guint8 *pixel) {
	const double *to = adjustment->to_yuv, *from = adjustment->from_yuv;
	double red = adjustment->linear[pixel[0]], green = adjustment->linear[pixel[1]],
		   blue = adjustment->linear[pixel[2]];
	double yuv[4];
	for (int row = 0; row < 4; row++)
		yuv[row] = to[row * 4] * red + to[row * 4 + 1] * green + to[row * 4 + 2] * blue + to[row * 4 + 3] * 1;
	yuv[0] *= adjustment->vibrance_lightness;
	yuv[1] *= adjustment->vibrance_chroma;
	yuv[2] *= adjustment->vibrance_chroma;
	/* The chroma is scaled and then grown by the saturation, in that order */
	yuv[1] *= adjustment->vibrance_saturation;
	yuv[2] *= adjustment->vibrance_saturation;
	for (int row = 0; row < 3; row++) {
		double value = from[row * 4] * yuv[0] + from[row * 4 + 1] * yuv[1] + from[row * 4 + 2] * yuv[2] +
					   from[row * 4 + 3] * yuv[3];
		pixel[row] = encode_gamma(MAX(0, MIN(1, value)));
	}
}

static void transform_channel_mixer(const Adjustment *adjustment, guint8 *pixel) {
	const double *mix = adjustment->mixer, unit = 1 / 255.0;
	double red = pixel[0] * unit, green = pixel[1] * unit, blue = pixel[2] * unit;
	for (int row = 0; row < 3; row++) {
		double value = mix[row * 4] * red + mix[row * 4 + 1] * green + mix[row * 4 + 2] * blue + mix[row * 4 + 3];
		pixel[row] = store_byte(255 * MAX(0, MIN(1, value)));
	}
}

static void transform_color_lookup(const Adjustment *adjustment, guint8 *pixel) {
	lookup_color_pixel(adjustment->color_lookup, pixel);
}

typedef void (*PixelTransform)(const Adjustment *adjustment, guint8 *pixel);

/* Each of these kernels gives one result per color, so a cache of recent colors gives the same bytes; images reuse
 * colors heavily, while a miss costs only a hash and a store */
static void apply_cached(const Adjustment *adjustment, PixelTransform transform, AdjustmentCache *cache, guint8 *pixels,
						 gsize length) {
	if (cache->owner != adjustment) {
		memset(cache->keys, 0, sizeof cache->keys);
		cache->owner = adjustment;
	}
	for (gsize index = 0; index + 3 < length; index += 4) {
		guint8 *pixel = pixels + index;
		guint32 color = pixel[0] | pixel[1] << 8 | (guint32)pixel[2] << 16;
		guint slot = (color * 2654435761u) >> (32 - ADJUSTMENT_CACHE_BITS);
		if (cache->keys[slot] == color + 1) {
			guint32 result = cache->colors[slot];
			pixel[0] = result;
			pixel[1] = result >> 8;
			pixel[2] = result >> 16;
			continue;
		}
		transform(adjustment, pixel);
		cache->keys[slot] = color + 1;
		cache->colors[slot] = pixel[0] | pixel[1] << 8 | (guint32)pixel[2] << 16;
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
		for (gsize index = 0; index + 3 < length; index += 4) {
			pixels[index] = red[pixels[index]];
			pixels[index + 1] = green[pixels[index + 1]];
			pixels[index + 2] = blue[pixels[index + 2]];
		}
	}
}
