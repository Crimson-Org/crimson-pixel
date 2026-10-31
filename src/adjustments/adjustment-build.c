#include <math.h>
#include <string.h>

#include "adjustment-kernels.h"
#include "color/color-spaces.h"
#include "color/gradient.h"

/* Replace color fuzziness runs to 200, scaled so 150 covers a whole unit */
#define FUZZINESS_SCALE 150
#define POSTERIZE_DIVISOR 255.001

/* ============================== Curves =========================================== */

/* Green and blue get the same curve as red */
static void copy_red_curve(Adjustment *adjustment) {
	memcpy(adjustment->green, adjustment->red, CURVE_LEVELS * sizeof *adjustment->red);
	memcpy(adjustment->blue, adjustment->red, CURVE_LEVELS * sizeof *adjustment->red);
}

/* Clamping keeps a NaN, which the eight-bit conversion then turns into zero */
static double clamp_unit(double value) {
	if (isnan(value))
		return value;
	return value < 0 ? 0 : value > 1 ? 1 : value;
}

static void multiply_matrices(const double *first, const double *second, double *product) {
	for (int row = 0; row < MATRIX_SIDE; row++)
		for (int column = 0; column < MATRIX_SIDE; column++) {
			const double *left = first + row * MATRIX_SIDE;
			product[row * MATRIX_SIDE + column] = left[0] * second[column] + left[1] * second[MATRIX_SIDE + column] +
												  left[2] * second[2 * MATRIX_SIDE + column] +
												  left[3] * second[3 * MATRIX_SIDE + column];
		}
}

static void scale_matrix(double red, double green, double blue, double *matrix) {
	const double values[MATRIX_VALUES] = {red, 0, 0, 0, 0, green, 0, 0, 0, 0, blue, 0, 0, 0, 0, 1};
	memcpy(matrix, values, sizeof values);
}

static void translate_matrix(double red, double green, double blue, double *matrix) {
	const double values[MATRIX_VALUES] = {1, 0, 0, red, 0, 1, 0, green, 0, 0, 1, blue, 0, 0, 0, 1};
	memcpy(matrix, values, sizeof values);
}

static void transform_curves(Adjustment *curves, const double *matrix) {
	for (int index = 0; index < CURVE_LEVELS; index++) {
		double red = curves->red[index], green = curves->green[index], blue = curves->blue[index];
		curves->red[index] = clamp_unit(matrix[0] * red + matrix[1] * green + matrix[2] * blue + matrix[3]);
		curves->green[index] = clamp_unit(matrix[4] * red + matrix[5] * green + matrix[6] * blue + matrix[7]);
		curves->blue[index] = clamp_unit(matrix[8] * red + matrix[9] * green + matrix[10] * blue + matrix[11]);
	}
}

static void raise_curves(Adjustment *curves, double red, double green, double blue) {
	for (int index = 0; index < CURVE_LEVELS; index++) {
		curves->red[index] = clamp_unit(pow(curves->red[index], red));
		curves->green[index] = clamp_unit(pow(curves->green[index], green));
		curves->blue[index] = clamp_unit(pow(curves->blue[index], blue));
	}
}

