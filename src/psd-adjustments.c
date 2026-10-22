#include <math.h>
#include <string.h>

#include "adjustment-kernels.h"
#include "color-spaces.h"
#include "descriptor.h"
#include "psd-output.h"
#include "../lang/en.h"

/* ============================== Reading ========================================= */

static void read_hue_block(PsdReader *reader, gsize offset, Record *descriptor) {
	gboolean colorize = read_psd_byte(reader, offset + 2) == 1;
	set_descriptor_boolean(descriptor, "Clrz", colorize);
	double values[7] = {0};
	for (int field = 0; field < 3; field++)
		values[4 + field] = read_i16(reader, offset + (colorize ? 4 : 10) + field * 2);
	set_hue_range(descriptor, 0, values);
	for (int range = 1; range < 7; range++) {
		gsize start = offset + 16 + (range - 1) * 14;
		for (int field = 0; field < 7; field++)
			values[field] = read_i16(reader, start + field * 2);
		set_hue_range(descriptor, range, values);
	}
}

static void read_curves_block(PsdReader *reader, gsize offset, Record *descriptor) {
	gboolean mapped = read_psd_byte(reader, offset) != 0;
	guint32 mask = read_psd_u32(reader, offset + 3);
	gsize position = offset + 7;
	for (int channel = 0; channel < 4 && !reader->failed; channel++) {
		gboolean stored = mask >> channel & 1;
		if (mapped) {
			double levels[256];
			for (int level = 0; level < 256; level++)
				levels[level] = stored ? read_psd_byte(reader, position + level) : level;
			if (stored)
				position += 256;
			set_curve_levels(descriptor, channel, levels);
			continue;
		}
		guint count = stored ? read_psd_u16(reader, position) : 2;
		g_autofree CurvePoint *points = g_new(CurvePoint, MAX(count, 1));
		for (guint point = 0; point < count; point++)
			points[point] = stored ? (CurvePoint){read_psd_u16(reader, position + 4 + point * 4),
												  read_psd_u16(reader, position + 2 + point * 4), TRUE}
								   : (CurvePoint){point * 255, point * 255, TRUE};
		if (stored)
			position += 2 + 4 * count;
		set_curve_points(descriptor, channel, points, count);
	}
}

static void read_black_white_block(PsdReader *reader, gsize offset, Record *descriptor) {
	g_autoptr(Record) stored = create_object(RECORD_PLAIN);
	read_descriptor(reader, offset + 4, stored);
	const char *keys[] = {"Bl", "Cyn", "Grn", "Mgnt", "Rd", "Yllw", "tintColor", "useTint"};
	for (guint index = 0; index < G_N_ELEMENTS(keys); index++)
		if (has_record(stored, keys[index]))
			set_record(descriptor, keys[index], copy_record(get_record(stored, keys[index])));
}

static void read_photo_filter_block(PsdReader *reader, gsize offset, Record *descriptor) {
	guint version = read_psd_u16(reader, offset);
	double lab[3];
	if (version == 3) {
		double xyz[3];
		for (int channel = 0; channel < 3; channel++)
			xyz[channel] = read_psd_u32(reader, offset + 2 + channel * 4) / 32768.0;
		convert_xyz_to_lab(xyz[0], xyz[1], xyz[2], lab);
	} else if (version == 2 && read_psd_u16(reader, offset + 2) == 7)
		for (int channel = 0; channel < 3; channel++)
			lab[channel] = read_i16(reader, offset + 4 + channel * 2) / 100.0;
	else {
		note_unsupported_psd_feature(reader, TEXT_PSD_ADJUSTMENTS);
		return;
	}
	set_record(descriptor, "Clr", create_typed_value("Objc", create_lab_descriptor(lab[0], lab[1], lab[2])));
	gsize position = offset + 2 + (version == 3 ? 12 : 10);
	set_descriptor_number(descriptor, "Dnst", "long", read_psd_u32(reader, position));
	set_descriptor_boolean(descriptor, "PrsL", read_psd_byte(reader, position + 4) == 1);
}

