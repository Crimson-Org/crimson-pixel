#include <math.h>
#include <string.h>

#include <gio/gio.h>

#include "common/key-store.h"
#include "color-spaces.h"
#include "swatches.h"
#include "kernels.h"
#include "../lang/en.h"

/* Big endian reads that fail instead of running past the end of a damaged file */
typedef struct {
	const guint8 *bytes;
	gsize length, offset;
	gboolean failed;
} SwatchReader;

static gboolean has_bytes(SwatchReader *reader, gsize count) {
	if (!reader->failed && reader->offset <= reader->length && count <= reader->length - reader->offset)
		return TRUE;
	reader->failed = TRUE;
	return FALSE;
}

static guint read_short(SwatchReader *reader) {
	if (!has_bytes(reader, 2))
		return 0;
	const guint8 *at = reader->bytes + reader->offset;
	reader->offset += 2;
	return at[0] << 8 | at[1];
}

static guint32 read_long(SwatchReader *reader) {
	if (!has_bytes(reader, 4))
		return 0;
	const guint8 *at = reader->bytes + reader->offset;
	reader->offset += 4;
	return (guint32)at[0] << 24 | at[1] << 16 | at[2] << 8 | at[3];
}

static float read_float(SwatchReader *reader) {
	guint32 bits = read_long(reader);
	float value;
	memcpy(&value, &bits, sizeof value);
	return value;
}

static char *read_text(SwatchReader *reader, guint units) {
	if (!has_bytes(reader, (gsize)units * 2))
		return NULL;
	gunichar2 *text = g_new(gunichar2, units + 1);
	for (guint index = 0; index < units; index++)
		text[index] = read_short(reader);
	char *converted = g_utf16_to_utf8(text, units, NULL, NULL, NULL);
	g_free(text);
	return converted ? converted : g_strdup("");
}

static void clear_swatch(gpointer data) {
	g_free(((Swatch *)data)->name);
}

GArray *create_swatch_list(void) {
	GArray *swatches = g_array_new(FALSE, FALSE, sizeof(Swatch));
	g_array_set_clear_func(swatches, clear_swatch);
	return swatches;
}

static gboolean read_aco_color(SwatchReader *reader, Swatch *swatch, GError **error) {
	guint space = read_short(reader), values[4];
	for (int index = 0; index < 4; index++)
		values[index] = read_short(reader);
	double channels[3];
	if (!convert_swatch_color(space, values, channels)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							space == 2 ? TEXT_SWATCH_SPACE_UNSUPPORTED : TEXT_SWATCH_FILE_DAMAGED);
		return FALSE;
	}
	swatch->red = channels[0];
	swatch->green = channels[1];
	swatch->blue = channels[2];
	return TRUE;
}

static gboolean read_aco_block(SwatchReader *reader, GArray *swatches, GError **error) {
	guint version = read_short(reader), count = read_short(reader);
	for (guint index = 0; index < count && !reader->failed; index++) {
		Swatch swatch = {0};
		if (!read_aco_color(reader, &swatch, error))
			return FALSE;
		if (version == 2) {
			guint32 units = read_long(reader);
			swatch.name = read_text(reader, units ? units - 1 : 0);
			reader->offset += 2;
		} else
			swatch.name = g_strdup_printf("clr%u", index);
		g_array_append_val(swatches, swatch);
	}
	return TRUE;
}

