#include <string.h>

#include "descriptor.h"

static const char *const *get_reference_fields(const char *type) {
	static const char *const name[] = {"classID", NULL}, *const property[] = {"classID", "keyID", NULL};
	static const char *const enumerated[] = {"classID", "typeID", "enum", NULL};
	if (!strcmp(type, "prop"))
		return property;
	if (!strcmp(type, "Enmr"))
		return enumerated;
	return name;
}

Record *create_typed_value(const char *type, Record *value) {
	Record *item = create_object(RECORD_PLAIN);
	set_string(item, "t", type);
	set_record(item, "v", value);
	return item;
}

/* ============================== Reading ========================================== */

static char *read_descriptor_key(PsdReader *reader, gsize offset) {
	gint32 length = read_i32(reader, offset);
	if (length > 1000) {
		fail_psd(reader, TEXT_PSD_DAMAGED);
		return g_strdup("");
	}
	char *text;
	if (length == 0) {
		char key[5];
		read_key(reader, offset + 4, key);
		text = g_strdup(key);
	} else {
		text = g_malloc0(MAX(length, 0) + 1);
		for (gint32 index = 0; index < length; index++)
			text[index] = read_psd_byte(reader, offset + 4 + index);
		if (!strcmp(text, " JPEG 2000") || !strcmp(text, "neutralizeColor    "))
			return text;
	}
	return g_strstrip(text);
}

static gsize get_key_size(PsdReader *reader, gsize offset) {
	gint32 length = read_i32(reader, offset);
	return length == 0 ? 8 : 4 + length;
}

static char *read_counted_text(PsdReader *reader, gsize offset, glong *units) {
	guint32 count = read_psd_u32(reader, offset);
	*units = count ? count - 1 : 0;
	return read_utf16(reader, offset + 4, *units);
}

static Record *read_descriptor_value(PsdReader *reader, gsize offset, gsize *size);

static gsize read_identifier(PsdReader *reader, gsize offset, Record *value, const char *field) {
	g_autofree char *identifier = read_descriptor_key(reader, offset);
	set_string(value, field, identifier);
	return 4 + MAX(4, strlen(identifier));
}

static Record *read_descriptor_value(PsdReader *reader, gsize offset, gsize *size) {
	gsize start = offset;
	char type[5];
	read_key(reader, offset, type);
	offset += 4;
	Record *item = create_object(RECORD_PLAIN), *value = NULL;
	set_string(item, "t", type);
	if (!strcmp(type, "obj ") || !strcmp(type, "VlLs")) {
		value = create_array();
		guint32 count = read_psd_u32(reader, offset);
		offset += 4;
		for (guint32 index = 0; index < count && !reader->failed; index++) {
			gsize child;
			append_record(value, read_descriptor_value(reader, offset, &child));
			offset += child;
		}
	} else if (!strcmp(type, "UntF")) {
		value = create_object(RECORD_PLAIN);
		char unit[5];
		read_key(reader, offset, unit);
		set_string(value, "type", unit);
		set_number(value, "val", read_double(reader, offset + 4));
		offset += 12;
	} else if (!strcmp(type, "doub")) {
		value = create_number(read_double(reader, offset));
		offset += 8;
	} else if (!strcmp(type, "bool")) {
		value = create_boolean(read_psd_byte(reader, offset) == 1);
		offset += 1;
	} else if (!strcmp(type, "long")) {
		value = create_number(read_i32(reader, offset));
		offset += 4;
	} else if (!strcmp(type, "comp")) {
		value = create_number(read_i32(reader, offset + 4));
		offset += 8;
	} else if (!strcmp(type, "Objc")) {
		value = create_object(RECORD_PLAIN);
		offset += read_descriptor(reader, offset, value);
	} else if (!strcmp(type, "TEXT")) {
		glong units;
		if (read_psd_u32(reader, offset) == 0) {
			value = create_string("");
			offset += 4;
		} else {
			g_autofree char *text = read_counted_text(reader, offset, &units);
			value = create_string(text);
			offset += 4 + units * 2 + 2;
		}
	} else if (!strcmp(type, "enum")) {
		g_autofree char *kind = read_descriptor_key(reader, offset);
		offset += get_key_size(reader, offset);
		g_autofree char *choice = read_descriptor_key(reader, offset);
		offset += get_key_size(reader, offset);
		value = create_object(RECORD_PLAIN);
		set_string(value, kind, choice);
	} else if (!strcmp(type, "tdta")) {
		/* Raw data such as a Color Lookup profile is kept as one byte buffer instead of a record per byte */
		gint32 length = read_i32(reader, offset);
		offset += 4;
		value = has_psd_bytes(reader, offset, MAX(0, length)) ? create_bytes(reader->data + offset, MAX(0, length))
															  : create_bytes(NULL, 0);
		offset += MAX(0, length);
	} else if (!strcmp(type, "Clss") || !strcmp(type, "type") || !strcmp(type, "rele")) {
		glong units;
		g_autofree char *name = read_counted_text(reader, offset, &units);
		offset += 4 + units * 2 + 2;
		value = create_object(RECORD_PLAIN);
		offset += read_identifier(reader, offset, value, "classID");
		if (*name)
			set_string(value, "__name", name);
		if (!strcmp(type, "rele")) {
			set_number(value, "val", read_i32(reader, offset));
			offset += 4;
		}
	} else if (!strcmp(type, "prop") || !strcmp(type, "Enmr") || !strcmp(type, "indx") || !strcmp(type, "name") ||
			   !strcmp(type, "Idnt")) {
		glong units;
		value = create_object(RECORD_PLAIN);
		g_autofree char *name = read_counted_text(reader, offset, &units);
		offset += 4 + units * 2 + 2;
		if (units)
			set_string(value, "unam", name);
		for (const char *const *field = get_reference_fields(type); *field; field++)
			offset += read_identifier(reader, offset, value, *field);
		if (!strcmp(type, "name")) {
			g_autofree char *text = read_counted_text(reader, offset, &units);
			set_string(value, "val", text);
			offset += 4 + units * 2 + 2;
		}
		if (!strcmp(type, "indx") || !strcmp(type, "Idnt")) {
			set_number(value, "val", read_psd_u32(reader, offset));
			offset += 4;
		}
	} else if (!strcmp(type, "alis")) {
		guint32 length = read_psd_u32(reader, offset);
		offset += 4;
		g_autofree char *text = g_malloc0(length + 1);
		for (guint32 index = 0; index < length && !reader->failed; index++)
			text[index] = read_psd_byte(reader, offset + index);
		value = create_string(text);
		offset += length;
	} else {
		fail_psd(reader, TEXT_PSD_DAMAGED);
		value = create_null();
	}
	set_record(item, "v", value);
	*size = offset - start;
	return item;
}