static void build_levels_curves(const Record *descriptor, Adjustment *curves) {
	/* Each channel's ranges in unit values, the composite first, and its gamma as a power */
	double black[TONE_CHANNELS], white[TONE_CHANNELS], low[TONE_CHANNELS], high[TONE_CHANNELS], power[TONE_CHANNELS];
	for (int channel = 0; channel < TONE_CHANNELS; channel++) {
		double levels[LEVELS_VALUES];
		get_levels_channel(descriptor, channel, levels);
		black[channel] = byte_to_unit(levels[LEVELS_INPUT_BLACK]);
		white[channel] = byte_to_unit(levels[LEVELS_INPUT_WHITE]);
		low[channel] = byte_to_unit(levels[LEVELS_OUTPUT_BLACK]);
		high[channel] = byte_to_unit(levels[LEVELS_OUTPUT_WHITE]);
		power[channel] = 1 / from_percent(levels[LEVELS_GAMMA]);
	}
	enum { COMPOSITE, RED, GREEN, BLUE };
	double offset = -black[COMPOSITE], factor = 1 / (white[COMPOSITE] - black[COMPOSITE]);
	double scale[MATRIX_VALUES], translate[MATRIX_VALUES], composite[MATRIX_VALUES], channels[MATRIX_VALUES];
	double combined[MATRIX_VALUES];
	scale_matrix(factor, factor, factor, scale);
	translate_matrix(offset, offset, offset, translate);
	multiply_matrices(scale, translate, composite);
	scale_matrix(1 / (white[RED] - black[RED]), 1 / (white[GREEN] - black[GREEN]), 1 / (white[BLUE] - black[BLUE]),
				 scale);
	translate_matrix(-black[RED], -black[GREEN], -black[BLUE], translate);
	multiply_matrices(scale, translate, channels);
	for (int index = 0; index < CURVE_LEVELS; index++)
		curves->red[index] = curves->green[index] = curves->blue[index] = byte_to_unit(index);
	transform_curves(curves, channels);
	raise_curves(curves, power[RED], power[GREEN], power[BLUE]);
	transform_curves(curves, composite);
	raise_curves(curves, power[COMPOSITE], power[COMPOSITE], power[COMPOSITE]);
	offset = low[COMPOSITE];
	factor = high[COMPOSITE] - low[COMPOSITE];
	translate_matrix(offset, offset, offset, translate);
	scale_matrix(factor, factor, factor, scale);
	multiply_matrices(translate, scale, composite);
	scale_matrix(high[RED] - low[RED], high[GREEN] - low[GREEN], high[BLUE] - low[BLUE], scale);
	translate_matrix(low[RED], low[GREEN], low[BLUE], translate);
	multiply_matrices(translate, scale, channels);
	multiply_matrices(composite, channels, combined);
	transform_curves(curves, combined);
}

static void build_exposure_curve(const Record *descriptor, float *curve) {
	double exposure = get_descriptor_number(descriptor, "Exps", 0),
		   offset = get_descriptor_number(descriptor, "Ofst", 0);
	double gamma = get_descriptor_number(descriptor, "gammaCorrection", 1);
	double lift = pow(fabs(offset), 1 / (G_PI / 2));
	for (int index = 0; index < CURVE_LEVELS; index++) {
		double value = byte_to_unit(index);
		if (offset > 0) {
			value = MAX(offset / G_E, value);
			value = value * exp(offset / 1.75 + exposure / G_PI);
			value = (1 - lift) * value + lift * 1;
		} else {
			value = value * exp(-offset * 1.75 + exposure / G_PI);
			value = value + -lift * 1.14;
		}
		curve[index] = clamp_unit(pow(value, 1 / gamma));
	}
}

/* The brightness curves run through four points */
#define BRIGHTNESS_POINTS 4

static void sample_smooth_spline(const double x[BRIGHTNESS_POINTS], const double y[BRIGHTNESS_POINTS], float *curve,
								 int length) {
	CurvePoint points[BRIGHTNESS_POINTS];
	for (int index = 0; index < BRIGHTNESS_POINTS; index++)
		points[index] = (CurvePoint){x[index], y[index], TRUE};
	sample_spline(points, BRIGHTNESS_POINTS, curve, length);
}

static void build_legacy_brightness(Adjustment *adjustment, double brightness, double contrast) {
	double shift = byte_to_unit(brightness), scale = 1 + from_percent(contrast);
	if (scale > 1)
		scale = 1 + tan(G_PI / 2 * contrast / 101);
	double offset = (1 - scale) / 2, translate[MATRIX_VALUES], matrix[MATRIX_VALUES];
	const double stretch[MATRIX_VALUES] = {scale, 0, 0, offset, 0, scale, 0, offset, 0, 0, scale, offset, 0, 0, 0, 1};
	translate_matrix(shift, shift, shift, translate);
	multiply_matrices(stretch, translate, matrix);
	for (int index = 0; index < CURVE_LEVELS; index++) {
		adjustment->red[index] = byte_to_unit(index);
		adjustment->green[index] = adjustment->blue[index] = 0;
	}
	transform_curves(adjustment, matrix);
	copy_red_curve(adjustment);
}

