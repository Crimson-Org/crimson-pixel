#include <limits.h>
#include <math.h>
#include <string.h>

#include "adjustments/adjustment-kernels.h"
#include "color/color-spaces.h"
#include "descriptor.h"
#include "psd-output.h"
#include "lang/en.h"

/* Most adjustment blocks start with a sixteen bit version, and many values are sixteen bit numbers */
#define VERSION_BYTES sizeof(guint16)
#define VALUE_BYTES sizeof(guint16)

/* Hue and saturation: version, colorize flag and a pad, the colorize shifts, the master shifts, then six ranges */
#define HUE_VERSION 2
#define HUE_COLORIZE 2
#define HUE_COLORIZE_SHIFTS 4
#define HUE_MASTER_SHIFTS 10
#define HUE_RANGES_START 16
#define HUE_SHIFT_COUNT (HUE_RANGE_VALUES - HUE_SHIFT)
#define HUE_RANGE_BYTES (HUE_RANGE_VALUES * VALUE_BYTES)
#define HUE_BLOCK_BYTES (HUE_RANGES_START + (HUE_RANGES - 1) * HUE_RANGE_BYTES)

/* Curves: a mapped flag, a pad, a version and a mask of the stored channels, then each stored channel */
#define CURVES_VERSION 1
#define CURVES_VERSION_AT 2
#define CURVES_MASK_AT 3
#define CURVES_CHANNELS_AT 7
#define CURVES_ALL_CHANNELS ((1 << TONE_CHANNELS) - 1)
#define CURVE_POINT_BYTES (2 * VALUE_BYTES)

/* Descriptor blocks start with their descriptor version, after a version of their own for some */
#define DESCRIPTOR_AT sizeof(guint32)
#define VERSIONED_DESCRIPTOR_AT (VERSION_BYTES + sizeof(guint32))

/* Photo filter: version 3 keeps an XYZ colour in 16.16 fixed point, version 2 a Lab colour in hundredths */
#define FILTER_XYZ_VERSION 3
#define FILTER_LAB_VERSION 2
#define FILTER_LAB_SPACE 7
#define FILTER_XYZ_SCALE 32768.0
#define FILTER_LAB_SCALE 100
#define FILTER_XYZ_BYTES (RGB_CHANNELS * sizeof(guint32))
#define FILTER_LAB_BYTES (VALUE_BYTES + RGB_CHANNELS * VALUE_BYTES)
#define FILTER_BLOCK_BYTES (VERSION_BYTES + FILTER_LAB_BYTES + VALUE_BYTES + sizeof(guint32) + 1)

/* Gradient stops: a location and midpoint, then a colour space, four colour values and two unused bytes, or an
 * opacity */
#define STOP_PLACE_BYTES (2 * sizeof(guint32))
#define COLOR_SPACE_AT STOP_PLACE_BYTES
#define COLOR_VALUES_AT (STOP_PLACE_BYTES + VALUE_BYTES)
#define COLOR_VALUES_COUNT 4
#define COLOR_STOP_BYTES (COLOR_VALUES_AT + COLOR_VALUES_COUNT * VALUE_BYTES + VALUE_BYTES)
#define OPACITY_STOP_BYTES (STOP_PLACE_BYTES + VALUE_BYTES)
/* After the stops: the kind, the smoothness and the colour model, where only the solid kind keeps its smoothness */
#define SOLID_GRADIENT 2
#define GRADIENT_SMOOTHNESS_AT 2
#define GRADIENT_COLOR_MODEL_AT 4
#define GRADIENT_COLOR_MODEL 32
#define GRADIENT_TAIL_BYTES 6
#define DEFAULT_SMOOTHNESS GRADIENT_LOCATION_RANGE

/* Gradient map: version, reverse and dither flags, an interpolation method in version 3, the name and the gradient */
#define GRADIENT_MAP_VERSION 3
#define GRADIENT_MAP_REVERSE 2
#define GRADIENT_MAP_DITHER 3
#define GRADIENT_MAP_METHOD_AT 4
#define GRADIENT_MAP_NAME_AT 8
/* The noise settings that follow every gradient map, written as their defaults */
#define NOISE_SEED 1466092501
#define NOISE_ROUGHNESS 2048
#define NOISE_COLOR_MODEL 3
#define NOISE_RANGE_HIGH 32768
#define NOISE_RANGES 4
#define NOISE_SEED_AT 2
#define NOISE_FLAGS_AT 6
#define NOISE_ROUGHNESS_AT 10
#define NOISE_MODEL_AT 14
#define NOISE_LOWS_AT 16
#define NOISE_HIGHS_AT 24
#define NOISE_BYTES 34