gsize read_descriptor(PsdReader *reader, gsize offset, Record *object) {
	gsize start = offset;
	if (read_psd_u32(reader, offset) == 0)
		offset += 4;
	else {
		glong units;
		g_autofree char *name = read_counted_text(reader, offset, &units);
		offset += 4 + units * 2 + 2;
		if (*name)
			set_string(object, "__name", name);
	}
	g_autofree char *class_identifier = read_descriptor_key(reader, offset);
	set_string(object, "classID", class_identifier);
	offset += get_key_size(reader, offset);
	gint32 count = read_i32(reader, offset);
	offset += 4;
	for (gint32 index = 0; index < count && !reader->failed; index++) {
		g_autofree char *key = read_descriptor_key(reader, offset);
		offset += get_key_size(reader, offset);
		gsize size;
		set_record(object, key, read_descriptor_value(reader, offset, &size));
		offset += size;
	}
	return offset - start;
}

/* ============================== Writing ========================================== */

void write_u8(GByteArray *output, guint value) {
	guint8 byte = value;
	g_byte_array_append(output, &byte, 1);
}

void write_u16(GByteArray *output, guint value) {
	guint8 bytes[2] = {value >> 8 & 255, value & 255};
	g_byte_array_append(output, bytes, 2);
}

void write_u32(GByteArray *output, double value) {
	guint32 bits = (guint32)(gint64)value;
	guint8 bytes[4] = {bits >> 24 & 255, bits >> 16 & 255, bits >> 8 & 255, bits & 255};
	g_byte_array_append(output, bytes, 4);
}

void write_double(GByteArray *output, double value) {
	guint64 bits;
	memcpy(&bits, &value, sizeof bits);
	for (int shift = 56; shift >= 0; shift -= 8)
		write_u8(output, bits >> shift & 255);
}

void write_ascii(GByteArray *output, const char *text) {
	g_byte_array_append(output, (const guint8 *)text, strlen(text));
}

void write_counted_text(GByteArray *output, const char *text, gboolean terminated) {
	glong units = 0;
	g_autofree gunichar2 *utf16 = g_utf8_to_utf16(text, -1, NULL, &units, NULL);
	write_u32(output, units + (terminated ? 1 : 0));
	for (glong index = 0; index < units; index++)
		write_u16(output, utf16[index]);
	if (terminated)
		write_u16(output, 0);
}

