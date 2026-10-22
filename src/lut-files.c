#include <math.h>
#include <string.h>

#include "lut-files.h"

static const guint8 LINK_PROFILE_HEADER[] = {
	0,	 0, 14, 204, 65, 68, 66, 69, 4,	 0,	  0,   0,	108, 105, 110, 107, 82, 71,	 66,  32,  82,	71,	 66, 32, 7,
	227, 0, 7,	0,	 27, 0,	 8,	 0,	 6,	 0,	  49,  97,	99,	 115, 112, 0,	0,	0,	 0,	  0,   0,	0,	 0,	 0,	 0,
	0,	 0, 0,	0,	 0,	 0,	 0,	 0,	 0,	 0,	  0,   0,	0,	 0,	  0,   0,	0,	0,	 0,	  0,   246, 214, 0,	 1,	 0,
	0,	 0, 0,	211, 45, 65, 68, 66, 69, 219, 126, 245, 21,	 206, 223, 186, 23, 165, 212, 201, 219, 250, 10, 33, 62};
static const char LINK_PROFILE_NAME[] = "ICC by CrimsonPix";

/* JavaScript parseFloat reads the longest decimal number at the start after white space, or NaN */
static double parse_float(const char *text) {
	if (!text)
		return NAN;
	while (g_ascii_isspace(*text))
		text++;
	const char *end = text;
	if (*end == '+' || *end == '-')
		end++;
	if (g_str_has_prefix(end, "Infinity"))
		return *text == '-' ? -INFINITY : INFINITY;
	const char *digits = end;
	while (g_ascii_isdigit(*end))
		end++;
	if (*end == '.') {
		end++;
		while (g_ascii_isdigit(*end))
			end++;
	}
	if (end == digits || (end == digits + 1 && *digits == '.'))
		return NAN;
	if (*end == 'e' || *end == 'E') {
		const char *exponent = end + 1;
		if (*exponent == '+' || *exponent == '-')
			exponent++;
		if (g_ascii_isdigit(*exponent)) {
			end = exponent;
			while (g_ascii_isdigit(*end))
				end++;
		}
	}
	g_autofree char *number = g_strndup(text, end - text);
	return g_ascii_strtod(number, NULL);
}

/* JavaScript parseInt in base ten */
static double parse_integer(const char *text) {
	if (!text)
		return NAN;
	while (g_ascii_isspace(*text))
		text++;
	double sign = 1, value = 0;
	if (*text == '+' || *text == '-')
		sign = *text++ == '-' ? -1 : 1;
	if (!g_ascii_isdigit(*text))
		return NAN;
	while (g_ascii_isdigit(*text))
		value = value * 10 + (*text++ - '0');
	return sign * value;
}

static char **split_lut_lines(const guint8 *data, gsize length) {
	if (length >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
		data += 3;
		length -= 3;
	}
	g_autofree char *text = g_strndup((const char *)data, length);
	const char *separator = NULL;
	for (gsize index = 0; index < length && !separator; index++) {
		if (text[index] == '\n')
			separator = "\n";
		else if (text[index] == '\r')
			separator = text[index + 1] == '\n' ? "\r\n" : "\r";
	}
	if (!separator)
		return g_strsplit(text, "\n", -1);
	return g_strsplit(text, separator, -1);
}

static double read_token(char **tokens, guint index) {
	return index < g_strv_length(tokens) ? parse_float(tokens[index]) : NAN;
}

static double *reorder_lut(int size, const double *values, gsize count) {
	gsize total = (gsize)size * size * size;
	double *ordered = g_new(double, total * 3);
	for (gsize index = 0; index < total * 3; index++)
		ordered[index] = 0;
	for (int blue = 0; blue < size; blue++)
		for (int green = 0; green < size; green++)
			for (int red = 0; red < size; red++) {
				gsize from = 3 * (red + green * (gsize)size + blue * (gsize)size * size);
				gsize to = 3 * (blue + green * (gsize)size + red * (gsize)size * size);
				for (int channel = 0; channel < 3; channel++)
					ordered[to + channel] = from + channel < count ? values[from + channel] : NAN;
			}
	return ordered;
}