static Record *read_gradient(PsdReader *reader, gsize *offset, const char *name) {
	guint color_count = read_psd_u16(reader, *offset);
	*offset += 2;
	if (!color_count)
		return NULL;
	g_autofree GradientColorStop *colors = g_new0(GradientColorStop, color_count);
	gboolean readable = TRUE;
	for (guint index = 0; index < color_count && !reader->failed; index++, *offset += 20) {
		guint values[4];
		for (int part = 0; part < 4; part++)
			values[part] = read_psd_u16(reader, *offset + 10 + part * 2);
		double rgb[3] = {0, 0, 0};
		if (!convert_swatch_color(read_psd_u16(reader, *offset + 8), values, rgb))
			readable = FALSE;
		colors[index] = (GradientColorStop){read_psd_u32(reader, *offset),
											read_psd_u32(reader, *offset + 4),
											GRADIENT_USER_COLOR,
											rgb[0],
											rgb[1],
											rgb[2]};
	}
	guint opacity_count = read_psd_u16(reader, *offset);
	*offset += 2;
	g_autofree GradientOpacityStop *opacities = g_new0(GradientOpacityStop, MAX(opacity_count, 1));
	for (guint index = 0; index < opacity_count && !reader->failed; index++, *offset += 10)
		opacities[index] = (GradientOpacityStop){read_psd_u32(reader, *offset), read_psd_u32(reader, *offset + 4),
												 round(100.0 * read_psd_u16(reader, *offset + 8) / 255)};
	guint kind = read_psd_u16(reader, *offset);
	int smoothness = kind == 2 ? (int)read_psd_u16(reader, *offset + 2) : 4096;
	*offset += 6;
	if (!readable)
		return NULL;
	const Gradient gradient = {smoothness, colors, color_count, opacities, opacity_count};
	return create_gradient_record(&gradient, name);
}

static void read_gradient_map_block(PsdReader *reader, gsize offset, Record *descriptor) {
	guint version = read_psd_u16(reader, offset);
	set_descriptor_boolean(descriptor, "Rvrs", read_psd_byte(reader, offset + 2) == 1);
	set_descriptor_boolean(descriptor, "Dthr", read_psd_byte(reader, offset + 3) == 1);
	gsize position = offset + 4;
	if (version >= 3) {
		char method[5];
		read_key(reader, position, method);
		position += 4;
		const char *const methods[] = {"Gcls", "Lnr ", "Perc", "Smoo"};
		GradientInterpolation interpolation = GRADIENT_STRIPES;
		for (guint index = 0; index < G_N_ELEMENTS(methods); index++)
			if (!strcmp(method, methods[index]))
				interpolation = index;
		set_gradient_map_method(descriptor, interpolation);
	}
	g_autofree char *name = read_unicode(reader, position);
	position += 4 + read_psd_u32(reader, position) * 2;
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
	set_absolute_selective_color(descriptor, read_psd_u16(reader, offset + 2) == 1);
	for (int range = 0; range < 9; range++) {
		double inks[4];
		for (int ink = 0; ink < 4; ink++)
			inks[ink] = read_i16(reader, offset + 4 + (range + 1) * 8 + ink * 2);
		set_selective_color(descriptor, range, inks);
	}
}

