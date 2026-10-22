#include <math.h>
#include <string.h>

#include "blend.h"

const char *const blend_mode_keys[BLEND_MODE_COUNT] = {
	"norm", "diss", "dark", "mul ", "idiv", "lbrn", "dkCl", "lite", "scrn", "div ", "lddg", "lgCl", "over", "sLit",
	"hLit", "vLit", "lLit", "pLit", "hMix", "diff", "smud", "fsub", "fdiv", "hue ", "sat ", "colr", "lum "};

/* JavaScript Math.min and Math.max return NaN when either side is NaN, unlike fmin and fmax */
static double js_min(double first, double second) {
	if (isnan(first) || isnan(second))
		return NAN;
	return first < second ? first : second;
}

static double js_max(double first, double second) {
	if (isnan(first) || isnan(second))
		return NAN;
	return first > second ? first : second;
}

/* JavaScript bitwise operators wrap every number into a signed 32 bit integer and turn NaN into zero */
static int32_t to_int32(double value) {
	if (!isfinite(value))
		return 0;
	double wrapped = fmod(trunc(value), 4294967296.0);
	if (wrapped < 0)
		wrapped += 4294967296.0;
	return (int32_t)(uint32_t)wrapped;
}

static uint32_t to_uint32(double value) {
	return (uint32_t)to_int32(value);
}

static int32_t round_half_up(double value) {
	return to_int32(value + .5);
}

int32_t hash_dissolve_position(double position) {
	int32_t value = to_int32(position);
	value = value ^ 61 ^ (int32_t)(to_uint32(value) >> 16);
	double sum = (double)value + (double)(int32_t)((uint32_t)value << 3);
	value = to_int32(sum) ^ (int32_t)(to_uint32(sum) >> 4);
	double product = (double)value * 668265261.0;
	return to_int32(product) ^ (int32_t)(to_uint32(product) >> 15);
}

int find_blend_mode(const char *key) {
	if (!key)
		return -1;
	for (int mode = 0; mode < BLEND_MODE_COUNT; mode++)
		if (!strcmp(blend_mode_keys[mode], key))
			return mode;
	return -1;
}

bool prepare_blend_ranges(const double *layer_ranges, double *prepared) {
	bool custom = false;
	for (int index = 0; index < 32; index += 8)
		if (layer_ranges[index] + layer_ranges[index + 1] + layer_ranges[index + 4] + layer_ranges[index + 5] != 0 ||
			layer_ranges[index + 2] + layer_ranges[index + 3] + layer_ranges[index + 6] + layer_ranges[index + 7] !=
				1020)
			custom = true;
	if (!custom)
		return false;
	for (int index = 0; index < 40; index += 4) {
		double low = layer_ranges[index] / 255, low_end = layer_ranges[index + 1] / 255;
		double high_start = layer_ranges[index + 2] / 255, high = layer_ranges[index + 3] / 255;
		prepared[index] = low - .001;
		prepared[index + 1] = low == low_end ? 1e6 : 1 / (low_end - low);
		prepared[index + 2] = high_start == high ? -1e6 : 1 / (high_start - high);
		prepared[index + 3] = high + .001;
	}
	return true;
}


static double normal_channel(double source, double destination, double fill) {
	return source;
}

static double darken_channel(double source, double destination, double fill) {
	return js_min(source, destination);
}

static double multiply_channel(double source, double destination, double fill) {
	return source * destination;
}

static double lighten_channel(double source, double destination, double fill) {
	return js_max(source, destination);
}

static double screen_channel(double source, double destination, double fill) {
	return destination + source - destination * source;
}

static double color_burn_channel(double source, double destination, double fill) {
	double scaled = source * fill + 1 - fill;
	return destination == 1 ? 1 : 1 - destination >= scaled ? 0 : 1 - (1 - destination) / scaled;
}

static double linear_burn_channel(double source, double destination, double fill) {
	return js_max(0, source * fill + destination - fill);
}

static double color_dodge_channel(double source, double destination, double fill) {
	source *= fill;
	return destination == 0 ? 0 : destination >= 1 - source ? 1 : destination / (1 - source);
}

