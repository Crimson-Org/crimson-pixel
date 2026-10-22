#include <math.h>
#include <string.h>

#include "adjustments.h"
#include "descriptor.h"
#include "psd.h"

/* Tagged blocks whose descriptors, engines or closures are not ported yet */
static const struct {
	const char *keys, *feature;
} UNSUPPORTED_BLOCKS[] = {{"lfx2 lmfx lfxs", TEXT_PSD_EFFECTS},
						  {"TySh tySh Txt2", TEXT_PSD_TEXT},
						  {"SoLd SoLE lnk2 lnkD lnk3 lnkE FEid FXid", TEXT_PSD_SMART_OBJECTS},
						  {"vmsk vsms vstk vscg vogk SoCo GdFl PtFl", TEXT_PSD_SHAPES},
						  {"artb artd", TEXT_PSD_ARTBOARDS}};

static const char *LARGE_LENGTH_KEYS =
	"LMsk Lr16 Lr32 Layr Mt16 Mt32 Mtrn Alph FMsk lnk2 lnkE FEid FXid PxSD extn cinf artd pths";

static const char *UNPADDED_KEYS = "luni TySh tySh lfx2 iOpa SoCo vmsk vstk";

/* ============================== Layer records =================================== */

static int read_mask_flags(Record *mask, int flags) {
	set_boolean(mask, "WE", (flags & 1) == 0);
	set_boolean(mask, "isEnabled", (flags >> 1 & 1) == 0);
	set_boolean(mask, "L7", (flags >> 3 & 1) == 1);
	return flags >> 4 & 1;
}

static Record *read_real_mask(PsdReader *reader, gsize offset) {
	Record *mask = create_mask(8);
	read_mask_flags(mask, read_psd_byte(reader, offset));
	set_number(mask, "color", read_psd_byte(reader, offset + 1));
	set_record(mask, "V", read_rectangle(reader, offset + 2));
	return mask;
}

static gboolean has_channel(const Record *layer, int identifier) {
	Record *channels = get_record(layer, "tc");
	for (guint index = 0; index < count_records(channels); index++)
		if (get_number(get_item(channels, index), "id") == identifier)
			return TRUE;
	return FALSE;
}

static gsize read_layer_mask(PsdReader *reader, Record *layer, gsize offset) {
	guint32 length = read_psd_u32(reader, offset);
	offset += 4;
	if (length == 0)
		return offset;
	gsize start = offset;
	Record *mask = create_mask(8);
	set_record(layer, "yy", mask);
	set_record(mask, "V", read_rectangle(reader, offset));
	offset += 16;
	set_number(mask, "color", read_psd_byte(reader, offset));
	offset += 1;
	gboolean parameters = read_mask_flags(mask, read_psd_byte(reader, offset));
	offset += 1;
	if (parameters) {
		if (has_channel(layer, -3)) {
			set_record(layer, "E_", read_real_mask(reader, offset));
			offset += 18;
		}
		gsize parameter_start = offset;
		int present = read_psd_byte(reader, offset++);
		double values[4] = {255, 0, 255, 0};
		if (present & 1)
			values[0] = read_psd_byte(reader, offset++);
		if (present >> 1 & 1) {
			values[1] = read_double(reader, offset);
			offset += 8;
		}
		if (present >> 2 & 1)
			values[2] = read_psd_byte(reader, offset++);
		if (present >> 3 & 1) {
			values[3] = read_double(reader, offset);
			offset += 8;
		}
		Record *parameter_values = create_array();
		for (int index = 0; index < 4; index++)
			append_record(parameter_values, create_number(values[index]));
		set_record(layer, "kz", parameter_values);
		(void)parameter_start;
	} else if (length != 20)
		set_record(layer, "E_", read_real_mask(reader, offset));
	return start + length;
}

static gsize read_blend_ranges(PsdReader *reader, Record *layer, gsize offset) {
	guint32 length = read_psd_u32(reader, offset);
	offset += 4;
	Record *ranges = get_record(layer, "hu");
	for (guint32 index = 0; index < length && !reader->failed; index++) {
		Record *value = create_number(read_psd_byte(reader, offset + index));
		if (index < count_records(ranges)) {
			free_record(ranges->value.array->pdata[index]);
			ranges->value.array->pdata[index] = value;
		} else {
			while (count_records(ranges) < index)
				append_record(ranges, create_null());
			append_record(ranges, value);
		}
	}
	return offset + length;
}