void read_adjustment_block(PsdReader *reader, const char *key, gsize offset, Record *descriptors) {
	Record *descriptor = create_adjustment_descriptor(key);
	if (!strcmp(key, "brit")) {
		set_descriptor_number(descriptor, "Brgh", "long", read_i16(reader, offset));
		set_descriptor_number(descriptor, "Cntr", "long", read_i16(reader, offset + 2));
		set_descriptor_boolean(descriptor, "useLegacy", TRUE);
	} else if (!strcmp(key, "vibA")) {
		free_record(descriptor);
		descriptor = create_object(RECORD_PLAIN);
		read_descriptor(reader, offset + 4, descriptor);
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
		read_descriptor(reader, offset + 6, descriptor);
	} else if (!strcmp(key, "blnc")) {
		for (int tone = 0; tone < 3; tone++) {
			double values[3];
			for (int channel = 0; channel < 3; channel++)
				values[channel] = read_i16(reader, offset + tone * 6 + channel * 2);
			set_color_balance(descriptor, tone, values);
		}
		set_descriptor_boolean(descriptor, "PrsL", read_psd_byte(reader, offset + 18) == 1);
	} else if (!strcmp(key, "mixr")) {
		double values[20];
		for (int field = 0; field < 20; field++)
			values[field] = read_i16(reader, offset + 4 + field * 2);
		set_channel_mixer(descriptor, read_psd_u16(reader, offset + 2) == 1, values);
	} else if (!strcmp(key, "levl"))
		for (int channel = 0; channel < 4; channel++) {
			double values[5];
			for (int field = 0; field < 5; field++)
				values[field] = read_psd_u16(reader, offset + 2 + channel * 10 + field * 2);
			set_levels_channel(descriptor, channel, values);
		}
	else if (!strcmp(key, "expA")) {
		set_descriptor_number(descriptor, "Exps", "doub", read_float(reader, offset + 2));
		set_descriptor_number(descriptor, "Ofst", "doub", read_float(reader, offset + 6));
		set_descriptor_number(descriptor, "gammaCorrection", "doub", read_float(reader, offset + 10));
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
	write_u32(bytes, 16);
	write_descriptor(bytes, descriptor);
	return put_bytes(writer, offset, bytes->data, bytes->len);
}

static gsize put_hue_block(PsdWriter *writer, gsize offset, const Record *descriptor) {
	put_u16(writer, offset, 2);
	put_byte(writer, offset + 2, get_descriptor_boolean(descriptor, "Clrz", FALSE));
	double values[7];
	get_hue_range(descriptor, 0, values);
	for (int field = 0; field < 3; field++) {
		put_i16(writer, offset + 4 + field * 2, values[4 + field]);
		put_i16(writer, offset + 10 + field * 2, values[4 + field]);
	}
	for (int range = 1; range < 7; range++) {
		get_hue_range(descriptor, range, values);
		for (int field = 0; field < 7; field++)
			put_i16(writer, offset + 16 + (range - 1) * 14 + field * 2, values[field]);
	}
	return 2 + 2 + 12 + 6 * 14;
}

static gsize put_curves_block(PsdWriter *writer, gsize offset, const Record *descriptor) {
	double levels[4][256];
	GArray *points[4];
	for (int channel = 0; channel < 4; channel++)
		points[channel] = get_curve_points(descriptor, channel, levels[channel]);
	gboolean mapped = points[0] == NULL;
	put_byte(writer, offset, mapped);
	put_byte(writer, offset + 1, 0);
	put_byte(writer, offset + 2, 1);
	put_u32(writer, offset + 3, 15);
	gsize position = offset + 7;
	for (int channel = 0; channel < 4; channel++) {
		if (mapped) {
			for (int level = 0; level < 256; level++)
				put_byte(writer, position + level, (guint)(gint64)fmod(trunc(levels[channel][level]), 256));
			position += 256;
		} else if (points[channel]) {
			guint count = points[channel]->len;
			put_u16(writer, position, count);
			for (guint index = 0; index < count; index++) {
				const CurvePoint *point = &g_array_index(points[channel], CurvePoint, index);
				put_i16(writer, position + 2 + index * 4, point->output);
				put_i16(writer, position + 4 + index * 4, point->input);
			}
			position += 2 + 4 * count;
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
	const char *keys[] = {"Lmnc", "A", "B"};
	put_u16(writer, offset, 2);
	put_u16(writer, offset + 2, 7);
	for (int channel = 0; channel < 3; channel++)
		put_i16(writer, offset + 4 + channel * 2, floor(get_descriptor_number(color, keys[channel], 0) * 100 + .5));
	put_u16(writer, offset + 10, 0);
	put_u32(writer, offset + 12, get_descriptor_number(descriptor, "Dnst", 80));
	put_byte(writer, offset + 16, get_descriptor_boolean(descriptor, "PrsL", TRUE));
	return 17;
}

static gsize put_gradient(PsdWriter *writer, gsize offset, const Gradient *gradient) {
	put_u16(writer, offset, gradient->color_count);
	offset += 2;
	for (int index = 0; index < gradient->color_count; index++, offset += 20) {
		const GradientColorStop *stop = &gradient->colors[index];
		put_u32(writer, offset, stop->location);
		put_u32(writer, offset + 4, stop->midpoint);
		put_u16(writer, offset + 8, 0);
		const double channels[4] = {stop->red, stop->green, stop->blue, 0};
		for (int channel = 0; channel < 4; channel++)
			put_i16(writer, offset + 10 + channel * 2, floor(65535 * (channels[channel] / 255) + .5));
	}
	put_u16(writer, offset, gradient->opacity_count);
	offset += 2;
	for (int index = 0; index < gradient->opacity_count; index++, offset += 10) {
		const GradientOpacityStop *stop = &gradient->opacities[index];
		put_u32(writer, offset, stop->location);
		put_u32(writer, offset + 4, stop->midpoint);
		put_i16(writer, offset + 8, floor(255 * stop->opacity / 100 + .5));
	}
	put_u16(writer, offset, 2);
	put_u16(writer, offset + 2, gradient->smoothness);
	put_u16(writer, offset + 4, 32);
	return offset + 6;
}

static gsize put_gradient_map_block(PsdWriter *writer, gsize offset, const Record *descriptor) {
	const char *const methods[] = {"Gcls", "Lnr ", "Perc", "Smoo", "\0\0\fm"};
	Record *record = get_record(get_record(descriptor, "Grad"), "v");
	g_autoptr(Gradient) gradient = read_gradient_record(record);
	put_u16(writer, offset, 3);
	put_byte(writer, offset + 2, get_descriptor_boolean(descriptor, "Rvrs", FALSE));
	put_byte(writer, offset + 3, get_descriptor_boolean(descriptor, "Dthr", FALSE));
	put_bytes(writer, offset + 4, (const guint8 *)methods[get_gradient_map_method(descriptor)], 4);
	gsize position = offset + 8 + put_counted_text(writer, offset + 8, get_string(get_record(record, "Nm"), "v"));
	position = put_gradient(writer, position, gradient ? gradient : get_two_color_gradient());
	put_u16(writer, position, gradient ? 0 : 1);
	put_u32(writer, position + 2, 1466092501);
	put_u16(writer, position + 6, 0);
	put_u16(writer, position + 8, 0);
	put_u32(writer, position + 10, 2048);
	put_u16(writer, position + 14, 3);
	for (int index = 0; index < 4; index++) {
		put_u16(writer, position + 16 + index * 2, 0);
		put_u16(writer, position + 24 + index * 2, 32768);
	}
	ensure_size(writer, position + 34);
	return position + 34 - offset;
}

static gsize put_selective_color_block(PsdWriter *writer, gsize offset, const Record *descriptor) {
	put_u16(writer, offset, 1);
	put_u16(writer, offset + 2, is_absolute_selective_color(descriptor));
	for (int range = -1; range < 9; range++) {
		double inks[4] = {0, 0, 0, 0};
		if (range >= 0)
			get_selective_color(descriptor, range, inks);
		for (int ink = 0; ink < 4; ink++)
			put_i16(writer, offset + 4 + (range + 1) * 8 + ink * 2, inks[ink]);
	}
	return 84;
}

gsize put_adjustment(PsdWriter *writer, gsize offset, const char *key, const Record *descriptor) {
	if (!strcmp(key, "brit")) {
		/* The settings go in the CgEd block instead */
		ensure_size(writer, offset + 8);
		return 8;
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
		return 2 + put_descriptor_block(writer, offset + 2, descriptor);
	}
	if (!strcmp(key, "hue2"))
		return put_hue_block(writer, offset, descriptor);
	if (!strcmp(key, "blnc")) {
		for (int tone = 0; tone < 3; tone++) {
			double values[3];
			get_color_balance(descriptor, tone, values);
			for (int channel = 0; channel < 3; channel++)
				put_i16(writer, offset + tone * 6 + channel * 2, values[channel]);
		}
		put_byte(writer, offset + 18, get_descriptor_boolean(descriptor, "PrsL", TRUE));
		return 19;
	}
	if (!strcmp(key, "mixr")) {
		double values[20];
		put_u16(writer, offset, 1);
		put_u16(writer, offset + 2, get_channel_mixer(descriptor, values));
		for (int field = 0; field < 20; field++)
			put_i16(writer, offset + 4 + field * 2, values[field]);
		return 44;
	}
	if (!strcmp(key, "levl")) {
		put_u16(writer, offset, 2);
		for (int channel = 0; channel < 29; channel++) {
			double values[5] = {0, 255, 0, 255, 100};
			if (channel < 4)
				get_levels_channel(descriptor, channel, values);
			for (int field = 0; field < 5; field++)
				put_u16(writer, offset + 2 + channel * 10 + field * 2, values[field]);
		}
		return 2 + 29 * 10;
	}
	if (!strcmp(key, "expA")) {
		put_u16(writer, offset, 1);
		put_float(writer, offset + 2, get_descriptor_number(descriptor, "Exps", 0));
		put_float(writer, offset + 6, get_descriptor_number(descriptor, "Ofst", 0));
		put_float(writer, offset + 10, get_descriptor_number(descriptor, "gammaCorrection", 1));
		return 14;
	}
	if (!strcmp(key, "nvrt"))
		return 0;
	put_u16(writer, offset, get_descriptor_number(descriptor, !strcmp(key, "post") ? "Lvls" : "Lvl", 0));
	ensure_size(writer, offset + 4);
	return 2;
}

gsize put_brightness_settings(PsdWriter *writer, gsize offset, const Record *brightness) {
	g_autoptr(Record) settings = create_object(RECORD_PLAIN);
	set_string(settings, "classID", "null");
	set_descriptor_number(settings, "Vrsn", "long", 1);
	set_descriptor_number(settings, "Brgh", "long", get_descriptor_number(brightness, "Brgh", 0));
	set_descriptor_number(settings, "Cntr", "long", get_descriptor_number(brightness, "Cntr", 0));
	set_descriptor_number(settings, "means", "long", 127);
	set_descriptor_boolean(settings, "Lab", FALSE);
	set_descriptor_boolean(settings, "useLegacy", get_descriptor_boolean(brightness, "useLegacy", FALSE));
	set_descriptor_boolean(settings, "Auto", TRUE);
	return put_descriptor_block(writer, offset, settings);
}
