#include <limits.h>
#include <math.h>
#include <string.h>

#include "blend.h"
#include "numbers.h"
#include "color/color-spaces.h"

const char *const blend_mode_keys[BLEND_MODE_COUNT] = {
	"norm", "diss", "dark", "mul ", "idiv", "lbrn", "dkCl", "lite", "scrn", "div ", "lddg", "lgCl", "over", "sLit",
	"hLit", "vLit", "lLit", "pLit", "hMix", "diff", "smud", "fsub", "fdiv", "hue ", "sat ", "colr", "lum "};

/* Adds a half and wraps the whole part into 32 bits, so negative halves round toward zero */
static int32_t round_to_int32(double value) {
	return wrap_int32(value + .5);
}

int find_blend_mode(const char *key) {
	if (!key)
		return -1;
	for (int mode = 0; mode < BLEND_MODE_COUNT; mode++)
		if (!strcmp(blend_mode_keys[mode], key))
			return mode;
	return -1;
}

/* Ranges reach a hair past their ends, and a range without a fade has a slope this steep */
#define RANGE_EDGE .001
#define STEEP_SLOPE 1e6

static double sum_range_part(const double *block, int from, int to) {
	const double *below = block + RANGE_VALUES;
	return block[from] + block[to] + below[from] + below[to];
}

/* Ranges that all run from black to white change nothing, so only other ones are prepared, as each range's start,
 * the slopes of its two fades and its end, in unit values */
bool prepare_blend_ranges(const double *layer_ranges, double *prepared) {
	bool custom = false;
	for (int block = 0; block < RANGE_BLOCKS - 1; block++) {
		const double *values = layer_ranges + block * RANGE_BLOCK;
		if (sum_range_part(values, RANGE_BLACK_FROM, RANGE_BLACK_TO) != 0 ||
			sum_range_part(values, RANGE_WHITE_FROM, RANGE_WHITE_TO) != RANGE_VALUES * G_MAXUINT8)
			custom = true;
	}
	if (!custom)
		return false;
	for (int index = 0; index < BLEND_RANGE_VALUES; index += RANGE_VALUES) {
		double low = layer_ranges[index + RANGE_BLACK_FROM] / G_MAXUINT8, low_end = layer_ranges[index + RANGE_BLACK_TO] / G_MAXUINT8;
		double high_start = layer_ranges[index + RANGE_WHITE_FROM] / G_MAXUINT8;
		double high = layer_ranges[index + RANGE_WHITE_TO] / G_MAXUINT8;
		prepared[index] = low - RANGE_EDGE;
		prepared[index + 1] = low == low_end ? STEEP_SLOPE : 1 / (low_end - low);
		prepared[index + 2] = high_start == high ? -STEEP_SLOPE : 1 / (high_start - high);
		prepared[index + 3] = high + RANGE_EDGE;
	}
	return true;
}


static double normal_channel(double source, double destination, double fill) {
	return source;
}

static double darken_channel(double source, double destination, double fill) {
	return nan_min(source, destination);
}

static double multiply_channel(double source, double destination, double fill) {
	return source * destination;
}

static double lighten_channel(double source, double destination, double fill) {
	return nan_max(source, destination);
}

static double screen_channel(double source, double destination, double fill) {
	return destination + source - destination * source;
}

static double color_burn_channel(double source, double destination, double fill) {
	double scaled = source * fill + 1 - fill;
	return destination == 1 ? 1 : 1 - destination >= scaled ? 0 : 1 - (1 - destination) / scaled;
}

static double linear_burn_channel(double source, double destination, double fill) {
	return nan_max(0, source * fill + destination - fill);
}

static double color_dodge_channel(double source, double destination, double fill) {
	source *= fill;
	return destination == 0 ? 0 : destination >= 1 - source ? 1 : destination / (1 - source);
}

static double linear_dodge_channel(double source, double destination, double fill) {
	source *= fill;
	return nan_min(1, source + destination);
}

static double vivid_burn(double source, double destination, double fill) {
	double scaled = source * fill + 1 - fill;
	return scaled < .001 ? 0 : 1 - nan_min(1, (1 - destination) / scaled);
}

static double vivid_dodge(double source, double destination, double fill) {
	return source * fill == 1 ? 1 : nan_min(1, destination / (1 - source * fill));
}