/* Selective color: version, method, then an unused range and nine ranges of four inks */
#define SELECTIVE_VERSION 1
#define SELECTIVE_RANGES_AT (VERSION_BYTES + VALUE_BYTES)
#define SELECTIVE_RANGE_BYTES (SELECTIVE_INKS_COUNT * VALUE_BYTES)
#define SELECTIVE_BLOCK_BYTES (SELECTIVE_RANGES_AT + (SELECTIVE_RANGES_COUNT + 1) * SELECTIVE_RANGE_BYTES)

/* Color balance: three tones of three shifts, then the keep luminosity flag */
#define BALANCE_TONE_BYTES (RGB_CHANNELS * VALUE_BYTES)
#define BALANCE_LUMINOSITY_AT (BALANCE_TONES_COUNT * BALANCE_TONE_BYTES)

/* Channel mixer: version, monochrome flag, then the mixer values */
#define MIXER_VERSION 1
#define MIXER_VALUES_AT (VERSION_BYTES + VALUE_BYTES)
#define MIXER_BLOCK_BYTES (MIXER_VALUES_AT + MIXER_VALUES * VALUE_BYTES)

/* Levels: version and 29 channel records, of which only the first four are used */
#define LEVELS_VERSION 2
#define LEVELS_RECORDS 29
#define LEVELS_RECORD_BYTES (LEVELS_VALUES * VALUE_BYTES)

/* Exposure: version, then the exposure, offset and gamma as floats */
#define EXPOSURE_VERSION 1
#define EXPOSURE_BLOCK_BYTES (VERSION_BYTES + 3 * sizeof(float))

/* Brightness and contrast keep their settings in their own block, so their adjustment block is empty */
#define BRIGHTNESS_BLOCK_BYTES 8
#define BRIGHTNESS_SETTINGS_VERSION 1
#define BRIGHTNESS_MEANS 127
/* Posterize and threshold keep one sixteen bit level in four bytes */
#define LEVEL_BLOCK_BYTES 4

/* ============================== Reading ========================================= */

static void read_hue_block(PsdReader *reader, gsize offset, Record *descriptor) {
	gboolean colorize = read_psd_byte(reader, offset + HUE_COLORIZE) == 1;
	set_descriptor_boolean(descriptor, "Clrz", colorize);
	double values[HUE_RANGE_VALUES] = {0};
	gsize shifts = offset + (colorize ? HUE_COLORIZE_SHIFTS : HUE_MASTER_SHIFTS);
	for (int field = 0; field < HUE_SHIFT_COUNT; field++)
		values[HUE_SHIFT + field] = read_i16(reader, shifts + field * VALUE_BYTES);
	set_hue_range(descriptor, 0, values);
	for (int range = 1; range < HUE_RANGES; range++) {
		gsize start = offset + HUE_RANGES_START + (range - 1) * HUE_RANGE_BYTES;
		for (int field = 0; field < HUE_RANGE_VALUES; field++)
			values[field] = read_i16(reader, start + field * VALUE_BYTES);
		set_hue_range(descriptor, range, values);
	}
}

static void read_curves_block(PsdReader *reader, gsize offset, Record *descriptor) {
	gboolean mapped = read_psd_byte(reader, offset) != 0;
	guint32 mask = read_psd_u32(reader, offset + CURVES_MASK_AT);
	gsize position = offset + CURVES_CHANNELS_AT;
	for (int channel = 0; channel < TONE_CHANNELS && !reader->failed; channel++) {
		gboolean stored = mask >> channel & 1;
		if (mapped) {
			double levels[CURVE_LEVELS];
			for (int level = 0; level < CURVE_LEVELS; level++)
				levels[level] = stored ? read_psd_byte(reader, position + level) : level;
			if (stored)
				position += CURVE_LEVELS;
			set_curve_levels(descriptor, channel, levels);
			continue;
		}
		/* A channel that is not stored is the straight line from black to white */
		guint count = stored ? read_psd_u16(reader, position) : 2;
		g_autofree CurvePoint *points = g_new(CurvePoint, MAX(count, 1));
		for (guint point = 0; point < count; point++) {
			gsize at = position + VALUE_BYTES + point * CURVE_POINT_BYTES;
			points[point] = stored ? (CurvePoint){read_psd_u16(reader, at + VALUE_BYTES), read_psd_u16(reader, at), TRUE}
								   : (CurvePoint){point * G_MAXUINT8, point * G_MAXUINT8, TRUE};
		}
		if (stored)
			position += VALUE_BYTES + CURVE_POINT_BYTES * count;
		set_curve_points(descriptor, channel, points, count);
	}
}