static void note_block_feature(PsdReader *reader, const char *key) {
	for (guint index = 0; index < G_N_ELEMENTS(UNSUPPORTED_BLOCKS); index++)
		if (is_listed_key(UNSUPPORTED_BLOCKS[index].keys, key))
			note_unsupported_psd_feature(reader, UNSUPPORTED_BLOCKS[index].feature);
}

static void read_tagged_block(PsdReader *reader, const char *key, gsize offset, gint64 length, Record *descriptors) {
	if (!strcmp(key, "iOpa") || !strcmp(key, "knko") || !strcmp(key, "infx") || !strcmp(key, "clbl") ||
		!strcmp(key, "lmgm") || !strcmp(key, "vmgm") || !strcmp(key, "fcmy"))
		set_number(descriptors, key, read_psd_byte(reader, offset));
	else if (!strcmp(key, "brst")) {
		Record *channels = create_array();
		for (int index = 0; index < 3; index++)
			append_record(channels, create_number(1));
		for (gint64 index = 0; index < length; index += 4) {
			guint32 channel = read_psd_u32(reader, offset + index);
			if (channel > 65536)
				continue;
			while (count_records(channels) <= channel)
				append_record(channels, create_null());
			free_record(channels->value.array->pdata[channel]);
			channels->value.array->pdata[channel] = create_number(0);
		}
		set_record(descriptors, key, channels);
	} else if (!strcmp(key, "lyid") || !strcmp(key, "sn2P") || !strcmp(key, "vowv") || !strcmp(key, "lyvr") ||
			   !strcmp(key, "lspf"))
		set_number(descriptors, key, read_psd_u32(reader, offset));
	else if (!strcmp(key, "lsct")) {
		Record *section = create_object(RECORD_PLAIN);
		set_number(section, "J", read_psd_u32(reader, offset));
		set_boolean(section, "Zd", FALSE);
		if (length >= 12) {
			char mode[5];
			read_key(reader, offset + 8, mode);
			set_string(section, "qu", mode);
		}
		if (length >= 16)
			set_boolean(section, "Zd", read_psd_u32(reader, offset + 12) == 1);
		set_record(descriptors, key, section);
	} else if (!strcmp(key, "lsdk")) {
		Record *section = create_object(RECORD_PLAIN);
		set_number(section, "J", read_psd_u32(reader, offset));
		set_record(descriptors, "lsct", section);
	} else if (!strcmp(key, "lnsr")) {
		char source[5];
		read_key(reader, offset, source);
		set_string(descriptors, key, source);
	} else if (!strcmp(key, "lclr"))
		set_number(descriptors, key, read_psd_u16(reader, offset));
	else if (!strcmp(key, "luni")) {
		g_autofree char *name = read_unicode(reader, offset);
		set_string(descriptors, key, name);
	} else if (!strcmp(key, "fxrp"))
		set_record(descriptors, key, create_point(read_double(reader, offset), read_double(reader, offset + 8)));
	else if (!strcmp(key, "Lr16") || !strcmp(key, "Lr32"))
		read_layer_records(reader, offset);
	else if (is_ported_adjustment(key))
		read_adjustment_block(reader, key, offset, descriptors);
	else if (!strcmp(key, "CgEd")) {
		Record *settings = create_object(RECORD_PLAIN);
		read_descriptor(reader, offset + 4, settings);
		set_record(descriptors, key, settings);
	} else
		note_block_feature(reader, key);
}