static double soft_light_curve(double value) {
	return value <= .25 ? ((16 * value - 12) * value + 4) * value : sqrt(value);
}

static double hard_light_channel(double source, double destination, double fill) {
	return source <= .5 ? multiply_channel(2 * source, destination, fill)
						: screen_channel(2 * source - 1, destination, fill);
}

static double overlay_channel(double source, double destination, double fill) {
	return hard_light_channel(destination, source, fill);
}

static double soft_light_channel(double source, double destination, double fill) {
	return source <= .5 ? destination - (1 - 2 * source) * destination * (1 - destination)
						: destination + (2 * source - 1) * (soft_light_curve(destination) - destination);
}

static double vivid_light_channel(double source, double destination, double fill) {
	return source <= .5 ? vivid_burn(2 * source, destination, fill) : vivid_dodge(2 * source - 1, destination, fill);
}

static double linear_light_channel(double source, double destination, double fill) {
	return source <= .5 ? linear_burn_channel(2 * source, destination, fill)
						: linear_dodge_channel(2 * source - 1, destination, fill);
}

static double pin_light_channel(double source, double destination, double fill) {
	return source <= .5 ? darken_channel(2 * source, destination, fill)
						: lighten_channel(2 * source - 1, destination, fill);
}

static double hard_mix_channel(double source, double destination, double fill) {
	if (fill > .99)
		return source + destination < 1 ? 0 : 1;
	return nan_min(1, nan_max(0, (destination + source * fill - fill) / (1 - fill + 1e-6)));
}

static double difference_channel(double source, double destination, double fill) {
	source *= fill;
	return fabs(source - destination);
}

static double exclusion_channel(double source, double destination, double fill) {
	return source + destination - 2 * source * destination;
}

static double subtract_channel(double source, double destination, double fill) {
	return nan_max(destination - source, 0);
}

static double divide_channel(double source, double destination, double fill) {
	return nan_min(destination / source, 1);
}

static double get_luminosity(const double *color) {
	return get_luma(color[0], color[1], color[2]);
}

/* Saturation as the spread between the strongest and weakest channel */
static double get_saturation(const double *color) {
	return nan_max(nan_max(color[0], color[1]), color[2]) - nan_min(nan_min(color[0], color[1]), color[2]);
}

static void clip_color(double *color) {
	double red = color[0], green = color[1], blue = color[2], luminosity = get_luminosity(color);
	double lowest = nan_min(nan_min(red, green), blue), highest = nan_max(nan_max(red, green), blue);
	if (lowest < 0) {
		double scale = luminosity / (luminosity - lowest);
		red = luminosity + (red - luminosity) * scale;
		green = luminosity + (green - luminosity) * scale;
		blue = luminosity + (blue - luminosity) * scale;
	}
	if (highest > 1) {
		double scale = (1 - luminosity) / (highest - luminosity);
		red = luminosity + (red - luminosity) * scale;
		green = luminosity + (green - luminosity) * scale;
		blue = luminosity + (blue - luminosity) * scale;
	}
	color[0] = red;
	color[1] = green;
	color[2] = blue;
}

static void set_luminosity(const double *color, double luminosity, double *result) {
	double change = luminosity - get_luminosity(color);
	result[0] = color[0] + change;
	result[1] = color[1] + change;
	result[2] = color[2] + change;
	clip_color(result);
}

static double scale_middle(double highest, double middle, double lowest, double saturation) {
	return (middle - lowest) * saturation / (highest - lowest);
}

static void set_saturation(const double *color, double saturation, double *result) {
	double red = color[0], green = color[1], blue = color[2];
	if (red == green && green == blue) {
		result[0] = result[1] = result[2] = 0;
	} else if (red > green) {
		if (red > blue) {
			if (green > blue) {
				result[0] = saturation;
				result[1] = scale_middle(red, green, blue, saturation);
				result[2] = 0;
			} else {
				result[0] = saturation;
				result[2] = scale_middle(red, blue, green, saturation);
				result[1] = 0;
			}
		} else {
			result[2] = saturation;
			result[0] = scale_middle(blue, red, green, saturation);
			result[1] = 0;
		}
	} else if (red < blue) {
		if (green > blue) {
			result[1] = saturation;
			result[2] = scale_middle(green, blue, red, saturation);
			result[0] = 0;
		} else {
			result[2] = saturation;
			result[1] = scale_middle(blue, green, red, saturation);
			result[0] = 0;
		}
	} else {
		result[1] = saturation;
		result[0] = scale_middle(green, red, blue, saturation);
		result[2] = 0;
	}
}