static void read_black_white_block(PsdReader *reader, gsize offset, Record *descriptor) {
	g_autoptr(Record) stored = create_object(RECORD_PLAIN);
	read_descriptor(reader, offset + DESCRIPTOR_AT, stored);
	const char *keys[] = {"Bl", "Cyn", "Grn", "Mgnt", "Rd", "Yllw", "tintColor", "useTint"};
	for (guint index = 0; index < G_N_ELEMENTS(keys); index++)
		if (has_record(stored, keys[index]))
			set_record(descriptor, keys[index], copy_record(get_record(stored, keys[index])));
}

static void read_photo_filter_block(PsdReader *reader, gsize offset, Record *descriptor) {
	guint version = read_psd_u16(reader, offset);
	double lab[RGB_CHANNELS];
	gsize color = offset + VERSION_BYTES;
	if (version == FILTER_XYZ_VERSION) {
		double xyz[RGB_CHANNELS];
		for (int channel = 0; channel < RGB_CHANNELS; channel++)
			xyz[channel] = read_psd_u32(reader, color + channel * sizeof(guint32)) / FILTER_XYZ_SCALE;
		convert_xyz_to_lab(xyz[0], xyz[1], xyz[2], lab);
	} else if (version == FILTER_LAB_VERSION && read_psd_u16(reader, color) == FILTER_LAB_SPACE)
		for (int channel = 0; channel < RGB_CHANNELS; channel++)
			lab[channel] = read_i16(reader, color + VALUE_BYTES + channel * VALUE_BYTES) / (double)FILTER_LAB_SCALE;
	else {
		note_unsupported_psd_feature(reader, TEXT_PSD_ADJUSTMENTS);
		return;
	}
	set_record(descriptor, "Clr", create_typed_value("Objc", create_lab_descriptor(lab[0], lab[1], lab[2])));
	gsize position = color + (version == FILTER_XYZ_VERSION ? FILTER_XYZ_BYTES : FILTER_LAB_BYTES + VALUE_BYTES);
	set_descriptor_number(descriptor, "Dnst", "long", read_psd_u32(reader, position));
	set_descriptor_boolean(descriptor, "PrsL", read_psd_byte(reader, position + sizeof(guint32)) == 1);
}

static Record *read_gradient(PsdReader *reader, gsize *offset, const char *name) {
	guint color_count = read_psd_u16(reader, *offset);
	*offset += VALUE_BYTES;
	if (!color_count)
		return NULL;
	g_autofree GradientColorStop *colors = g_new0(GradientColorStop, color_count);
	gboolean readable = TRUE;
	for (guint index = 0; index < color_count && !reader->failed; index++, *offset += COLOR_STOP_BYTES) {
		guint values[COLOR_VALUES_COUNT];
		for (int part = 0; part < COLOR_VALUES_COUNT; part++)
			values[part] = read_psd_u16(reader, *offset + COLOR_VALUES_AT + part * VALUE_BYTES);
		double rgb[RGB_CHANNELS] = {0, 0, 0};
		if (!convert_swatch_color(read_psd_u16(reader, *offset + COLOR_SPACE_AT), values, rgb))
			readable = FALSE;
		colors[index] = (GradientColorStop){read_psd_u32(reader, *offset),
											read_psd_u32(reader, *offset + sizeof(guint32)),
											GRADIENT_USER_COLOR,
											rgb[0],
											rgb[1],
											rgb[2]};
	}
	guint opacity_count = read_psd_u16(reader, *offset);
	*offset += VALUE_BYTES;
	g_autofree GradientOpacityStop *opacities = g_new0(GradientOpacityStop, MAX(opacity_count, 1));
	for (guint index = 0; index < opacity_count && !reader->failed; index++, *offset += OPACITY_STOP_BYTES)
		opacities[index] =
			(GradientOpacityStop){read_psd_u32(reader, *offset), read_psd_u32(reader, *offset + sizeof(guint32)),
								  round(PERCENT * read_psd_u16(reader, *offset + STOP_PLACE_BYTES) / G_MAXUINT8)};
	guint kind = read_psd_u16(reader, *offset);
	int smoothness =
		kind == SOLID_GRADIENT ? (int)read_psd_u16(reader, *offset + GRADIENT_SMOOTHNESS_AT) : DEFAULT_SMOOTHNESS;
	*offset += GRADIENT_TAIL_BYTES;
	if (!readable)
		return NULL;
	const Gradient gradient = {smoothness, colors, color_count, opacities, opacity_count};
	return create_gradient_record(&gradient, name);
}