static double *read_cube_file(char **lines, int *size) {
	GArray *values = g_array_new(FALSE, FALSE, sizeof(double));
	for (char **line = lines; *line; line++) {
		if (!**line || **line == '#')
			continue;
		g_auto(GStrv) tokens = g_strsplit(*line, " ", -1);
		if (!strcmp(tokens[0], "LUT_3D_SIZE")) {
			*size = parse_integer(tokens[1]);
			continue;
		}
		if (!strcmp(tokens[0], "TITLE") || !strcmp(tokens[0], "DOMAIN_MIN") || !strcmp(tokens[0], "DOMAIN_MAX"))
			continue;
		for (guint channel = 0; channel < 3; channel++) {
			double value = read_token(tokens, channel);
			g_array_append_val(values, value);
		}
	}
	if (*size < 2 || *size > 256) {
		g_array_free(values, TRUE);
		return NULL;
	}
	double *ordered = reorder_lut(*size, (double *)values->data, values->len);
	g_array_free(values, TRUE);
	return ordered;
}

static double *read_mesh_file(char **lines, int *size) {
	GArray *values = g_array_new(FALSE, FALSE, sizeof(double));
	*size = 0;
	for (char **line = lines; *line; line++) {
		if (!**line || !strcmp(*line, "3DMESH") || g_str_has_prefix(*line, "Mesh") || **line == '#')
			continue;
		g_auto(GStrv) tokens = g_strsplit(*line, " ", -1);
		if (!*size) {
			*size = g_strv_length(tokens);
			continue;
		}
		for (guint channel = 0; channel < 3; channel++) {
			double value = read_token(tokens, channel) * (1 / 4095.0);
			g_array_append_val(values, value);
		}
	}
	gsize total = (gsize)*size * *size * *size * 3;
	if (*size < 2 || *size > 256) {
		g_array_free(values, TRUE);
		return NULL;
	}
	while (values->len < total)
		g_array_append_val(values, (double){NAN});
	return (double *)g_array_free(values, FALSE);
}

/* The text of the element child after skipping the given number of child elements */
static char *find_child_text(const char *start, const char *end, int skipped) {
	const char *position = start;
	for (int child = 0; position && position < end; child++) {
		position = strchr(position, '<');
		if (!position || position >= end || position[1] == '/')
			return NULL;
		const char *name_end = position + 1 + strcspn(position + 1, " />");
		g_autofree char *closing = g_strdup_printf("</%.*s>", (int)(name_end - position - 1), position + 1);
		const char *content = strchr(position, '>'), *close = content ? strstr(content, closing) : NULL;
		if (!close || close > end)
			return NULL;
		if (child == skipped)
			return g_strndup(content + 1, close - content - 1);
		position = close + strlen(closing);
	}
	return NULL;
}

static double *read_look_file(const char *text, int *size) {
	const char *start = strstr(text, "<LUT>"), *end = strstr(text, "</LUT>");
	if (!start || !end)
		return NULL;
	g_autofree char *size_text = find_child_text(start + 5, end, 0), *data_text = find_child_text(start + 5, end, 1);
	if (!size_text || !data_text)
		return NULL;
	g_strstrip(size_text);
	if (*size_text == '"')
		memmove(size_text, size_text + 1, strlen(size_text));
	*size = parse_integer(size_text);
	if (*size < 2 || *size > 256)
		return NULL;
	GString *hex = g_string_new(NULL);
	for (const char *character = data_text; *character; character++)
		if (*character != '"' && !g_ascii_isspace(*character))
			g_string_append_c(hex, *character);
	gsize count = (gsize)*size * *size * *size * 3;
	double *values = g_new(double, count);
	for (gsize index = 0; index < count; index++) {
		guint8 bytes[4];
		for (int part = 0; part < 4; part++) {
			gsize at = (index * 4 + part) * 2;
			int high = at < hex->len ? hex->str[at] : 0, low = at + 1 < hex->len ? hex->str[at + 1] : 0;
			high = high < 58 ? high - 48 : high - 55;
			low = low < 58 ? low - 48 : low - 55;
			bytes[part] = at + 1 < hex->len ? (high << 4) + low : 0;
		}
		float value;
		guint32 bits = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | (guint32)bytes[3] << 24;
		memcpy(&value, &bits, sizeof value);
		values[index] = value;
	}
	g_string_free(hex, TRUE);
	double *ordered = reorder_lut(*size, values, count);
	g_free(values);
	return ordered;
}