static void darker_color(const double *source, const double *destination, double *result) {
	memcpy(result, get_luminosity(source) < get_luminosity(destination) ? source : destination, 3 * sizeof(double));
}

static void lighter_color(const double *source, const double *destination, double *result) {
	memcpy(result, get_luminosity(source) > get_luminosity(destination) ? source : destination, 3 * sizeof(double));
}

static void hue_color(const double *source, const double *destination, double *result) {
	set_saturation(source, get_saturation(destination), result);
	set_luminosity(result, get_luminosity(destination), result);
}

static void saturation_color(const double *source, const double *destination, double *result) {
	set_saturation(destination, get_saturation(source), result);
	set_luminosity(result, get_luminosity(destination), result);
}

static void color_color(const double *source, const double *destination, double *result) {
	set_luminosity(source, get_luminosity(destination), result);
}

static void luminosity_color(const double *source, const double *destination, double *result) {
	set_luminosity(destination, get_luminosity(source), result);
}

static double measure_range(double value, const double *ranges, int index) {
	return nan_min((value - ranges[index]) * ranges[index + 1], (value - ranges[index + 3]) * ranges[index + 2]);
}

static double measure_blend_ranges(const double *source, const double *destination, double destination_alpha,
								   const double *ranges) {
	/* The gray block first, then red, green and blue */
	double top = measure_range(get_luminosity(source), ranges, 0);
	double below = measure_range(get_luminosity(destination), ranges, RANGE_VALUES);
	for (int channel = 0; channel < RGB_CHANNELS; channel++) {
		int block = (channel + 1) * RANGE_BLOCK;
		top = nan_min(top, measure_range(source[channel], ranges, block));
		below = nan_min(below, measure_range(destination[channel], ranges, block + RANGE_VALUES));
	}
	below = nan_max(below, 1 - destination_alpha);
	double factor = nan_min(top, below);
	return factor < 0 ? 0 : factor > 1 ? 1 : factor;
}

static void read_color(uint32_t pixel, double *color) {
	color[0] = ((pixel >> WORD_RED_SHIFT) & G_MAXUINT8) * BYTE_UNIT;
	color[1] = ((pixel >> WORD_GREEN_SHIFT) & G_MAXUINT8) * BYTE_UNIT;
	color[2] = (pixel & G_MAXUINT8) * BYTE_UNIT;
}

uint32_t compose_normal_pixel(uint32_t source, uint32_t destination, double opacity) {
	unsigned source_alpha = source >> WORD_ALPHA_SHIFT;
	if (!source_alpha)
		return destination;
	if (source_alpha == G_MAXUINT8 && opacity == 1)
		return source;
	double source_weight = source_alpha * opacity * BYTE_UNIT;
	double destination_alpha = (destination >> WORD_ALPHA_SHIFT) * BYTE_UNIT;
	double destination_weight = destination_alpha * (1 - source_weight);
	double alpha = source_weight + destination_weight;
	if (alpha == 0)
		return 0;
	double reciprocal = 1 / alpha;
	uint32_t pixel = (uint32_t)(0.5 + unit_to_byte(alpha)) << WORD_ALPHA_SHIFT;
	for (unsigned shift = 0; shift <= WORD_RED_SHIFT; shift += CHAR_BIT) {
		double source_channel = (source >> shift) & G_MAXUINT8;
		double destination_channel = (destination >> shift) & G_MAXUINT8;
		unsigned channel =
			0.5 + (source_channel * source_weight + destination_channel * destination_weight) * reciprocal;
		pixel |= channel << shift;
	}
	return pixel;
}