static gboolean read_ase(SwatchReader *reader, GArray *swatches, GError **error) {
	reader->offset = 8;
	guint32 count = read_long(reader);
	g_autofree char *group = NULL;
	for (guint32 index = 0; index < count && !reader->failed; index++) {
		guint type = read_short(reader);
		guint32 size = read_long(reader);
		gsize end = reader->offset + size;
		g_autofree char *name = NULL;
		if (type == 0xc001 || type == 1) {
			guint units = read_short(reader);
			name = read_text(reader, units ? units - 1 : 0);
			reader->offset += 2;
			if (type == 0xc001) {
				g_free(group);
				group = g_steal_pointer(&name);
			}
		}
		if (type == 1) {
			gboolean rgb = has_bytes(reader, 4) && !memcmp(reader->bytes + reader->offset, "RGB ", 4);
			reader->offset += 4;
			if (!rgb) {
				g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_SWATCH_SPACE_UNSUPPORTED);
				return FALSE;
			}
			Swatch swatch = {group ? g_strconcat(group, " : ", name, NULL) : g_strdup(name), 0, 0, 0};
			swatch.red = read_float(reader) * 255.0;
			swatch.green = read_float(reader) * 255.0;
			swatch.blue = read_float(reader) * 255.0;
			g_array_append_val(swatches, swatch);
		}
		reader->offset = end;
	}
	return TRUE;
}

static void shorten_swatch_names(GArray *swatches) {
	for (guint index = 0; index < swatches->len; index++) {
		Swatch *swatch = &g_array_index(swatches, Swatch, index);
		const char *slash = strrchr(swatch->name, '/');
		if (!slash)
			continue;
		const char *equals = strrchr(slash, '=');
		char *shortened = g_strdup(equals ? equals + 1 : slash + 1);
		g_free(swatch->name);
		swatch->name = shortened;
	}
}

GArray *read_swatches(const guint8 *bytes, gsize length, GError **error) {
	SwatchReader reader = {bytes, length, 0, FALSE};
	GArray *swatches = create_swatch_list();
	gboolean read;
	if (length >= 4 && !memcmp(bytes, "ASEF", 4))
		read = read_ase(&reader, swatches, error);
	else {
		read = read_aco_block(&reader, swatches, error);
		if (read && !reader.failed && reader.offset + 3 < length) {
			g_array_set_size(swatches, 0);
			read = read_aco_block(&reader, swatches, error);
		}
	}
	if (read && reader.failed) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, TEXT_SWATCH_FILE_DAMAGED);
		read = FALSE;
	}
	if (!read) {
		g_array_unref(swatches);
		return NULL;
	}
	if (reader.offset + 3 >= length)
		shorten_swatch_names(swatches);
	return swatches;
}

static void append_short(GByteArray *bytes, guint value) {
	guint8 pair[2] = {value >> 8 & 255, value & 255};
	g_byte_array_append(bytes, pair, 2);
}

GBytes *write_swatches(const GArray *swatches) {
	GByteArray *bytes = g_byte_array_new();
	for (guint version = 1; version < 3; version++) {
		append_short(bytes, version);
		append_short(bytes, swatches->len);
		for (guint index = 0; index < swatches->len; index++) {
			const Swatch *swatch = &g_array_index(swatches, Swatch, index);
			append_short(bytes, 0);
			append_short(bytes, round_like_source(swatch->red * (65535.0 / 255)));
			append_short(bytes, round_like_source(swatch->green * (65535.0 / 255)));
			append_short(bytes, round_like_source(swatch->blue * (65535.0 / 255)));
			append_short(bytes, 0);
			if (version == 1)
				continue;
			glong units = 0;
			g_autofree gunichar2 *text = g_utf8_to_utf16(swatch->name, -1, NULL, &units, NULL);
			append_short(bytes, (units + 1) >> 16);
			append_short(bytes, (units + 1) & 0xffff);
			for (glong unit = 0; unit < units; unit++)
				append_short(bytes, text[unit]);
			append_short(bytes, 0);
		}
	}
	return g_byte_array_free_to_bytes(bytes);
}

guint32 get_swatch_color(const Swatch *swatch) {
	return (guint32)round_like_source(swatch->red) << 16 | (guint32)round_like_source(swatch->green) << 8 |
		   (guint32)round_like_source(swatch->blue);
}

void add_swatch(GArray *swatches, const char *name, guint32 color) {
	Swatch swatch = {g_strdup(name), color >> 16 & 255, color >> 8 & 255, color & 255};
	g_array_append_val(swatches, swatch);
}

/* The library is kept as an ACO file store, so Photoshop can read it too */
char *get_swatch_library_path(void) {
	return get_config_file_path("swatches.aco");
}