static void write_descriptor_key(GByteArray *output, const char *key) {
	static const char *const counted[] = {"warp", "list", "Comp", "None", "xx",	  "xy",	  "yx",	  "yy",	  "tx",
										  "ty",	  "PinP", "PnRt", "PnOv", "PnDp", "xor",  "PuX0", "PuX1", "PuX2",
										  "PuX3", "PuY0", "PuY1", "PuY2", "PuY3", "base", "kana", "ruby", "box",
										  "flow", "time", "hold", "clio", "trim", "then", "else", NULL};
	gboolean with_length = strlen(key) > 4;
	for (const char *const *item = counted; *item && !with_length; item++)
		with_length = !strcmp(*item, key);
	write_u32(output, with_length ? strlen(key) : 0);
	write_ascii(output, key);
	for (gsize index = strlen(key); index < 4 && !with_length; index++)
		write_u8(output, ' ');
}

static void write_descriptor_value(GByteArray *output, const Record *item);

static void write_identifier(GByteArray *output, const char *identifier) {
	gsize start = output->len;
	write_descriptor_key(output, identifier);
	gsize end = start + 4 + MAX(4, strlen(identifier)), written = output->len;
	g_byte_array_set_size(output, end);
	if (end > written)
		memset(output->data + written, 0, end - written);
}

static void write_descriptor_value(GByteArray *output, const Record *item) {
	const char *type = get_string(item, "t");
	const Record *value = get_record(item, "v");
	write_ascii(output, type);
	if (!strcmp(type, "obj ") || !strcmp(type, "VlLs")) {
		write_u32(output, count_records(value));
		for (guint index = 0; index < count_records(value); index++)
			write_descriptor_value(output, get_item(value, index));
	} else if (!strcmp(type, "UntF")) {
		write_ascii(output, get_string(value, "type"));
		write_double(output, get_number(value, "val"));
	} else if (!strcmp(type, "doub"))
		write_double(output, value->value.number);
	else if (!strcmp(type, "bool"))
		write_u8(output, value->value.boolean ? 1 : 0);
	else if (!strcmp(type, "long"))
		write_u32(output, value->value.number);
	else if (!strcmp(type, "comp")) {
		write_u32(output, 0);
		write_u32(output, value->value.number);
	} else if (!strcmp(type, "Objc"))
		write_descriptor(output, value);
	else if (!strcmp(type, "TEXT"))
		write_counted_text(output, value->value.string, TRUE);
	else if (!strcmp(type, "enum")) {
		g_autoptr(GPtrArray) keys = get_record_keys(value);
		const char *kind = g_ptr_array_index(keys, 0);
		write_descriptor_key(output, kind);
		write_descriptor_key(output, get_string(value, kind));
	} else if (!strcmp(type, "tdta")) {
		write_u32(output, value->value.buffer.length);
		g_byte_array_append(output, value->value.buffer.data, value->value.buffer.length);
	} else if (!strcmp(type, "Clss") || !strcmp(type, "type") || !strcmp(type, "rele")) {
		const char *name = get_string(value, "__name");
		write_counted_text(output, name ? name : "", TRUE);
		write_identifier(output, get_string(value, "classID"));
		if (!strcmp(type, "rele"))
			write_u32(output, get_number(value, "val"));
	} else if (!strcmp(type, "prop") || !strcmp(type, "Enmr") || !strcmp(type, "indx") || !strcmp(type, "name") ||
			   !strcmp(type, "Idnt")) {
		const char *name = "";
		if (!strcmp(type, "name") && !g_strcmp0(get_string(value, "classID"), "Grdn"))
			name = "Gradient";
		if (get_string(value, "unam"))
			name = get_string(value, "unam");
		write_counted_text(output, name, TRUE);
		for (const char *const *field = get_reference_fields(type); *field; field++)
			write_identifier(output, get_string(value, *field));
		if (!strcmp(type, "name"))
			write_counted_text(output, get_string(value, "val"), TRUE);
		if (!strcmp(type, "indx") || !strcmp(type, "Idnt"))
			write_u32(output, get_number(value, "val"));
	} else if (!strcmp(type, "alis")) {
		write_u32(output, value->string_length);
		write_ascii(output, value->value.string);
	} else
		g_byte_array_set_size(output, output->len - 4);
}

void write_descriptor(GByteArray *output, const Record *object) {
	const char *name = get_string(object, "__name");
	write_counted_text(output, name ? name : "", TRUE);
	write_descriptor_key(output, get_string(object, "classID"));
	write_u32(output, g_hash_table_size(object->value.object) - 1 - (name ? 1 : 0));
	g_autoptr(GPtrArray) keys = get_record_keys(object);
	for (guint index = 0; index < keys->len; index++) {
		const char *key = g_ptr_array_index(keys, index);
		if (!strcmp(key, "classID") || !strcmp(key, "__name"))
			continue;
		write_descriptor_key(output, key);
		write_descriptor_value(output, get_record(object, key));
	}
}