static void read_gradient_map_block(PsdReader *reader, gsize offset, Record *descriptor) {
	guint version = read_psd_u16(reader, offset);
	set_descriptor_boolean(descriptor, "Rvrs", read_psd_byte(reader, offset + GRADIENT_MAP_REVERSE) == 1);
	set_descriptor_boolean(descriptor, "Dthr", read_psd_byte(reader, offset + GRADIENT_MAP_DITHER) == 1);
	gsize position = offset + GRADIENT_MAP_METHOD_AT;
	if (version >= GRADIENT_MAP_VERSION) {
		char method[PSD_KEY_LENGTH + 1];
		read_key(reader, position, method);
		position += PSD_KEY_LENGTH;
		const char *const methods[] = {"Gcls", "Lnr ", "Perc", "Smoo"};
		GradientInterpolation interpolation = GRADIENT_STRIPES;
		for (guint index = 0; index < G_N_ELEMENTS(methods); index++)
			if (!strcmp(method, methods[index]))
				interpolation = index;
		set_gradient_map_method(descriptor, interpolation);
	}
	g_autofree char *name = read_unicode(reader, position);
	position += sizeof(guint32) + read_psd_u32(reader, position) * sizeof(guint16);
	Record *gradient = read_gradient(reader, &position, name);
	gboolean noise = read_psd_u16(reader, position) == 1;
	if (!gradient || noise) {
		if (gradient)
			free_record(gradient);
		note_unsupported_psd_feature(reader, TEXT_PSD_ADJUSTMENTS);
		return;
	}
	set_record(descriptor, "Grad", create_typed_value("Objc", gradient));
}

static void read_selective_color_block(PsdReader *reader, gsize offset, Record *descriptor) {
	set_absolute_selective_color(descriptor, read_psd_u16(reader, offset + VERSION_BYTES) == 1);
	for (int range = 0; range < SELECTIVE_RANGES_COUNT; range++) {
		double inks[SELECTIVE_INKS_COUNT];
		gsize start = offset + SELECTIVE_RANGES_AT + (range + 1) * SELECTIVE_RANGE_BYTES;
		for (int ink = 0; ink < SELECTIVE_INKS_COUNT; ink++)
			inks[ink] = read_i16(reader, start + ink * VALUE_BYTES);
		set_selective_color(descriptor, range, inks);
	}
}

