#include <math.h>
#include <string.h>

#include "adjustment-kernels.h"
#include "color-spaces.h"
#include "gradient.h"

/* ============================== Curves =========================================== */

/* JavaScript Math.max and Math.min keep a NaN, which the eight-bit conversion then turns into zero */
static double clamp_unit(double value) {
	if (isnan(value))
		return value;
	return value < 0 ? 0 : value > 1 ? 1 : value;
}

static void multiply_matrices(const double *first, const double *second, double *product) {
	for (int row = 0; row < 4; row++)
		for (int column = 0; column < 4; column++)
			product[row * 4 + column] = first[row * 4] * second[column] + first[row * 4 + 1] * second[4 + column] +
										first[row * 4 + 2] * second[8 + column] +
										first[row * 4 + 3] * second[12 + column];
}

static void scale_matrix(double red, double green, double blue, double *matrix) {
	const double values[16] = {red, 0, 0, 0, 0, green, 0, 0, 0, 0, blue, 0, 0, 0, 0, 1};
	memcpy(matrix, values, sizeof values);
}

static void translate_matrix(double red, double green, double blue, double *matrix) {
	const double values[16] = {1, 0, 0, red, 0, 1, 0, green, 0, 0, 1, blue, 0, 0, 0, 1};
	memcpy(matrix, values, sizeof values);
}

static void transform_curves(Adjustment *curves, const double *matrix) {
	for (int index = 0; index < 256; index++) {
		double red = curves->red[index], green = curves->green[index], blue = curves->blue[index];
		curves->red[index] = clamp_unit(matrix[0] * red + matrix[1] * green + matrix[2] * blue + matrix[3]);
		curves->green[index] = clamp_unit(matrix[4] * red + matrix[5] * green + matrix[6] * blue + matrix[7]);
		curves->blue[index] = clamp_unit(matrix[8] * red + matrix[9] * green + matrix[10] * blue + matrix[11]);
	}
}

static void raise_curves(Adjustment *curves, double red, double green, double blue) {
	for (int index = 0; index < 256; index++) {
		curves->red[index] = clamp_unit(pow(curves->red[index], red));
		curves->green[index] = clamp_unit(pow(curves->green[index], green));
		curves->blue[index] = clamp_unit(pow(curves->blue[index], blue));
	}
}

static void build_levels_curves(const Record *descriptor, Adjustment *curves) {
	double levels[4][5];
	for (int channel = 0; channel < 4; channel++)
		get_levels_channel(descriptor, channel, levels[channel]);
	double offset = -levels[0][0] / 255, factor = 1 / (levels[0][1] / 255 - levels[0][0] / 255);
	double scale[16], translate[16], composite[16], channels[16], combined[16];
	scale_matrix(factor, factor, factor, scale);
	translate_matrix(offset, offset, offset, translate);
	multiply_matrices(scale, translate, composite);
	scale_matrix(1 / (levels[1][1] / 255 - levels[1][0] / 255), 1 / (levels[2][1] / 255 - levels[2][0] / 255),
				 1 / (levels[3][1] / 255 - levels[3][0] / 255), scale);
	translate_matrix(-levels[1][0] / 255, -levels[2][0] / 255, -levels[3][0] / 255, translate);
	multiply_matrices(scale, translate, channels);
	for (int index = 0; index < 256; index++)
		curves->red[index] = curves->green[index] = curves->blue[index] = index / 255.0;
	transform_curves(curves, channels);
	raise_curves(curves, 1 / (levels[1][4] / 100), 1 / (levels[2][4] / 100), 1 / (levels[3][4] / 100));
	transform_curves(curves, composite);
	double gamma = 1 / (levels[0][4] / 100);
	raise_curves(curves, gamma, gamma, gamma);
	offset = levels[0][2] / 255;
	factor = levels[0][3] / 255 - levels[0][2] / 255;
	translate_matrix(offset, offset, offset, translate);
	scale_matrix(factor, factor, factor, scale);
	multiply_matrices(translate, scale, composite);
	scale_matrix(levels[1][3] / 255 - levels[1][2] / 255, levels[2][3] / 255 - levels[2][2] / 255,
				 levels[3][3] / 255 - levels[3][2] / 255, scale);
	translate_matrix(levels[1][2] / 255, levels[2][2] / 255, levels[3][2] / 255, translate);
	multiply_matrices(translate, scale, channels);
	multiply_matrices(composite, channels, combined);
	transform_curves(curves, combined);
}