static void put_u32(guint8 *data, gsize offset, guint32 value) {
	for (int index = 0; index < 4; index++)
		data[offset + index] = value >> (24 - index * 8) & 255;
}

static GBytes *write_link_profile(int size, const double *values) {
	gsize name_length = strlen(LINK_PROFILE_NAME) + 1, entries = (gsize)size * size * size * 3;
	/* The mluc header fields before the name, and the mAB header, offsets and clut header before the values */
	gsize name_size = 28 + name_length * 2 + 2, table_size = 52 + entries * 2;
	gsize name_padded = (name_size + 3) & ~(gsize)3, table_padded = (table_size + 3) & ~(gsize)3;
	gsize first = ICC_HEADER_SIZE + 4 + 2 * 12, total = first + name_padded + table_padded;
	guint8 *data = g_malloc0(total);
	memcpy(data, LINK_PROFILE_HEADER, sizeof LINK_PROFILE_HEADER);
	put_u32(data, 0, total);
	put_u32(data, ICC_HEADER_SIZE, 2);
	gsize entry = ICC_HEADER_SIZE + 4, offset = first;
	memcpy(data + entry, "desc", 4);
	put_u32(data, entry + 4, offset);
	put_u32(data, entry + 8, name_padded);
	memcpy(data + offset, "mluc", 4);
	put_u32(data, offset + 8, 1);
	put_u32(data, offset + 12, 12);
	memcpy(data + offset + 16, "enUS", 4);
	put_u32(data, offset + 20, name_length * 2 + 2);
	put_u32(data, offset + 24, 28);
	for (gsize index = 0; index < name_length - 1; index++)
		data[offset + 28 + index * 2 + 1] = LINK_PROFILE_NAME[index];
	entry += 12;
	offset += name_padded;
	memcpy(data + entry, "A2B0", 4);
	put_u32(data, entry + 4, offset);
	put_u32(data, entry + 8, table_padded);
	memcpy(data + offset, "mAB ", 4);
	data[offset + 8] = data[offset + 9] = 3;
	put_u32(data, offset + 24, 32);
	gsize table = offset + 32;
	data[table] = data[table + 1] = data[table + 2] = size;
	data[table + 16] = 2;
	for (gsize index = 0; index < entries; index++) {
		double scaled = floor(values[index] * 65535 + .5);
		guint value = isnan(scaled) ? 0 : (guint)MAX(0, MIN(65535, scaled));
		data[table + 20 + index * 2] = value >> 8 & 255;
		data[table + 20 + index * 2 + 1] = value & 255;
	}
	return g_bytes_new_take(data, total);
}

static gboolean contains_bytes(const guint8 *data, gsize length, const char *text) {
	gsize size = strlen(text);
	for (gsize index = 0; index + size <= length; index++)
		if (!memcmp(data + index, text, size))
			return TRUE;
	return FALSE;
}

GBytes *convert_lut_file(const guint8 *data, gsize length) {
	if (length >= 4 && (gsize)((guint32)data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3]) == length)
		return g_bytes_new(data, length);
	int size = 0;
	g_autofree double *values = NULL;
	if (length && data[0] == '<') {
		g_autofree char *text = g_strndup((const char *)data, length);
		values = read_look_file(text, &size);
	} else {
		g_auto(GStrv) lines = split_lut_lines(data, length);
		values =
			contains_bytes(data, length, "LUT_3D_SIZE") ? read_cube_file(lines, &size) : read_mesh_file(lines, &size);
	}
	return values ? write_link_profile(size, values) : NULL;
}