static uint32_t blend_normal_pixel(const BlendKernel *kernel, uint32_t source, uint32_t destination, uint32_t index) {
	if (!kernel->options.preserve_alpha)
		return compose_normal_pixel(source, destination, kernel->opacity);
	if (!(source >> WORD_ALPHA_SHIFT))
		return destination;
	double opacity = kernel->opacity, destination_alpha = (destination >> WORD_ALPHA_SHIFT) * BYTE_UNIT;
	double below = 1 - opacity, reciprocal = 1 / (opacity + below);
	uint32_t pixel = (uint32_t)round_to_int32(unit_to_byte(destination_alpha)) << WORD_ALPHA_SHIFT;
	for (unsigned shift = 0; shift <= WORD_RED_SHIFT; shift += CHAR_BIT) {
		double top = (source >> shift) & G_MAXUINT8, bottom = (destination >> shift) & G_MAXUINT8;
		pixel |= ((uint32_t)round_to_int32((top * opacity + bottom * below) * reciprocal) & G_MAXUINT8) << shift;
	}
	return pixel;
}

static uint32_t blend_dissolve_pixel(const BlendKernel *kernel, uint32_t source, uint32_t destination, uint32_t index) {
	double preserve = kernel->options.preserve_alpha ? 1 : 0;
	double scale = floor(kernel->opacity * ((double)(1 << DISSOLVE_BITS) / G_MAXUINT8) + .5);
	double share = preserve * G_MAXUINT8 + (1 - preserve) * (source >> WORD_ALPHA_SHIFT);
	if ((hash_whole_number(index) & DISSOLVE_MASK) >= share * scale)
		return destination;
	return (source & WORD_COLOR_MASK) |
		   (uint32_t)wrap_int32(preserve * (destination >> WORD_ALPHA_SHIFT) + (1 - preserve) * G_MAXUINT8) << WORD_ALPHA_SHIFT;
}

static uint32_t blend_separable_pixel(const BlendKernel *kernel, uint32_t source, uint32_t destination,
									  uint32_t index) {
	if (!(source >> WORD_ALPHA_SHIFT))
		return destination;
	double preserve = kernel->options.preserve_alpha ? 1 : 0, fill = kernel->options.fill;
	double top_alpha = kernel->opacity, bottom_alpha = 1;
	double top[RGB_CHANNELS], bottom[RGB_CHANNELS];
	read_color(source, top);
	read_color(destination, bottom);
	if (!preserve) {
		top_alpha = (source >> WORD_ALPHA_SHIFT) * (BYTE_UNIT * kernel->opacity);
		bottom_alpha = (destination >> WORD_ALPHA_SHIFT) * BYTE_UNIT;
	}
	if (kernel->options.ranges)
		top_alpha *= measure_blend_ranges(top, bottom, bottom_alpha, kernel->options.ranges);
	double below = bottom_alpha * (1 - top_alpha), total = top_alpha + below;
	double scale = total == 0 ? 0 : G_MAXUINT8 / total, weight = kernel->options.style ? 1 : top_alpha;
	double mixed_fill = (1 + top_alpha - weight) * fill;
	uint32_t pixel = 0;
	for (int channel = 0; channel < RGB_CHANNELS; channel++) {
		double value = ((1 - bottom_alpha) * top_alpha * top[channel] + (1 - weight) * bottom_alpha * bottom[channel] +
						weight * bottom_alpha * kernel->channel(top[channel], bottom[channel], mixed_fill)) *
					   scale;
		pixel |= ((uint32_t)round_to_int32(value) & G_MAXUINT8) << (WORD_RED_SHIFT - channel * CHAR_BIT);
	}
	double alpha = top_alpha * fill + bottom_alpha * (1 - top_alpha * fill);
	double kept = preserve * (destination >> WORD_ALPHA_SHIFT) + (1 - preserve) * round_to_int32(unit_to_byte(alpha));
	return pixel | (uint32_t)round_to_int32(kept) << WORD_ALPHA_SHIFT;
}