void read_adjustment_block(PsdReader *reader, const char *key, gsize offset, Record *descriptors) {
	Record *descriptor = create_adjustment_descriptor(key);
	if (!strcmp(key, "brit")) {
		set_descriptor_number(descriptor, "Brgh", "long", read_i16(reader, offset));
		set_descriptor_number(descriptor, "Cntr", "long", read_i16(reader, offset + VALUE_BYTES));
		set_descriptor_boolean(descriptor, "useLegacy", TRUE);
	} else if (!strcmp(key, "vibA")) {
		free_record(descriptor);
		descriptor = create_object(RECORD_PLAIN);
		read_descriptor(reader, offset + DESCRIPTOR_AT, descriptor);
	} else if (!strcmp(key, "hue2"))
		read_hue_block(reader, offset, descriptor);
	else if (!strcmp(key, "curv"))
		read_curves_block(reader, offset, descriptor);
	else if (!strcmp(key, "blwh"))
		read_black_white_block(reader, offset, descriptor);
	else if (!strcmp(key, "phfl"))
		read_photo_filter_block(reader, offset, descriptor);
	else if (!strcmp(key, "grdm"))
		read_gradient_map_block(reader, offset, descriptor);
	else if (!strcmp(key, "selc"))
		read_selective_color_block(reader, offset, descriptor);
	else if (!strcmp(key, "clrL") || !strcmp(key, "rplc")) {
		/* These keep their whole settings in a descriptor after a version */
		free_record(descriptor);
		descriptor = create_object(RECORD_PLAIN);
		read_descriptor(reader, offset + VERSIONED_DESCRIPTOR_AT, descriptor);
	} else if (!strcmp(key, "blnc")) {
		for (int tone = 0; tone < BALANCE_TONES_COUNT; tone++) {
			double values[RGB_CHANNELS];
			for (int channel = 0; channel < RGB_CHANNELS; channel++)
				values[channel] = read_i16(reader, offset + tone * BALANCE_TONE_BYTES + channel * VALUE_BYTES);
			set_color_balance(descriptor, tone, values);
		}
		set_descriptor_boolean(descriptor, "PrsL", read_psd_byte(reader, offset + BALANCE_LUMINOSITY_AT) == 1);
	} else if (!strcmp(key, "mixr")) {
		double values[MIXER_VALUES];
		for (int field = 0; field < MIXER_VALUES; field++)
			values[field] = read_i16(reader, offset + MIXER_VALUES_AT + field * VALUE_BYTES);
		set_channel_mixer(descriptor, read_psd_u16(reader, offset + VERSION_BYTES) == 1, values);
	} else if (!strcmp(key, "levl"))
		for (int channel = 0; channel < TONE_CHANNELS; channel++) {
			double values[LEVELS_VALUES];
			gsize start = offset + VERSION_BYTES + channel * LEVELS_RECORD_BYTES;
			for (int field = 0; field < LEVELS_VALUES; field++)
				values[field] = read_psd_u16(reader, start + field * VALUE_BYTES);
			set_levels_channel(descriptor, channel, values);
		}
	else if (!strcmp(key, "expA")) {
		gsize values = offset + VERSION_BYTES;
		set_descriptor_number(descriptor, "Exps", "doub", read_float(reader, values));
		set_descriptor_number(descriptor, "Ofst", "doub", read_float(reader, values + sizeof(float)));
		set_descriptor_number(descriptor, "gammaCorrection", "doub", read_float(reader, values + 2 * sizeof(float)));
	} else if (!strcmp(key, "post"))
		set_descriptor_number(descriptor, "Lvls", "long", read_psd_u16(reader, offset));
	else if (!strcmp(key, "thrs"))
		set_descriptor_number(descriptor, "Lvl", "long", read_psd_u16(reader, offset));
	set_record(descriptors, key, descriptor);
	/* Settings the kernels cannot build, such as CMYK colors, keep the saved composite in place */
	g_autofree Adjustment *adjustment = g_new(Adjustment, 1);
	if (!build_adjustment(key, descriptor, adjustment))
		note_unsupported_psd_feature(reader, TEXT_PSD_ADJUSTMENTS);
}

/* ============================== Writing ========================================= */

static gsize put_descriptor_block(PsdWriter *writer, gsize offset, const Record *descriptor) {
	g_autoptr(GByteArray) bytes = g_byte_array_new();
	write_u32(bytes, DESCRIPTOR_VERSION);
	write_descriptor(bytes, descriptor);
	return put_bytes(writer, offset, bytes->data, bytes->len);
}

static gsize put_hue_block(PsdWriter *writer, gsize offset, const Record *descriptor) {
	put_u16(writer, offset, HUE_VERSION);
	put_byte(writer, offset + HUE_COLORIZE, get_descriptor_boolean(descriptor, "Clrz", FALSE));
	double values[HUE_RANGE_VALUES];
	get_hue_range(descriptor, 0, values);
	for (int field = 0; field < HUE_SHIFT_COUNT; field++) {
		put_i16(writer, offset + HUE_COLORIZE_SHIFTS + field * VALUE_BYTES, values[HUE_SHIFT + field]);
		put_i16(writer, offset + HUE_MASTER_SHIFTS + field * VALUE_BYTES, values[HUE_SHIFT + field]);
	}
	for (int range = 1; range < HUE_RANGES; range++) {
		get_hue_range(descriptor, range, values);
		gsize start = offset + HUE_RANGES_START + (range - 1) * HUE_RANGE_BYTES;
		for (int field = 0; field < HUE_RANGE_VALUES; field++)
			put_i16(writer, start + field * VALUE_BYTES, values[field]);
	}
	return HUE_BLOCK_BYTES;
}