static void build_brightness(Adjustment *adjustment, double brightness, double contrast) {
	const int length = ADJUSTMENT_CURVE_LIMIT;
	double bend = -30 + 60 * (contrast + 100) / 200, amount = from_percent(fabs(brightness));
	const double contrast_x[] = {0, 64, 192, 255}, contrast_y[] = {0, 64 - bend, 192 + bend, 255};
	const double lift_x[] = {0, 130 - amount * 26, 233 - amount * 48, 255};
	const double lift_y[] = {0, 130 + amount * 51, 233 + amount * 10, 255};
	float lift[ADJUSTMENT_CURVE_LIMIT];
	/* The contrast curve waits in the blue curve until the chained result replaces it */
	sample_smooth_spline(contrast_x, contrast_y, adjustment->blue, length);
	sample_smooth_spline(lift_x, lift_y, lift, length);
	double step = 1.0 / length;
	for (int index = 0; index < length; index++) {
		double level = lift[index];
		/* Darkening reads the brightening curve backwards */
		if (brightness < 0) {
			double target = index * step;
			int found = index;
			while (lift[found] > target && found > 1)
				found--;
			level = found * step;
		}
		adjustment->red[index] = adjustment->blue[(int)floor((length - 1) * level + .5)];
	}
	memcpy(adjustment->green, adjustment->red, sizeof adjustment->red);
	memcpy(adjustment->blue, adjustment->red, sizeof adjustment->red);
	adjustment->curve_length = length;
}

/* ============================== Building ======================================== */

static void build_color_balance(const Record *descriptor, Adjustment *adjustment) {
	double shifts[BALANCE_TONES_COUNT][RGB_CHANNELS];
	gboolean preserve = get_descriptor_boolean(descriptor, "PrsL", TRUE);
	for (int tone = 0; tone < BALANCE_TONES_COUNT; tone++) {
		double values[RGB_CHANNELS];
		get_color_balance(descriptor, tone, values);
		for (int channel = 0; channel < RGB_CHANNELS; channel++)
			values[channel] = from_percent(values[channel]);
		double middle =
			preserve ? (MIN(values[0], MIN(values[1], values[2])) + MAX(values[0], MAX(values[1], values[2]))) / 2 : 0;
		for (int channel = 0; channel < RGB_CHANNELS; channel++)
			shifts[tone][channel] = values[channel] - middle;
	}
	/* Each tone bends the curve its own way, the highlights first and the shadows last */
	float *curves[] = {adjustment->red, adjustment->green, adjustment->blue};
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		for (int index = 0; index < CURVE_LEVELS; index++) {
			double value = index * BYTE_UNIT, shift = shifts[BALANCE_HIGHLIGHTS][channel], weight = fabs(shift), bent;
			bent = shift < 0 ? pow(value, G_SQRT2) : 1.63 * (pow(value + .04, .5) - .2);
			value = weight * bent + (1 - weight) * value;
			shift = shifts[BALANCE_MIDTONES][channel];
			weight = fabs(shift);
			bent = shift < 0 ? pow(value, 2) : MIN(2.35 * (pow(value + .09, .5) - .3), pow(value, 1 / 2.0));
			value = weight * bent + (1 - weight) * value;
			shift = shifts[BALANCE_SHADOWS][channel];
			weight = fabs(shift);
			bent = shift < 0 ? (value < .4 ? 0 : pow((value - .4) / .6, G_SQRT2)) : pow(value, G_SQRT2 / 2);
			value = weight * bent + (1 - weight) * value;
			curves[channel][index] = clamp_unit(value);
		}
}

/* Saturation above zero is stretched through a tangent, so the top of the range grows the most */
#define SATURATION_POWER 1.3
static double saturation_stretches[SATURATION_STRETCHES];

static double stretch_saturation(double value) {
	return value < 0 ? value : pow(tan(G_PI / 2 * value), SATURATION_POWER);
}

static double shrink_saturation(double value) {
	if (value < 0)
		return value;
	return atan2(pow(value, 1 / SATURATION_POWER), 1) / (G_PI / 2);
}