static double linear_dodge_channel(double source, double destination, double fill) {
	source *= fill;
	return js_min(1, source + destination);
}

static double vivid_burn(double source, double destination, double fill) {
	double scaled = source * fill + 1 - fill;
	return scaled < .001 ? 0 : 1 - js_min(1, (1 - destination) / scaled);
}

static double vivid_dodge(double source, double destination, double fill) {
	return source * fill == 1 ? 1 : js_min(1, destination / (1 - source * fill));
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
	return js_min(1, js_max(0, (destination + source * fill - fill) / (1 - fill + 1e-6)));
}

static double difference_channel(double source, double destination, double fill) {
	source *= fill;
	return fabs(source - destination);
}

static double exclusion_channel(double source, double destination, double fill) {
	return source + destination - 2 * source * destination;
}

static double subtract_channel(double source, double destination, double fill) {
	return js_max(destination - source, 0);
}

static double divide_channel(double source, double destination, double fill) {
	return js_min(destination / source, 1);
}


static double get_luminosity(const double *color) {
	return .3 * color[0] + .59 * color[1] + .11 * color[2];
}

/* h.G2 */
static double get_saturation(const double *color) {
	return js_max(js_max(color[0], color[1]), color[2]) - js_min(js_min(color[0], color[1]), color[2]);
}

static void clip_color(double *color) {
	double red = color[0], green = color[1], blue = color[2], luminosity = get_luminosity(color);
	double lowest = js_min(js_min(red, green), blue), highest = js_max(js_max(red, green), blue);
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
	return js_min((value - ranges[index]) * ranges[index + 1], (value - ranges[index + 3]) * ranges[index + 2]);
}

static double measure_blend_ranges(const double *source, const double *destination, double destination_alpha,
								   const double *ranges) {
	double top = measure_range(get_luminosity(source), ranges, 0);
	top = js_min(top, measure_range(source[0], ranges, 8));
	top = js_min(top, measure_range(source[1], ranges, 16));
	top = js_min(top, measure_range(source[2], ranges, 24));
	double below = measure_range(get_luminosity(destination), ranges, 4);
	below = js_min(below, measure_range(destination[0], ranges, 12));
	below = js_min(below, measure_range(destination[1], ranges, 20));
	below = js_min(below, measure_range(destination[2], ranges, 28));
	below = js_max(below, 1 - destination_alpha);
	double factor = js_min(top, below);
	return factor < 0 ? 0 : factor > 1 ? 1 : factor;
}

static void read_color(uint32_t pixel, double *color) {
	color[0] = ((pixel >> 16) & 255) * (1.0 / 255);
	color[1] = ((pixel >> 8) & 255) * (1.0 / 255);
	color[2] = (pixel & 255) * (1.0 / 255);
}

uint32_t compose_normal_pixel(uint32_t source, uint32_t destination, double opacity) {
	unsigned source_alpha = source >> 24;
	if (!source_alpha)
		return destination;
	if (source_alpha == 255 && opacity == 1)
		return source;
	double source_weight = source_alpha * opacity * (1.0 / 255);
	double destination_alpha = (destination >> 24) * (1.0 / 255);
	double destination_weight = destination_alpha * (1 - source_weight);
	double alpha = source_weight + destination_weight;
	if (alpha == 0)
		return 0;
	double reciprocal = 1 / alpha;
	uint32_t pixel = (uint32_t)(0.5 + alpha * 255) << 24;
	for (unsigned shift = 0; shift <= 16; shift += 8) {
		double source_channel = (source >> shift) & 255;
		double destination_channel = (destination >> shift) & 255;
		unsigned channel =
			0.5 + (source_channel * source_weight + destination_channel * destination_weight) * reciprocal;
		pixel |= channel << shift;
	}
	return pixel;
}

static uint32_t blend_normal_pixel(const BlendKernel *kernel, uint32_t source, uint32_t destination, uint32_t index) {
	if (!kernel->options.preserve_alpha)
		return compose_normal_pixel(source, destination, kernel->opacity);
	if (!(source >> 24))
		return destination;
	double opacity = kernel->opacity, destination_alpha = (destination >> 24) * (1.0 / 255);
	double below = 1 - opacity, reciprocal = 1 / (opacity + below);
	uint32_t pixel = (uint32_t)round_half_up(destination_alpha * 255) << 24;
	for (unsigned shift = 0; shift <= 16; shift += 8) {
		double top = (source >> shift) & 255, bottom = (destination >> shift) & 255;
		pixel |= ((uint32_t)round_half_up((top * opacity + bottom * below) * reciprocal) & 255) << shift;
	}
	return pixel;
}