static void build_exposure_curve(const Record *descriptor, float *curve) {
	double exposure = get_descriptor_number(descriptor, "Exps", 0),
		   offset = get_descriptor_number(descriptor, "Ofst", 0);
	double gamma = get_descriptor_number(descriptor, "gammaCorrection", 1);
	double lift = pow(fabs(offset), 1 / (G_PI / 2));
	for (int index = 0; index < 256; index++) {
		double value = index / 255.0;
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

static void sample_smooth_spline(const double x[4], const double y[4], float *curve, int length) {
	CurvePoint points[4];
	for (int index = 0; index < 4; index++)
		points[index] = (CurvePoint){x[index], y[index], TRUE};
	sample_spline(points, 4, curve, length);
}

static void build_legacy_brightness(Adjustment *adjustment, double brightness, double contrast) {
	double shift = brightness / 255, scale = 1 + contrast / 100;
	if (scale > 1)
		scale = 1 + tan(G_PI / 2 * contrast / 101);
	double offset = (1 - scale) / 2, translate[16], matrix[16];
	const double stretch[16] = {scale, 0, 0, offset, 0, scale, 0, offset, 0, 0, scale, offset, 0, 0, 0, 1};
	translate_matrix(shift, shift, shift, translate);
	multiply_matrices(stretch, translate, matrix);
	for (int index = 0; index < 256; index++) {
		adjustment->red[index] = index / 255.0;
		adjustment->green[index] = adjustment->blue[index] = 0;
	}
	transform_curves(adjustment, matrix);
	memcpy(adjustment->green, adjustment->red, 256 * sizeof *adjustment->red);
	memcpy(adjustment->blue, adjustment->red, 256 * sizeof *adjustment->red);
}

static void build_brightness(Adjustment *adjustment, double brightness, double contrast) {
	const int length = ADJUSTMENT_CURVE_LIMIT;
	double bend = -30 + 60 * (contrast + 100) / 200, amount = fabs(brightness) / 100;
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
	double shifts[3][3];
	gboolean preserve = get_descriptor_boolean(descriptor, "PrsL", TRUE);
	for (int tone = 0; tone < 3; tone++) {
		double values[3];
		get_color_balance(descriptor, tone, values);
		for (int channel = 0; channel < 3; channel++)
			values[channel] /= 100;
		double middle =
			preserve ? (MIN(values[0], MIN(values[1], values[2])) + MAX(values[0], MAX(values[1], values[2]))) / 2 : 0;
		for (int channel = 0; channel < 3; channel++)
			shifts[tone][channel] = values[channel] - middle;
	}
	float *curves[] = {adjustment->red, adjustment->green, adjustment->blue};
	for (int channel = 0; channel < 3; channel++)
		for (int index = 0; index < 256; index++) {
			double value = index * (1 / 255.0), shift = shifts[2][channel], weight = fabs(shift), bent;
			bent = shift < 0 ? pow(value, G_SQRT2) : 1.63 * (pow(value + .04, .5) - .2);
			value = weight * bent + (1 - weight) * value;
			shift = shifts[1][channel];
			weight = fabs(shift);
			bent = shift < 0 ? pow(value, 2) : MIN(2.35 * (pow(value + .09, .5) - .3), pow(value, 1 / 2.0));
			value = weight * bent + (1 - weight) * value;
			shift = shifts[0][channel];
			weight = fabs(shift);
			bent = shift < 0 ? (value < .4 ? 0 : pow((value - .4) / .6, G_SQRT2)) : pow(value, G_SQRT2 / 2);
			value = weight * bent + (1 - weight) * value;
			curves[channel][index] = clamp_unit(value);
		}
}

static double saturation_stretches[1024];

static double stretch_saturation(double value) {
	return value < 0 ? value : pow(tan(G_PI / 2 * value), 1.3);
}

static double shrink_saturation(double value) {
	if (value < 0)
		return value;
	return atan2(pow(value, 1 / 1.3), 1) / (G_PI / 2);
}

static void build_hue_saturation(const Record *descriptor, Adjustment *adjustment) {
	static gsize ready;
	if (g_once_init_enter(&ready)) {
		for (int index = 0; index < 1024; index++)
			saturation_stretches[index] = stretch_saturation(-1 + 2 * index / 1023.0);
		g_once_init_leave(&ready, 1);
	}
	adjustment->saturation_stretches = saturation_stretches;
	double hue[256], saturation[256], lightness[256], master[7];
	adjustment->colorize = get_descriptor_boolean(descriptor, "Clrz", FALSE);
	get_hue_range(descriptor, 0, master);
	double master_saturation = stretch_saturation(master[5] / 100);
	for (int index = 0; index < 256; index++) {
		hue[index] = adjustment->colorize ? master[4] / 360 : index / 255.0;
		saturation[index] = adjustment->colorize ? master_saturation : 0;
		lightness[index] = 0;
	}
	if (!adjustment->colorize) {
		double ranges[6][7];
		for (int range = 0; range < 6; range++)
			get_hue_range(descriptor, range + 1, ranges[range]);
		for (int index = 0; index < 256; index++) {
			double level = hue[index];
			hue[index] += master[4] / 360;
			for (int range = 0; range < 6; range++) {
				double limits[4], weight = 0, degrees = level * 360;
				memcpy(limits, ranges[range], sizeof limits);
				for (int field = 1; field < 4; field++)
					if (limits[field] < limits[0])
						limits[field] += 360;
				if (degrees < limits[0])
					degrees += 360;
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
				hue[index] += weight * ranges[range][4] / 360;
				saturation[index] += weight * stretch_saturation(ranges[range][5] / 100);
				lightness[index] += weight * ranges[range][6] / 100;
			}
		}
		for (int index = 0; index < 256; index++) {
			saturation[index] = (1 + saturation[index]) * (1 + master_saturation) - 1;
			lightness[index] = MAX(-1, MIN(1, lightness[index]));
		}
	}
	for (int index = 0; index < 256; index++) {
		adjustment->hue_map[index * 4] = hue[index];
		adjustment->hue_map[index * 4 + 1] = shrink_saturation(saturation[index]);
		adjustment->hue_map[index * 4 + 2] = lightness[index];
		adjustment->hue_map[index * 4 + 3] = 0;
	}
	double change = master[6] / 100, amount = fabs(change);
	adjustment->hue_lift = amount * (change < 0 ? 0 : 1);
	adjustment->hue_keep = 1 - amount;
}

static void build_vibrance(const Record *descriptor, Adjustment *adjustment) {
	double vibrance = get_descriptor_number(descriptor, "vibrance", 0) / 100;
	double saturation = get_descriptor_number(descriptor, "Strt", 0) / 100;
	double chroma = 1 + vibrance * (vibrance > 0 ? .25 : .5);
	adjustment->vibrance_lightness = .8 + .2 / chroma;
	adjustment->vibrance_chroma = chroma;
	adjustment->vibrance_saturation = 1 + saturation;
	const double yuv[16] = {.299, .587, .114, 0, -.147, -.289, .436, 0, .615, -.515, -.1, 0, 0, 0, 0, 1};
	memcpy(adjustment->to_yuv, yuv, sizeof yuv);
	invert_matrix(yuv, adjustment->from_yuv);
	const double unit = 1 / 255.0;
	for (int index = 0; index < 256; index++)
		adjustment->linear[index] = pow(index * unit, 2.4);
}

static void build_channel_mixer(const Record *descriptor, Adjustment *adjustment) {
	double values[20];
	gboolean monochrome = get_channel_mixer(descriptor, values);
	for (int output = 0; output < 3; output++) {
		const double *source = values + (monochrome ? 0 : output * 5);
		const int inputs[] = {0, 1, 2, 4};
		for (int part = 0; part < 4; part++)
			adjustment->mixer[output * 4 + part] = source[inputs[part]] / 100;
	}
}

static void chain_curves(const float *channel, const float *composite, float *result) {
	for (int index = 0; index < 256; index++) {
		double position = channel[index] * 255.0;
		result[index] = composite[isnan(position) ? 0 : (int)position];
	}
}

static gboolean build_curves(const Record *descriptor, Adjustment *adjustment) {
	double levels[256];
	float composite[256], channel[256], *results[] = {adjustment->red, adjustment->green, adjustment->blue};
	g_autoptr(GArray) base = get_curve_points(descriptor, 0, levels);
	if (base && base->len < 2)
		return FALSE;
	if (base)
		sample_spline((const CurvePoint *)base->data, base->len, composite, 256);
	else
		for (int level = 0; level < 256; level++)
			composite[level] = levels[level] / 255;
	for (int index = 1; index < 4; index++) {
		g_autoptr(GArray) points = get_curve_points(descriptor, index, levels);
		if (!base != !points || (points && points->len < 2))
			return FALSE;
		if (points)
			sample_spline((const CurvePoint *)points->data, points->len, channel, 256);
		else
			for (int level = 0; level < 256; level++)
				channel[level] = levels[level] / 255;
		chain_curves(channel, composite, results[index - 1]);
	}
	return TRUE;
}

static gboolean build_photo_filter(const Record *descriptor, Adjustment *adjustment) {
	double color[3];
	if (!read_descriptor_color(get_record(get_record(descriptor, "Clr"), "v"), color))
		return FALSE;
	double density = get_descriptor_number(descriptor, "Dnst", 80) / 100;
	float *curves[] = {adjustment->red, adjustment->green, adjustment->blue};
	for (int channel = 0; channel < 3; channel++)
		for (int index = 0; index < 256; index++) {
			double value = index * (1 / 255.0), filtered = MAX(0, MIN(1, value * (color[channel] / 255)));
			curves[channel][index] = density * filtered + (1 - density) * value;
		}
	adjustment->preserve_luminosity = get_descriptor_boolean(descriptor, "PrsL", TRUE);
	return TRUE;
}

static gboolean build_black_white(const Record *descriptor, Adjustment *adjustment) {
	for (int index = 0; index < 6; index++)
		adjustment->gray_weights[index] = (get_descriptor_number(descriptor, GRAY_WEIGHTS[index], 50) - 50) / 50;
	adjustment->tinted = get_descriptor_boolean(descriptor, "useTint", FALSE);
	Record *color = get_record(get_record(descriptor, "tintColor"), "v");
	double tint[3] = {0, 0, 0}, hsv[3], full[3];
	if (color && !read_descriptor_color(color, tint))
		return FALSE;
	convert_rgb_to_hsv(tint[0] / 255, tint[1] / 255, tint[2] / 255, hsv);
	convert_hsl_to_rgb(hsv[0], 1, .5, full);
	double middle = .3 * full[0] + .59 * full[1] + .11 * full[2], strength = hsv[1] * hsv[2], shadows, highlights;
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
	for (int range = 0; range < 9; range++) {
		double inks[4];
		get_selective_color(descriptor, range, inks);
		float *entry = adjustment->selective + range * 6;
		double black = inks[3] / 100;
		for (int ink = 0; ink < 3; ink++) {
			double amount = inks[ink] / 100;
			if (absolute) {
				entry[ink] = 1;
				entry[ink + 3] = amount * (1 + black) + black;
			} else
				entry[ink] = (1 + amount) * (1 + black);
		}
	}
}

static gboolean read_lab_color(const Record *descriptor, const char *key, double lab[3]) {
	Record *color = get_record(get_record(descriptor, key), "v");
	const char *keys[] = {"Lmnc", "A", "B"};
	for (int channel = 0; channel < 3; channel++)
		lab[channel] = get_descriptor_number(color, keys[channel], NAN);
	return color != NULL;
}

static gboolean build_replace_color(const Record *descriptor, Adjustment *adjustment) {
	if (!read_lab_color(descriptor, "Mnm", adjustment->replace_low) ||
		!read_lab_color(descriptor, "Mxm", adjustment->replace_high))
		return FALSE;
	adjustment->replace_shift[0] = get_descriptor_number(descriptor, "H", 0) / 360;
	adjustment->replace_shift[1] = get_descriptor_number(descriptor, "Strt", 0) / 100;
	adjustment->replace_shift[2] = get_descriptor_number(descriptor, "Lght", 0) / 100;
	adjustment->replace_fuzziness = get_descriptor_number(descriptor, "Fzns", 55) / 150;
	return TRUE;
}

static gboolean build_gradient_map(const Record *descriptor, Adjustment *adjustment) {
	g_autoptr(Gradient) gradient = read_gradient_record(get_record(get_record(descriptor, "Grad"), "v"));
	if (!gradient)
		return FALSE;
	GradientInterpolation interpolation = get_gradient_map_method(descriptor);
	float lookup[GRADIENT_LOOKUP_SIZE * 4];
	build_gradient_lookup(gradient, get_descriptor_boolean(descriptor, "Rvrs", FALSE), interpolation, 0, 0, lookup);
	for (int index = 0; index < GRADIENT_LOOKUP_SIZE; index++) {
		adjustment->red[index] = lookup[index * 4];
		adjustment->green[index] = lookup[index * 4 + 1];
		adjustment->blue[index] = lookup[index * 4 + 2];
	}
	adjustment->curve_length = GRADIENT_LOOKUP_SIZE;
	adjustment->luminance = TRUE;
	return TRUE;
}

static gboolean build_settings(const char *key, const Record *descriptor, Adjustment *adjustment) {
	adjustment->kind = ADJUSTMENT_CURVES;
	adjustment->curve_length = 256;
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
		for (int index = 0; index < 256; index++)
			adjustment->red[index] = adjustment->green[index] = adjustment->blue[index] = index / 255.0;
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
			for (int index = 0; index < 256; index++)
				adjustment->red[index] = 1 - index / 255.0;
		else if (!g_strcmp0(key, "post")) {
			double levels = get_descriptor_number(descriptor, "Lvls", 3), step = levels / 255.001,
				   size = 1 / (levels - 1);
			for (int index = 0; index < 256; index++)
				adjustment->red[index] = floor(index * step) * size;
		} else if (!g_strcmp0(key, "thrs")) {
			double level = get_descriptor_number(descriptor, "Lvl", 128);
			for (int index = 0; index < 256; index++)
				adjustment->red[index] = index >= level ? 1 : 0;
			adjustment->luminance = TRUE;
		} else
			return FALSE;
		memcpy(adjustment->green, adjustment->red, 256 * sizeof *adjustment->red);
		memcpy(adjustment->blue, adjustment->red, 256 * sizeof *adjustment->red);
	}
	return TRUE;
}

gboolean build_adjustment(const char *key, const Record *descriptor, Adjustment *adjustment) {
	if (!build_settings(key, descriptor, adjustment))
		return FALSE;
	prepare_adjustment(adjustment);
	return TRUE;
}