static void build_hue_saturation(const Record *descriptor, Adjustment *adjustment) {
	static gsize ready;
	if (g_once_init_enter(&ready)) {
		for (int index = 0; index < SATURATION_STRETCHES; index++)
			saturation_stretches[index] = stretch_saturation(-1 + 2 * index / (SATURATION_STRETCHES - 1.0));
		g_once_init_leave(&ready, 1);
	}
	adjustment->saturation_stretches = saturation_stretches;
	double hue[CURVE_LEVELS], saturation[CURVE_LEVELS], lightness[CURVE_LEVELS], master[HUE_RANGE_VALUES];
	adjustment->colorize = get_descriptor_boolean(descriptor, "Clrz", FALSE);
	get_hue_range(descriptor, 0, master);
	double master_saturation = stretch_saturation(from_percent(master[HUE_SATURATION]));
	for (int index = 0; index < CURVE_LEVELS; index++) {
		hue[index] = adjustment->colorize ? master[HUE_SHIFT] / DEGREES_PER_TURN : byte_to_unit(index);
		saturation[index] = adjustment->colorize ? master_saturation : 0;
		lightness[index] = 0;
	}
	if (!adjustment->colorize) {
		double ranges[HUE_RANGES - 1][HUE_RANGE_VALUES];
		for (int range = 0; range < HUE_RANGES - 1; range++)
			get_hue_range(descriptor, range + 1, ranges[range]);
		for (int index = 0; index < CURVE_LEVELS; index++) {
			double level = hue[index];
			hue[index] += master[HUE_SHIFT] / DEGREES_PER_TURN;
			for (int range = 0; range < HUE_RANGES - 1; range++) {
				/* A range fades in between its first two limits and out between its last two */
				double limits[HUE_LIMITS], weight = 0, degrees = level * DEGREES_PER_TURN;
				memcpy(limits, ranges[range], sizeof limits);
				for (int field = 1; field < HUE_LIMITS; field++)
					if (limits[field] < limits[0])
						limits[field] += DEGREES_PER_TURN;
				if (degrees < limits[0])
					degrees += DEGREES_PER_TURN;
				double rising = (degrees - limits[0]) / (limits[1] - limits[0]);
				double falling = (degrees - limits[2]) / (limits[3] - limits[2]);
				if (rising < 0)
					weight = 0;
				else if (rising < 1)
					weight = rising;
				else if (falling < 0)
					weight = 1;
				else if (falling < 1)
					weight = 1 - falling;
				hue[index] += weight * ranges[range][HUE_SHIFT] / DEGREES_PER_TURN;
				saturation[index] += weight * stretch_saturation(from_percent(ranges[range][HUE_SATURATION]));
				lightness[index] += weight * ranges[range][HUE_LIGHTNESS] / PERCENT;
			}
		}
		for (int index = 0; index < CURVE_LEVELS; index++) {
			saturation[index] = (1 + saturation[index]) * (1 + master_saturation) - 1;
			lightness[index] = MAX(-1, MIN(1, lightness[index]));
		}
	}
	for (int index = 0; index < CURVE_LEVELS; index++) {
		float *entry = adjustment->hue_map + index * HUE_MAP_STRIDE;
		entry[HUE_MAP_HUE] = hue[index];
		entry[HUE_MAP_SATURATION] = shrink_saturation(saturation[index]);
		entry[HUE_MAP_LIGHTNESS] = lightness[index];
		entry[HUE_MAP_STRIDE - 1] = 0;
	}
	double change = from_percent(master[HUE_LIGHTNESS]), amount = fabs(change);
	adjustment->hue_lift = amount * (change < 0 ? 0 : 1);
	adjustment->hue_keep = 1 - amount;
}

static void build_vibrance(const Record *descriptor, Adjustment *adjustment) {
	double vibrance = from_percent(get_descriptor_number(descriptor, "vibrance", 0));
	double saturation = from_percent(get_descriptor_number(descriptor, "Strt", 0));
	double chroma = 1 + vibrance * (vibrance > 0 ? .25 : .5);
	adjustment->vibrance_lightness = .8 + .2 / chroma;
	adjustment->vibrance_chroma = chroma;
	adjustment->vibrance_saturation = 1 + saturation;
	const double yuv[MATRIX_VALUES] = {.299, .587, .114, 0, -.147, -.289, .436, 0, .615, -.515, -.1, 0, 0, 0, 0, 1};
	memcpy(adjustment->to_yuv, yuv, sizeof yuv);
	invert_matrix(yuv, adjustment->from_yuv);
	for (int index = 0; index < CURVE_LEVELS; index++)
		adjustment->linear[index] = pow(index * BYTE_UNIT, VIBRANCE_GAMMA);
}