static gsize put_curves_block(PsdWriter *writer, gsize offset, const Record *descriptor) {
	double levels[TONE_CHANNELS][CURVE_LEVELS];
	GArray *points[TONE_CHANNELS];
	for (int channel = 0; channel < TONE_CHANNELS; channel++)
		points[channel] = get_curve_points(descriptor, channel, levels[channel]);
	gboolean mapped = points[0] == NULL;
	put_byte(writer, offset, mapped);
	put_byte(writer, offset + 1, 0);
	put_byte(writer, offset + CURVES_VERSION_AT, CURVES_VERSION);
	put_u32(writer, offset + CURVES_MASK_AT, CURVES_ALL_CHANNELS);
	gsize position = offset + CURVES_CHANNELS_AT;
	for (int channel = 0; channel < TONE_CHANNELS; channel++) {
		if (mapped) {
			for (int level = 0; level < CURVE_LEVELS; level++)
				put_byte(writer, position + level, wrap_byte(levels[channel][level]));
			position += CURVE_LEVELS;
		} else if (points[channel]) {
			guint count = points[channel]->len;
			put_u16(writer, position, count);
			for (guint index = 0; index < count; index++) {
				const CurvePoint *point = &g_array_index(points[channel], CurvePoint, index);
				gsize at = position + VALUE_BYTES + index * CURVE_POINT_BYTES;
				put_i16(writer, at, point->output);
				put_i16(writer, at + VALUE_BYTES, point->input);
			}
			position += VALUE_BYTES + CURVE_POINT_BYTES * count;
		}
		if (points[channel])
			g_array_free(points[channel], TRUE);
	}
	return position - offset;
}

static gsize put_black_white_block(PsdWriter *writer, gsize offset, const Record *descriptor) {
	g_autoptr(Record) settings = create_object(RECORD_PLAIN);
	set_string(settings, "classID", "null");
	set_descriptor_number(settings, "bwPresetKind", "long", 1);
	set_record(settings, "blackAndWhitePresetFileName", create_typed_value("TEXT", create_string("")));
	const char *keys[] = {"Bl", "Cyn", "Grn", "Mgnt", "Rd", "Yllw", "tintColor", "useTint"};
	for (guint index = 0; index < G_N_ELEMENTS(keys); index++)
		if (has_record(descriptor, keys[index]))
			set_record(settings, keys[index], copy_record(get_record(descriptor, keys[index])));
	return put_descriptor_block(writer, offset, settings);
}

static gsize put_photo_filter_block(PsdWriter *writer, gsize offset, const Record *descriptor) {
	Record *color = get_record(get_record(descriptor, "Clr"), "v");
	gsize position = offset + VERSION_BYTES;
	put_u16(writer, offset, FILTER_LAB_VERSION);
	put_u16(writer, position, FILTER_LAB_SPACE);
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		put_i16(writer, position + VALUE_BYTES + channel * VALUE_BYTES,
				floor(get_descriptor_number(color, LAB_KEYS[channel], 0) * FILTER_LAB_SCALE + .5));
	position += FILTER_LAB_BYTES;
	put_u16(writer, position, 0);
	position += VALUE_BYTES;
	put_u32(writer, position, get_descriptor_number(descriptor, "Dnst", DEFAULT_FILTER_DENSITY));
	put_byte(writer, position + sizeof(guint32), get_descriptor_boolean(descriptor, "PrsL", TRUE));
	return FILTER_BLOCK_BYTES;
}

static gsize put_gradient(PsdWriter *writer, gsize offset, const Gradient *gradient) {
	put_u16(writer, offset, gradient->color_count);
	offset += VALUE_BYTES;
	for (int index = 0; index < gradient->color_count; index++, offset += COLOR_STOP_BYTES) {
		const GradientColorStop *stop = &gradient->colors[index];
		put_u32(writer, offset, stop->location);
		put_u32(writer, offset + sizeof(guint32), stop->midpoint);
		put_u16(writer, offset + COLOR_SPACE_AT, 0);
		const double channels[COLOR_VALUES_COUNT] = {stop->red, stop->green, stop->blue, 0};
		for (int channel = 0; channel < COLOR_VALUES_COUNT; channel++)
			put_i16(writer, offset + COLOR_VALUES_AT + channel * VALUE_BYTES,
					floor(G_MAXUINT16 * (channels[channel] / G_MAXUINT8) + .5));
	}
	put_u16(writer, offset, gradient->opacity_count);
	offset += VALUE_BYTES;
	for (int index = 0; index < gradient->opacity_count; index++, offset += OPACITY_STOP_BYTES) {
		const GradientOpacityStop *stop = &gradient->opacities[index];
		put_u32(writer, offset, stop->location);
		put_u32(writer, offset + sizeof(guint32), stop->midpoint);
		put_i16(writer, offset + STOP_PLACE_BYTES, floor(G_MAXUINT8 * stop->opacity / PERCENT + .5));
	}
	put_u16(writer, offset, SOLID_GRADIENT);
	put_u16(writer, offset + GRADIENT_SMOOTHNESS_AT, gradient->smoothness);
	put_u16(writer, offset + GRADIENT_COLOR_MODEL_AT, GRADIENT_COLOR_MODEL);
	return offset + GRADIENT_TAIL_BYTES;
}