static uint32_t blend_dissolve_pixel(const BlendKernel *kernel, uint32_t source, uint32_t destination, uint32_t index) {
	double preserve = kernel->options.preserve_alpha ? 1 : 0;
	double scale = floor(kernel->opacity * (256.0 * 256 * 256 / 255) + .5);
	double share = preserve * 255 + (1 - preserve) * (source >> 24);
	if ((hash_dissolve_position(index) & 16777215) >= share * scale)
		return destination;
	return (source & 16777215) | (uint32_t)to_int32(preserve * (destination >> 24) + (1 - preserve) * 255) << 24;
}

static uint32_t blend_separable_pixel(const BlendKernel *kernel, uint32_t source, uint32_t destination,
									  uint32_t index) {
	if (!(source >> 24))
		return destination;
	double preserve = kernel->options.preserve_alpha ? 1 : 0, fill = kernel->options.fill;
	double top_alpha = kernel->opacity, bottom_alpha = 1;
	double top[3], bottom[3];
	read_color(source, top);
	read_color(destination, bottom);
	if (!preserve) {
		top_alpha = (source >> 24) * ((1.0 / 255) * kernel->opacity);
		bottom_alpha = (destination >> 24) * (1.0 / 255);
	}
	if (kernel->options.ranges)
		top_alpha *= measure_blend_ranges(top, bottom, bottom_alpha, kernel->options.ranges);
	double below = bottom_alpha * (1 - top_alpha), total = top_alpha + below;
	double scale = total == 0 ? 0 : 255 / total, weight = kernel->options.style ? 1 : top_alpha;
	double mixed_fill = (1 + top_alpha - weight) * fill;
	uint32_t pixel = 0;
	for (int channel = 0; channel < 3; channel++) {
		double value = ((1 - bottom_alpha) * top_alpha * top[channel] + (1 - weight) * bottom_alpha * bottom[channel] +
						weight * bottom_alpha * kernel->channel(top[channel], bottom[channel], mixed_fill)) *
					   scale;
		pixel |= ((uint32_t)round_half_up(value) & 255) << (16 - channel * 8);
	}
	double alpha = top_alpha * fill + bottom_alpha * (1 - top_alpha * fill);
	return pixel | (uint32_t)round_half_up(preserve * (destination >> 24) + (1 - preserve) * round_half_up(alpha * 255))
					   << 24;
}

static uint32_t blend_color_pixel(const BlendKernel *kernel, uint32_t source, uint32_t destination, uint32_t index) {
	double preserve = kernel->options.preserve_alpha ? 1 : 0;
	double top_alpha = kernel->opacity, bottom_alpha = 1;
	double top[3], bottom[3], mixed[3];
	read_color(source, top);
	read_color(destination, bottom);
	if (!preserve) {
		top_alpha = (source >> 24) * ((1.0 / 255) * kernel->opacity);
		bottom_alpha = (destination >> 24) * (1.0 / 255);
	}
	if (kernel->options.ranges)
		top_alpha *= measure_blend_ranges(top, bottom, bottom_alpha, kernel->options.ranges);
	double below = bottom_alpha * (1 - top_alpha), total = top_alpha + below, scale = 255 / total;
	kernel->color(top, bottom, mixed);
	uint32_t pixel = 0;
	for (int channel = 0; channel < 3; channel++) {
		double value = (((1 - bottom_alpha) * top[channel] + bottom_alpha * mixed[channel]) * top_alpha +
						bottom[channel] * below) *
					   scale;
		pixel |= ((uint32_t)to_int32(value) & 255) << (16 - channel * 8);
	}
	double alpha = preserve * (destination >> 24) + (1 - preserve) * floor(total * 255 + .5);
	return pixel | (uint32_t)to_int32(alpha) << 24;
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