static uint32_t blend_color_pixel(const BlendKernel *kernel, uint32_t source, uint32_t destination, uint32_t index) {
	double preserve = kernel->options.preserve_alpha ? 1 : 0;
	double top_alpha = kernel->opacity, bottom_alpha = 1;
	double top[RGB_CHANNELS], bottom[RGB_CHANNELS], mixed[RGB_CHANNELS];
	read_color(source, top);
	read_color(destination, bottom);
	if (!preserve) {
		top_alpha = (source >> WORD_ALPHA_SHIFT) * (BYTE_UNIT * kernel->opacity);
		bottom_alpha = (destination >> WORD_ALPHA_SHIFT) * BYTE_UNIT;
	}
	if (kernel->options.ranges)
		top_alpha *= measure_blend_ranges(top, bottom, bottom_alpha, kernel->options.ranges);
	double below = bottom_alpha * (1 - top_alpha), total = top_alpha + below, scale = G_MAXUINT8 / total;
	kernel->color(top, bottom, mixed);
	uint32_t pixel = 0;
	for (int channel = 0; channel < RGB_CHANNELS; channel++) {
		double value = (((1 - bottom_alpha) * top[channel] + bottom_alpha * mixed[channel]) * top_alpha +
						bottom[channel] * below) *
					   scale;
		pixel |= ((uint32_t)wrap_int32(value) & G_MAXUINT8) << (WORD_RED_SHIFT - channel * CHAR_BIT);
	}
	double alpha = preserve * (destination >> WORD_ALPHA_SHIFT) + (1 - preserve) * floor(unit_to_byte(total) + .5);
	return pixel | (uint32_t)wrap_int32(alpha) << WORD_ALPHA_SHIFT;
}

BlendKernel prepare_blend_kernel(BlendMode mode, double opacity, BlendOptions options) {
	static double (*const channels[BLEND_MODE_COUNT])(double, double,
													  double) = {[BLEND_NORMAL] = normal_channel,
																 [BLEND_DARKEN] = darken_channel,
																 [BLEND_MULTIPLY] = multiply_channel,
																 [BLEND_COLOR_BURN] = color_burn_channel,
																 [BLEND_LINEAR_BURN] = linear_burn_channel,
																 [BLEND_LIGHTEN] = lighten_channel,
																 [BLEND_SCREEN] = screen_channel,
																 [BLEND_COLOR_DODGE] = color_dodge_channel,
																 [BLEND_LINEAR_DODGE] = linear_dodge_channel,
																 [BLEND_OVERLAY] = overlay_channel,
																 [BLEND_SOFT_LIGHT] = soft_light_channel,
																 [BLEND_HARD_LIGHT] = hard_light_channel,
																 [BLEND_VIVID_LIGHT] = vivid_light_channel,
																 [BLEND_LINEAR_LIGHT] = linear_light_channel,
																 [BLEND_PIN_LIGHT] = pin_light_channel,
																 [BLEND_HARD_MIX] = hard_mix_channel,
																 [BLEND_DIFFERENCE] = difference_channel,
																 [BLEND_EXCLUSION] = exclusion_channel,
																 [BLEND_SUBTRACT] = subtract_channel,
																 [BLEND_DIVIDE] = divide_channel};
	static void (*const colors[BLEND_MODE_COUNT])(const double *, const double *,
												  double *) = {[BLEND_DARKER_COLOR] = darker_color,
															   [BLEND_LIGHTER_COLOR] = lighter_color,
															   [BLEND_HUE] = hue_color,
															   [BLEND_SATURATION] = saturation_color,
															   [BLEND_COLOR] = color_color,
															   [BLEND_LUMINOSITY] = luminosity_color};
	bool uses_fill = mode == BLEND_COLOR_BURN || mode == BLEND_LINEAR_BURN || mode == BLEND_COLOR_DODGE ||
					 mode == BLEND_LINEAR_DODGE || mode == BLEND_VIVID_LIGHT || mode == BLEND_LINEAR_LIGHT ||
					 mode == BLEND_HARD_MIX || mode == BLEND_DIFFERENCE;
	if (!uses_fill) {
		opacity *= options.fill;
		options.fill = 1;
		options.style = false;
	}
	BlendKernel kernel = {NULL, channels[mode], colors[mode], opacity, options};
	if (mode == BLEND_NORMAL && !options.ranges)
		kernel.blend = blend_normal_pixel;
	else if (mode == BLEND_DISSOLVE)
		kernel.blend = blend_dissolve_pixel;
	else if (kernel.channel)
		kernel.blend = blend_separable_pixel;
	else
		kernel.blend = blend_color_pixel;
	return kernel;
}