gsize read_tagged_blocks(PsdReader *reader, gsize offset, gsize end, Record *descriptors) {
	while (offset < end && !reader->failed) {
		char signature[5], key[5];
		read_key(reader, offset, signature);
		if (strcmp(signature, "8BIM") && strcmp(signature, "8B64")) {
			fail_psd(reader, TEXT_PSD_DAMAGED);
			return end;
		}
		read_key(reader, offset + 4, key);
		gint64 length = read_psd_u32(reader, offset + 8);
		offset += 12;
		if (reader->large && is_listed_key(LARGE_LENGTH_KEYS, key)) {
			length = (gint32)((guint32)length | read_psd_u32(reader, offset));
			offset += 4;
		}
		if (length < 0 || !has_psd_bytes(reader, offset, length))
			return end;
		if (!strcmp(key, "lrFX") || !strcmp(key, "PlLd")) {
			if (strcmp(key, "lrFX") && length % 4)
				length += 4 - length % 4;
			offset += length;
			continue;
		}
		read_tagged_block(reader, key, offset, length, descriptors);
		offset += length;
		if (!is_listed_key(UNPADDED_KEYS, key) && length % 4)
			offset += 4 - length % 4;
	}
	/* Newer files keep the current Brightness/Contrast settings in CgEd next to the legacy brit block */
	Record *settings = get_record(descriptors, "CgEd"), *brightness = get_record(descriptors, "brit");
	if (settings && brightness) {
		set_descriptor_number(brightness, "Brgh", "long", get_descriptor_number(settings, "Brgh", 0));
		set_descriptor_number(brightness, "Cntr", "long", get_descriptor_number(settings, "Cntr", 0));
		set_descriptor_boolean(brightness, "useLegacy", get_descriptor_boolean(settings, "useLegacy", FALSE));
	}
	if (settings)
		remove_record(descriptors, "CgEd");
	return offset;
}

static gsize read_layer_record(PsdReader *reader, Record *layer, gsize offset) {
	set_record(layer, "V", read_rectangle(reader, offset));
	offset += 16;
	guint channel_count = read_psd_u16(reader, offset);
	offset += 2;
	Record *channels = create_array();
	for (guint index = 0; index < channel_count && !reader->failed; index++) {
		Record *channel = create_object(RECORD_PLAIN);
		set_number(channel, "id", read_i16(reader, offset));
		set_number(channel, "length", read_length(reader, offset + 2));
		append_record(channels, channel);
		offset += reader->large ? 10 : 6;
	}
	set_record(layer, "tc", channels);
	char signature[5], mode[5];
	read_key(reader, offset, signature);
	if (strcmp(signature, "8BIM")) {
		fail_psd(reader, TEXT_PSD_DAMAGED);
		return offset;
	}
	read_key(reader, offset + 4, mode);
	set_string(layer, "qu", mode);
	set_number(layer, "nv", read_psd_byte(reader, offset + 8));
	set_boolean(layer, "jl", read_psd_byte(reader, offset + 9) == 1);
	set_number(layer, "NF", read_psd_byte(reader, offset + 10));
	offset += 12;
	guint32 extra_length = read_psd_u32(reader, offset);
	offset += 4;
	gsize extra_start = offset;
	offset = read_layer_mask(reader, layer, offset);
	offset = read_blend_ranges(reader, layer, offset);
	gsize name_length;
	g_autofree char *name = read_pascal(reader, offset, &name_length);
	set_string(layer, "name", name);
	offset += name_length;
	if (name_length % 4)
		offset += 4 - name_length % 4;
	Record *descriptors = get_record(layer, "add");
	offset = read_tagged_blocks(reader, offset, extra_start + extra_length, descriptors);
	Record *section = get_record(descriptors, "lsct");
	if (section && section->type == RECORD_OBJECT) {
		if (get_string(section, "qu"))
			set_string(layer, "qu", get_string(section, "qu"));
		Record *grouped = get_record(section, "Zd");
		if (grouped)
			set_record(layer, "Zd", copy_record(grouped));
		else
			remove_record(layer, "Zd");
		set_number(descriptors, "lsct", get_number(section, "J"));
	}
	return offset;
}

static Record *get_layer_mask_target(Record *layer, int identifier) {
	if (identifier == -3)
		return get_record(layer, "E_");
	if (identifier == -2)
		return get_record(layer, "yy");
	return layer;
}