static gsize put_gradient_map_block(PsdWriter *writer, gsize offset, const Record *descriptor) {
	const char *const methods[] = {"Gcls", "Lnr ", "Perc", "Smoo", "\0\0\fm"};
	Record *record = get_record(get_record(descriptor, "Grad"), "v");
	g_autoptr(Gradient) gradient = read_gradient_record(record);
	put_u16(writer, offset, GRADIENT_MAP_VERSION);
	put_byte(writer, offset + GRADIENT_MAP_REVERSE, get_descriptor_boolean(descriptor, "Rvrs", FALSE));
	put_byte(writer, offset + GRADIENT_MAP_DITHER, get_descriptor_boolean(descriptor, "Dthr", FALSE));
	put_bytes(writer, offset + GRADIENT_MAP_METHOD_AT, (const guint8 *)methods[get_gradient_map_method(descriptor)],
			  PSD_KEY_LENGTH);
	const char *name = get_string(get_record(record, "Nm"), "v");
	gsize position = offset + GRADIENT_MAP_NAME_AT + put_counted_text(writer, offset + GRADIENT_MAP_NAME_AT, name);
	position = put_gradient(writer, position, gradient ? gradient : get_two_color_gradient());
	put_u16(writer, position, gradient ? 0 : 1);
	put_u32(writer, position + NOISE_SEED_AT, NOISE_SEED);
	put_u16(writer, position + NOISE_FLAGS_AT, 0);
	put_u16(writer, position + NOISE_FLAGS_AT + VALUE_BYTES, 0);
	put_u32(writer, position + NOISE_ROUGHNESS_AT, NOISE_ROUGHNESS);
	put_u16(writer, position + NOISE_MODEL_AT, NOISE_COLOR_MODEL);
	for (int index = 0; index < NOISE_RANGES; index++) {
		put_u16(writer, position + NOISE_LOWS_AT + index * VALUE_BYTES, 0);
		put_u16(writer, position + NOISE_HIGHS_AT + index * VALUE_BYTES, NOISE_RANGE_HIGH);
	}
	ensure_size(writer, position + NOISE_BYTES);
	return position + NOISE_BYTES - offset;
}

static gsize put_selective_color_block(PsdWriter *writer, gsize offset, const Record *descriptor) {
	put_u16(writer, offset, SELECTIVE_VERSION);
	put_u16(writer, offset + VERSION_BYTES, is_absolute_selective_color(descriptor));
	for (int range = -1; range < SELECTIVE_RANGES_COUNT; range++) {
		double inks[SELECTIVE_INKS_COUNT] = {0, 0, 0, 0};
		if (range >= 0)
			get_selective_color(descriptor, range, inks);
		gsize start = offset + SELECTIVE_RANGES_AT + (range + 1) * SELECTIVE_RANGE_BYTES;
		for (int ink = 0; ink < SELECTIVE_INKS_COUNT; ink++)
			put_i16(writer, start + ink * VALUE_BYTES, inks[ink]);
	}
	return SELECTIVE_BLOCK_BYTES;
}