static void build_channel_mixer(const Record *descriptor, Adjustment *adjustment) {
	double values[MIXER_VALUES];
	gboolean monochrome = get_channel_mixer(descriptor, values);
	for (int output = 0; output < RGB_CHANNELS; output++) {
		const double *source = values + (monochrome ? 0 : output * MIXER_INPUTS_COUNT);
		const int inputs[MIXER_PARTS] = {MIXER_RED, MIXER_GREEN, MIXER_BLUE, MIXER_CONSTANT};
		for (int part = 0; part < MIXER_PARTS; part++)
			adjustment->mixer[output * MIXER_PARTS + part] = from_percent(source[inputs[part]]);
	}
}

static void chain_curves(const float *channel, const float *composite, float *result) {
	for (int index = 0; index < CURVE_LEVELS; index++) {
		double position = unit_to_byte(channel[index]);
		result[index] = composite[isnan(position) ? 0 : (int)position];
	}
}

static gboolean build_curves(const Record *descriptor, Adjustment *adjustment) {
	double levels[CURVE_LEVELS];
	float composite[CURVE_LEVELS], channel[CURVE_LEVELS], *results[] = {adjustment->red, adjustment->green, adjustment->blue};
	g_autoptr(GArray) base = get_curve_points(descriptor, 0, levels);
	if (base && base->len < 2)
		return FALSE;
	if (base)
		sample_spline((const CurvePoint *)base->data, base->len, composite, CURVE_LEVELS);
	else
		for (int level = 0; level < CURVE_LEVELS; level++)
			composite[level] = byte_to_unit(levels[level]);
	for (int index = 1; index < TONE_CHANNELS; index++) {
		g_autoptr(GArray) points = get_curve_points(descriptor, index, levels);
		if (!base != !points || (points && points->len < 2))
			return FALSE;
		if (points)
			sample_spline((const CurvePoint *)points->data, points->len, channel, CURVE_LEVELS);
		else
			for (int level = 0; level < CURVE_LEVELS; level++)
				channel[level] = byte_to_unit(levels[level]);
		chain_curves(channel, composite, results[index - 1]);
	}
	return TRUE;
}

static gboolean build_photo_filter(const Record *descriptor, Adjustment *adjustment) {
	double color[RGB_CHANNELS];
	if (!read_descriptor_color(get_record(get_record(descriptor, "Clr"), "v"), color))
		return FALSE;
	double density = from_percent(get_descriptor_number(descriptor, "Dnst", DEFAULT_FILTER_DENSITY));
	float *curves[] = {adjustment->red, adjustment->green, adjustment->blue};
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		for (int index = 0; index < CURVE_LEVELS; index++) {
			double value = index * BYTE_UNIT, filtered = MAX(0, MIN(1, value * byte_to_unit(color[channel])));
			curves[channel][index] = density * filtered + (1 - density) * value;
		}
	adjustment->preserve_luminosity = get_descriptor_boolean(descriptor, "PrsL", TRUE);
	return TRUE;
}

static gboolean build_black_white(const Record *descriptor, Adjustment *adjustment) {
	for (int index = 0; index < GRAY_WEIGHTS_COUNT; index++)
		adjustment->gray_weights[index] =
			(get_descriptor_number(descriptor, GRAY_WEIGHTS[index], DEFAULT_GRAY_WEIGHT) - DEFAULT_GRAY_WEIGHT) /
			DEFAULT_GRAY_WEIGHT;
	adjustment->tinted = get_descriptor_boolean(descriptor, "useTint", FALSE);
	Record *color = get_record(get_record(descriptor, "tintColor"), "v");
	double tint[RGB_CHANNELS] = {0, 0, 0}, hsv[RGB_CHANNELS], full[RGB_CHANNELS];
	if (color && !read_descriptor_color(color, tint))
		return FALSE;
	convert_rgb_to_hsv(byte_to_unit(tint[0]), byte_to_unit(tint[1]), byte_to_unit(tint[2]), hsv);
	convert_hsl_to_rgb(hsv[0], 1, .5, full);
	double middle = get_luma(full[0], full[1], full[2]), strength = hsv[1] * hsv[2], shadows, highlights;
	if (middle == .5)
		shadows = highlights = .5;
	else {
		shadows = strength * (.5 - middle) / (.5 / middle - 1);
		highlights = 1 - strength * (.5 - middle) - 1 / (2 * (1 - middle));
		highlights /= 1 - 1 / (2 * (1 - middle));
	}
	adjustment->tint_hue = hsv[0];
	adjustment->tint_middle = middle;
	adjustment->tint_strength = strength;
	adjustment->tint_shadows = shadows;
	adjustment->tint_highlights = highlights;
	return TRUE;
}