static gsize read_layer_channels(PsdReader *reader, Record *layer, gsize offset) {
	Record *record = reader->document->record;
	int mode = get_number(record, "Ia");
	unsigned depth = get_number(record, "depth"), file_depth = get_number(record, "g_");
	Record *channels = get_record(layer, "tc"), *color[4] = {0};
	g_autoptr(GPtrArray) owned = g_ptr_array_new_with_free_func((GDestroyNotify)free_record);
	for (guint index = 0; index < count_records(channels) && !reader->failed; index++) {
		Record *channel = get_item(channels, index);
		int identifier = get_number(channel, "id");
		gint64 length = get_number(channel, "length");
		Record *mask = get_record(layer, "yy");
		if (identifier == -2 && (!mask || mask->type == RECORD_NULL))
			identifier = -1;
		Record *target = get_layer_mask_target(layer, identifier);
		Record *extent = get_record(target, "V");
		double width = MAX(0, get_number(extent, "s")), height = MAX(0, get_number(extent, "T"));
		Record *samples = read_psd_channel(reader, file_depth, width, height, offset, length);
		if (length < 0 || !has_psd_bytes(reader, offset, length))
			return offset;
		offset += length;
		if (!samples)
			return offset;
		(void)depth;
		if (identifier == -3 || identifier == -2)
			set_record(target, "C", samples);
		else if (identifier >= -1 && identifier <= 2) {
			int slot = identifier == -1 ? 3 : identifier;
			if (color[slot])
				g_ptr_array_remove(owned, color[slot]);
			color[slot] = samples;
			g_ptr_array_add(owned, samples);
		} else
			free_record(samples);
	}
	if (mode == 1)
		color[1] = color[2] = color[0];
	else if (mode != 3)
		color[0] = color[1] = color[2] = color[3] = NULL;
	unsigned layer_depth = get_number(record, "depth");
	Record *extent = get_record(layer, "V");
	double area = get_number(extent, "s") * get_number(extent, "T");
	Record *buffer = create_channels(MAX(0, area * 4), layer_depth);
	if (!buffer) {
		fail_psd(reader, TEXT_IMAGE_ALLOCATION_FAILED);
		return offset;
	}
	double maximum = layer_depth == 32 ? 1 : channel_maximum(layer_depth);
	gsize pixels = buffer->value.buffer.length / 4;
	for (int slot = 0; slot < 4; slot++) {
		if (color[slot])
			interleave_psd_channel(color[slot], buffer, slot);
		else if (slot == 3 && color[0])
			for (gsize index = 0; index < MIN(pixels, color[0]->value.buffer.length); index++)
				write_channel(buffer, index * 4 + 3, maximum);
		else if (slot > 0 && slot < 3 && color[0])
			interleave_psd_channel(color[0], buffer, slot);
	}
	set_record(layer, "buffer", buffer);
	return offset;
}

gsize read_layer_records(PsdReader *reader, gsize offset) {
	Record *layers = get_record(reader->document->record, "Z");
	int count = abs(read_i16(reader, offset));
	offset += 2;
	g_autoptr(GPtrArray) created = g_ptr_array_new();
	for (int index = 0; index < count && !reader->failed; index++) {
		if (g_cancellable_set_error_if_cancelled(reader->cancellable, reader->error)) {
			reader->failed = TRUE;
			return offset;
		}
		Record *layer = create_layer_record();
		offset = read_layer_record(reader, layer, offset);
		double section = get_number(get_record(layer, "add"), "lsct");
		if (section != 1 && section != 2 && !g_strcmp0(get_string(layer, "qu"), "pass"))
			set_string(layer, "qu", "norm");
		if ((guint)index < count_records(layers)) {
			free_record(layers->value.array->pdata[index]);
			layers->value.array->pdata[index] = layer;
		} else
			append_record(layers, layer);
		g_ptr_array_add(created, layer);
	}
	for (guint index = 0; index < created->len && !reader->failed; index++)
		offset = read_layer_channels(reader, g_ptr_array_index(created, index), offset);
	return offset;
}

gsize read_layer_and_mask_information(PsdReader *reader, gsize offset) {
	gsize start = offset, length_size = reader->large ? 8 : 4;
	gint64 length = read_length(reader, offset);
	offset += length_size;
	if (length == 0)
		return start + length_size;
	if (length < 0 || !has_psd_bytes(reader, offset, length))
		return offset;
	gint64 layer_length = read_length(reader, offset);
	offset += length_size;
	if (layer_length != 0)
		read_layer_records(reader, offset);
	offset += layer_length;
	offset += 4 + read_psd_u32(reader, offset);
	read_tagged_blocks(reader, offset, start + length_size + length, get_record(reader->document->record, "add"));
	return start + length_size + length;
}