gsize put_adjustment(PsdWriter *writer, gsize offset, const char *key, const Record *descriptor) {
	if (!strcmp(key, "brit")) {
		/* The settings go in the CgEd block instead */
		ensure_size(writer, offset + BRIGHTNESS_BLOCK_BYTES);
		return BRIGHTNESS_BLOCK_BYTES;
	}
	if (!strcmp(key, "vibA"))
		return put_descriptor_block(writer, offset, descriptor);
	if (!strcmp(key, "curv"))
		return put_curves_block(writer, offset, descriptor);
	if (!strcmp(key, "blwh"))
		return put_black_white_block(writer, offset, descriptor);
	if (!strcmp(key, "phfl"))
		return put_photo_filter_block(writer, offset, descriptor);
	if (!strcmp(key, "grdm"))
		return put_gradient_map_block(writer, offset, descriptor);
	if (!strcmp(key, "selc"))
		return put_selective_color_block(writer, offset, descriptor);
	if (!strcmp(key, "clrL") || !strcmp(key, "rplc")) {
		put_u16(writer, offset, 1);
		return VERSION_BYTES + put_descriptor_block(writer, offset + VERSION_BYTES, descriptor);
	}
	if (!strcmp(key, "hue2"))
		return put_hue_block(writer, offset, descriptor);
	if (!strcmp(key, "blnc")) {
		for (int tone = 0; tone < BALANCE_TONES_COUNT; tone++) {
			double values[RGB_CHANNELS];
			get_color_balance(descriptor, tone, values);
			for (int channel = 0; channel < RGB_CHANNELS; channel++)
				put_i16(writer, offset + tone * BALANCE_TONE_BYTES + channel * VALUE_BYTES, values[channel]);
		}
		put_byte(writer, offset + BALANCE_LUMINOSITY_AT, get_descriptor_boolean(descriptor, "PrsL", TRUE));
		return BALANCE_LUMINOSITY_AT + 1;
	}
	if (!strcmp(key, "mixr")) {
		double values[MIXER_VALUES];
		put_u16(writer, offset, MIXER_VERSION);
		put_u16(writer, offset + VERSION_BYTES, get_channel_mixer(descriptor, values));
		for (int field = 0; field < MIXER_VALUES; field++)
			put_i16(writer, offset + MIXER_VALUES_AT + field * VALUE_BYTES, values[field]);
		return MIXER_BLOCK_BYTES;
	}
	if (!strcmp(key, "levl")) {
		put_u16(writer, offset, LEVELS_VERSION);
		for (int channel = 0; channel < LEVELS_RECORDS; channel++) {
			double values[LEVELS_VALUES];
			/* Channels past the used ones have no settings, so they get the defaults */
			get_levels_channel(descriptor, channel, values);
			gsize start = offset + VERSION_BYTES + channel * LEVELS_RECORD_BYTES;
			for (int field = 0; field < LEVELS_VALUES; field++)
				put_u16(writer, start + field * VALUE_BYTES, values[field]);
		}
		return VERSION_BYTES + LEVELS_RECORDS * LEVELS_RECORD_BYTES;
	}
	if (!strcmp(key, "expA")) {
		gsize values = offset + VERSION_BYTES;
		put_u16(writer, offset, EXPOSURE_VERSION);
		put_float(writer, values, get_descriptor_number(descriptor, "Exps", 0));
		put_float(writer, values + sizeof(float), get_descriptor_number(descriptor, "Ofst", 0));
		put_float(writer, values + 2 * sizeof(float), get_descriptor_number(descriptor, "gammaCorrection", 1));
		return EXPOSURE_BLOCK_BYTES;
	}
	if (!strcmp(key, "nvrt"))
		return 0;
	put_u16(writer, offset, get_descriptor_number(descriptor, !strcmp(key, "post") ? "Lvls" : "Lvl", 0));
	ensure_size(writer, offset + LEVEL_BLOCK_BYTES);
	return VALUE_BYTES;
}

gsize put_brightness_settings(PsdWriter *writer, gsize offset, const Record *brightness) {
	g_autoptr(Record) settings = create_object(RECORD_PLAIN);
	set_string(settings, "classID", "null");
	set_descriptor_number(settings, "Vrsn", "long", BRIGHTNESS_SETTINGS_VERSION);
	set_descriptor_number(settings, "Brgh", "long", get_descriptor_number(brightness, "Brgh", 0));
	set_descriptor_number(settings, "Cntr", "long", get_descriptor_number(brightness, "Cntr", 0));
	set_descriptor_number(settings, "means", "long", BRIGHTNESS_MEANS);
	set_descriptor_boolean(settings, "Lab", FALSE);
	set_descriptor_boolean(settings, "useLegacy", get_descriptor_boolean(brightness, "useLegacy", FALSE));
	set_descriptor_boolean(settings, "Auto", TRUE);
	return put_descriptor_block(writer, offset, settings);
}
