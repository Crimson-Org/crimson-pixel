#include <limits.h>
#include <math.h>
#include <string.h>

#include <gio/gio.h>

#include "common/storage/key-store.h"
#include "color-spaces.h"

/* ACO files list the colours once without names, then again with them, and ASE files have a signature, a version,
 * a count of blocks, then group and colour blocks with a colour model of four characters */
#define ACO_PLAIN_VERSION 1
#define ACO_NAMED_VERSION 2
#define ASE_SIGNATURE "ASEF"
#define ASE_RGB "RGB "
#define ASE_MODEL_BYTES 4
#define ASE_COUNT_AT 8
#define ASE_GROUP 0xc001
#define ASE_COLOR 1
#include "swatches.h"
#include "tools/kernels.h"
#include "lang/en.h"

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
	if (!has_bytes(reader, sizeof(guint16)))
		return 0;
	const guint8 *at = reader->bytes + reader->offset;
	reader->offset += sizeof(guint16);
	return at[0] << CHAR_BIT | at[1];
}

static guint32 read_long(SwatchReader *reader) {
	if (!has_bytes(reader, sizeof(guint32)))
		return 0;
	const guint8 *at = reader->bytes + reader->offset;
	reader->offset += sizeof(guint32);
	return (guint32)at[0] << 3 * CHAR_BIT | at[1] << 2 * CHAR_BIT | at[2] << CHAR_BIT | at[3];
}

static float read_float(SwatchReader *reader) {
	guint32 bits = read_long(reader);
	float value;
	memcpy(&value, &bits, sizeof value);
	return value;
}

static char *read_text(SwatchReader *reader, guint units) {
	if (!has_bytes(reader, (gsize)units * sizeof(gunichar2)))
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
	guint space = read_short(reader), values[SWATCH_VALUES];
	for (int index = 0; index < SWATCH_VALUES; index++)
		values[index] = read_short(reader);
	double channels[RGB_CHANNELS];
	if (!convert_swatch_color(space, values, channels)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							space == SWATCH_CMYK ? TEXT_SWATCH_SPACE_UNSUPPORTED : TEXT_SWATCH_FILE_DAMAGED);
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
		if (version == ACO_NAMED_VERSION) {
			guint32 units = read_long(reader);
			swatch.name = read_text(reader, units ? units - 1 : 0);
			reader->offset += sizeof(gunichar2);
		} else
			swatch.name = g_strdup_printf("clr%u", index);
		g_array_append_val(swatches, swatch);
	}
	return TRUE;
}

static gboolean read_ase(SwatchReader *reader, GArray *swatches, GError **error) {
	reader->offset = ASE_COUNT_AT;
	guint32 count = read_long(reader);
	g_autofree char *group = NULL;
	for (guint32 index = 0; index < count && !reader->failed; index++) {
		guint type = read_short(reader);
		guint32 size = read_long(reader);
		gsize end = reader->offset + size;
		g_autofree char *name = NULL;
		if (type == ASE_GROUP || type == ASE_COLOR) {
			guint units = read_short(reader);
			name = read_text(reader, units ? units - 1 : 0);
			reader->offset += sizeof(gunichar2);
			if (type == ASE_GROUP) {
				g_free(group);
				group = g_steal_pointer(&name);
			}
		}
		if (type == ASE_COLOR) {
			gboolean rgb = has_bytes(reader, ASE_MODEL_BYTES) &&
						   !memcmp(reader->bytes + reader->offset, ASE_RGB, ASE_MODEL_BYTES);
			reader->offset += ASE_MODEL_BYTES;
			if (!rgb) {
				g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_SWATCH_SPACE_UNSUPPORTED);
				return FALSE;
			}
			Swatch swatch = {group ? g_strconcat(group, " : ", name, NULL) : g_strdup(name), 0, 0, 0};
			swatch.red = unit_to_byte(read_float(reader));
			swatch.green = unit_to_byte(read_float(reader));
			swatch.blue = unit_to_byte(read_float(reader));
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
	if (length >= ASE_MODEL_BYTES && !memcmp(bytes, ASE_SIGNATURE, ASE_MODEL_BYTES))
		read = read_ase(&reader, swatches, error);
	else {
		read = read_aco_block(&reader, swatches, error);
		/* A second block after the first holds the same colours with their names */
		if (read && !reader.failed && reader.offset + sizeof(guint32) <= length) {
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
	if (reader.offset + sizeof(guint32) > length)
		shorten_swatch_names(swatches);
	return swatches;
}

static void append_short(GByteArray *bytes, guint value) {
	guint8 pair[] = {value >> CHAR_BIT & G_MAXUINT8, value & G_MAXUINT8};
	g_byte_array_append(bytes, pair, sizeof pair);
}

GBytes *write_swatches(const GArray *swatches) {
	GByteArray *bytes = g_byte_array_new();
	/* Both versions, so older readers find the colours and newer ones the names too */
	for (guint version = ACO_PLAIN_VERSION; version <= ACO_NAMED_VERSION; version++) {
		append_short(bytes, version);
		append_short(bytes, swatches->len);
		for (guint index = 0; index < swatches->len; index++) {
			const Swatch *swatch = &g_array_index(swatches, Swatch, index);
			append_short(bytes, SWATCH_RGB);
			append_short(bytes, round_half_up(swatch->red * ((double)G_MAXUINT16 / G_MAXUINT8)));
			append_short(bytes, round_half_up(swatch->green * ((double)G_MAXUINT16 / G_MAXUINT8)));
			append_short(bytes, round_half_up(swatch->blue * ((double)G_MAXUINT16 / G_MAXUINT8)));
			append_short(bytes, 0);
			if (version == ACO_PLAIN_VERSION)
				continue;
			glong units = 0;
			g_autofree gunichar2 *text = g_utf8_to_utf16(swatch->name, -1, NULL, &units, NULL);
			append_short(bytes, (units + 1) >> 2 * CHAR_BIT);
			append_short(bytes, (units + 1) & G_MAXUINT16);
			for (glong unit = 0; unit < units; unit++)
				append_short(bytes, text[unit]);
			append_short(bytes, 0);
		}
	}
	return g_byte_array_free_to_bytes(bytes);
}

guint32 get_swatch_color(const Swatch *swatch) {
	return (guint32)round_half_up(swatch->red) << WORD_RED_SHIFT | (guint32)round_half_up(swatch->green) << WORD_GREEN_SHIFT |
		   (guint32)round_half_up(swatch->blue);
}

void add_swatch(GArray *swatches, const char *name, guint32 color) {
	Swatch swatch = {g_strdup(name), color >> WORD_RED_SHIFT & G_MAXUINT8, color >> WORD_GREEN_SHIFT & G_MAXUINT8,
					 color & G_MAXUINT8};
	g_array_append_val(swatches, swatch);
}

/* The library is kept as an ACO file store, so Photoshop can read it too */
char *get_swatch_library_path(void) {
	return get_config_file_path("swatches.aco");
}