static void build_selective_color(const Record *descriptor, Adjustment *adjustment) {
	gboolean absolute = is_absolute_selective_color(descriptor);
	memset(adjustment->selective, 0, sizeof adjustment->selective);
	for (int range = 0; range < SELECTIVE_RANGES_COUNT; range++) {
		double inks[SELECTIVE_INKS_COUNT];
		get_selective_color(descriptor, range, inks);
		float *entry = adjustment->selective + range * SELECTIVE_ENTRY;
		double black = from_percent(inks[SELECTIVE_INKS_MIXED]);
		for (int ink = 0; ink < SELECTIVE_INKS_MIXED; ink++) {
			double amount = from_percent(inks[ink]);
			if (absolute) {
				entry[ink] = 1;
				entry[ink + SELECTIVE_INKS_MIXED] = amount * (1 + black) + black;
			} else
				entry[ink] = (1 + amount) * (1 + black);
		}
	}
}

static gboolean read_lab_color(const Record *descriptor, const char *key, double lab[RGB_CHANNELS]) {
	Record *color = get_record(get_record(descriptor, key), "v");
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		lab[channel] = get_descriptor_number(color, LAB_KEYS[channel], NAN);
	return color != NULL;
}

static gboolean build_replace_color(const Record *descriptor, Adjustment *adjustment) {
	if (!read_lab_color(descriptor, "Mnm", adjustment->replace_low) ||
		!read_lab_color(descriptor, "Mxm", adjustment->replace_high))
		return FALSE;
	adjustment->replace_shift[0] = get_descriptor_number(descriptor, "H", 0) / DEGREES_PER_TURN;
	adjustment->replace_shift[1] = from_percent(get_descriptor_number(descriptor, "Strt", 0));
	adjustment->replace_shift[2] = from_percent(get_descriptor_number(descriptor, "Lght", 0));
	adjustment->replace_fuzziness = get_descriptor_number(descriptor, "Fzns", DEFAULT_REPLACE_FUZZINESS) / FUZZINESS_SCALE;
	return TRUE;
}

static gboolean build_gradient_map(const Record *descriptor, Adjustment *adjustment) {
	g_autoptr(Gradient) gradient = read_gradient_record(get_record(get_record(descriptor, "Grad"), "v"));
	if (!gradient)
		return FALSE;
	GradientInterpolation interpolation = get_gradient_map_method(descriptor);
	float lookup[GRADIENT_LOOKUP_SIZE * RGBA_CHANNELS];
	build_gradient_lookup(gradient, get_descriptor_boolean(descriptor, "Rvrs", FALSE), interpolation, 0, 0, lookup);
	for (int index = 0; index < GRADIENT_LOOKUP_SIZE; index++) {
		const float *color = lookup + index * RGBA_CHANNELS;
		adjustment->red[index] = color[0];
		adjustment->green[index] = color[1];
		adjustment->blue[index] = color[2];
	}
	adjustment->curve_length = GRADIENT_LOOKUP_SIZE;
	adjustment->luminance = TRUE;
	return TRUE;
}

static gboolean build_settings(const char *key, const Record *descriptor, Adjustment *adjustment) {
	adjustment->kind = ADJUSTMENT_CURVES;
	adjustment->curve_length = CURVE_LEVELS;
	adjustment->luminance = adjustment->preserve_luminosity = FALSE;
	if (!g_strcmp0(key, "levl"))
		build_levels_curves(descriptor, adjustment);
	else if (!g_strcmp0(key, "curv")) {
		if (!build_curves(descriptor, adjustment))
			return FALSE;
	} else if (!g_strcmp0(key, "phfl")) {
		if (!build_photo_filter(descriptor, adjustment))
			return FALSE;
	} else if (!g_strcmp0(key, "grdm")) {
		if (!build_gradient_map(descriptor, adjustment))
			return FALSE;
	} else if (!g_strcmp0(key, "blwh")) {
		adjustment->kind = ADJUSTMENT_BLACK_WHITE;
		return build_black_white(descriptor, adjustment);
	} else if (!g_strcmp0(key, "selc")) {
		adjustment->kind = ADJUSTMENT_SELECTIVE_COLOR;
		build_selective_color(descriptor, adjustment);
		return TRUE;
	} else if (!g_strcmp0(key, "rplc")) {
		adjustment->kind = ADJUSTMENT_REPLACE_COLOR;
		return build_replace_color(descriptor, adjustment);
	} else if (!g_strcmp0(key, "clrL")) {
		g_autoptr(GBytes) profile = get_color_lookup_profile(descriptor);
		if (profile) {
			adjustment->kind = ADJUSTMENT_COLOR_LOOKUP;
			gsize length;
			const guint8 *data = g_bytes_get_data(profile, &length);
			return sample_color_lookup(data, length, adjustment->color_lookup);
		}
		for (int index = 0; index < CURVE_LEVELS; index++)
			adjustment->red[index] = adjustment->green[index] = adjustment->blue[index] = byte_to_unit(index);
	} else if (!g_strcmp0(key, "brit")) {
		double brightness = get_descriptor_number(descriptor, "Brgh", 0);
		double contrast = get_descriptor_number(descriptor, "Cntr", 0);
		if (get_descriptor_boolean(descriptor, "useLegacy", FALSE))
			build_legacy_brightness(adjustment, brightness, contrast);
		else
			build_brightness(adjustment, brightness, contrast);
	} else if (!g_strcmp0(key, "blnc"))
		build_color_balance(descriptor, adjustment);
	else if (!g_strcmp0(key, "hue2")) {
		adjustment->kind = ADJUSTMENT_HUE_SATURATION;
		build_hue_saturation(descriptor, adjustment);
		return TRUE;
	} else if (!g_strcmp0(key, "vibA")) {
		adjustment->kind = ADJUSTMENT_VIBRANCE;
		build_vibrance(descriptor, adjustment);
		return TRUE;
	} else if (!g_strcmp0(key, "mixr")) {
		adjustment->kind = ADJUSTMENT_MIXER;
		build_channel_mixer(descriptor, adjustment);
		return TRUE;
	} else {
		if (!g_strcmp0(key, "expA"))
			build_exposure_curve(descriptor, adjustment->red);
		else if (!g_strcmp0(key, "nvrt"))
			for (int index = 0; index < CURVE_LEVELS; index++)
				adjustment->red[index] = 1 - byte_to_unit(index);
		else if (!g_strcmp0(key, "post")) {
			/* Dividing by a hair more than 255 keeps white in the top level */
			double levels = get_descriptor_number(descriptor, "Lvls", DEFAULT_POSTERIZE_LEVELS);
			double step = levels / POSTERIZE_DIVISOR, size = 1 / (levels - 1);
			for (int index = 0; index < CURVE_LEVELS; index++)
				adjustment->red[index] = floor(index * step) * size;
		} else if (!g_strcmp0(key, "thrs")) {
			double level = get_descriptor_number(descriptor, "Lvl", DEFAULT_THRESHOLD);
			for (int index = 0; index < CURVE_LEVELS; index++)
				adjustment->red[index] = index >= level ? 1 : 0;
			adjustment->luminance = TRUE;
		} else
			return FALSE;
		copy_red_curve(adjustment);
	}
	return TRUE;
}

gboolean build_adjustment(const char *key, const Record *descriptor, Adjustment *adjustment) {
	if (!build_settings(key, descriptor, adjustment))
		return FALSE;
	prepare_adjustment(adjustment);
	return TRUE;
}
