#include <limits.h>
#include <math.h>
#include <string.h>

#include "psd.h"

/* UTF-16 pairs a high and a low surrogate into a character past the first 65536 */
#define HIGH_SURROGATE 0xd800
#define LOW_SURROGATE 0xdc00
#define SURROGATES_END 0xe000
#define SURROGATE_BITS 10
#define FIRST_PAIRED_CHARACTER 0x10000
#define REPLACEMENT_CHARACTER 0xfffd
#include "descriptor.h"

void fail_psd(PsdReader *reader, const char *message) {
	if (reader->failed)
		return;
	reader->failed = TRUE;
	g_set_error_literal(reader->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
}

void note_unsupported_psd_feature(PsdReader *reader, const char *feature) {
	GPtrArray *features = reader->document->unsupported_features;
	for (guint index = 0; index < features->len; index++)
		if (!strcmp(g_ptr_array_index(features, index), feature))
			return;
	g_ptr_array_add(features, g_strdup(feature));
}

gboolean has_psd_bytes(PsdReader *reader, gsize offset, gsize count) {
	if (offset <= reader->length && count <= reader->length - offset)
		return TRUE;
	if (!reader->lenient)
		fail_psd(reader, TEXT_PSD_DAMAGED);
	return FALSE;
}

/* Resource views read missing bytes as zero, while the file itself stays strict */
int read_psd_byte(PsdReader *reader, gsize offset) {
	return has_psd_bytes(reader, offset, 1) ? reader->data[offset] : 0;
}

guint read_psd_u16(PsdReader *reader, gsize offset) {
	return read_psd_byte(reader, offset) << CHAR_BIT | read_psd_byte(reader, offset + 1);
}

guint32 read_psd_u32(PsdReader *reader, gsize offset) {
	return (guint32)read_psd_u16(reader, offset) << 2 * CHAR_BIT | read_psd_u16(reader, offset + sizeof(guint16));
}

gint16 read_i16(PsdReader *reader, gsize offset) {
	return (gint16)read_psd_u16(reader, offset);
}

gint32 read_i32(PsdReader *reader, gsize offset) {
	return (gint32)read_psd_u32(reader, offset);
}

gint64 read_large(PsdReader *reader, gsize offset) {
	return (gint32)(read_psd_u32(reader, offset) | read_psd_u32(reader, offset + sizeof(guint32)));
}

gint64 read_length(PsdReader *reader, gsize offset) {
	return reader->large ? read_large(reader, offset) : read_psd_u32(reader, offset);
}

double read_double(PsdReader *reader, gsize offset) {
	guint64 bits = (guint64)read_psd_u32(reader, offset) << sizeof(guint32) * CHAR_BIT |
				   read_psd_u32(reader, offset + sizeof(guint32));
	double value;
	memcpy(&value, &bits, sizeof value);
	return value;
}

double read_float(PsdReader *reader, gsize offset) {
	guint32 bits = read_psd_u32(reader, offset);
	float value;
	memcpy(&value, &bits, sizeof value);
	return value;
}

double read_fixed(PsdReader *reader, gsize offset) {
	return read_psd_u16(reader, offset) + read_psd_u16(reader, offset + sizeof(guint16)) * (1.0 / (G_MAXUINT16 + 1));
}

void read_key(PsdReader *reader, gsize offset, char key[PSD_KEY_SIZE]) {
	for (int index = 0; index < PSD_KEY_LENGTH; index++)
		key[index] = read_psd_byte(reader, offset + index);
	key[PSD_KEY_LENGTH] = 0;
}

const char *const LARGE_LENGTH_KEYS =
	"LMsk Lr16 Lr32 Layr Mt16 Mt32 Mtrn Alph FMsk lnk2 lnkE FEid FXid PxSD extn cinf artd pths";

/* The keys are listed with a space between them */
gboolean is_listed_key(const char *list, const char *key) {
	for (const char *position = list;; position += PSD_KEY_SIZE) {
		if (!strncmp(position, key, PSD_KEY_LENGTH))
			return TRUE;
		if (!position[PSD_KEY_LENGTH])
			return FALSE;
	}
}

char *read_pascal(PsdReader *reader, gsize offset, gsize *length) {
	int count = read_psd_byte(reader, offset);
	GString *text = g_string_new(NULL);
	for (int index = 0; index < count; index++) {
		int byte = read_psd_byte(reader, offset + 1 + index);
		g_string_append_unichar(text, byte);
	}
	/* The count byte and the text take an even number of bytes */
	*length = count + 1 - count % 2 + 1;
	return g_string_free(text, FALSE);
}

char *read_utf16(PsdReader *reader, gsize offset, gsize count) {
	if (!has_psd_bytes(reader, offset, count * sizeof(gunichar2)))
		return g_strdup("");
	g_autofree gunichar2 *units = g_new(gunichar2, count + 1);
	for (gsize index = 0; index < count; index++)
		units[index] = read_psd_u16(reader, offset + index * sizeof(gunichar2));
	/* Pairs of surrogates join into one character, and a lone surrogate becomes the replacement character */
	GString *text = g_string_new(NULL);
	for (gsize index = 0; index < count; index++) {
		gunichar unit = units[index];
		gboolean pair = unit >= HIGH_SURROGATE && unit < LOW_SURROGATE && index + 1 < count &&
						units[index + 1] >= LOW_SURROGATE && units[index + 1] < SURROGATES_END;
		if (pair)
			unit = FIRST_PAIRED_CHARACTER + ((unit - HIGH_SURROGATE) << SURROGATE_BITS) + (units[++index] - LOW_SURROGATE);
		else if (unit >= HIGH_SURROGATE && unit < SURROGATES_END)
			unit = REPLACEMENT_CHARACTER;
		g_string_append_unichar(text, unit);
	}
	return g_string_free(text, FALSE);
}

char *read_unicode(PsdReader *reader, gsize offset) {
	return read_utf16(reader, offset + sizeof(guint32), read_psd_u32(reader, offset));
}

Record *read_rectangle(PsdReader *reader, gsize offset) {
	double top = read_i32(reader, offset), left = read_i32(reader, offset + sizeof(guint32));
	double bottom = read_i32(reader, offset + 2 * sizeof(guint32)), right = read_i32(reader, offset + 3 * sizeof(guint32));
	return create_extent(left, top, right - left, bottom - top);
}

Record *create_byte_array(PsdReader *reader, gsize offset, gsize length) {
	if (!has_psd_bytes(reader, offset, length))
		return create_bytes(NULL, 0);
	return create_bytes(reader->data + offset, length);
}

/* ============================== Header and resources ============================ */

static gsize read_header(PsdReader *reader, gsize offset) {
	Record *record = reader->document->record;
	char signature[PSD_KEY_SIZE];
	read_key(reader, offset, signature);
	guint version = read_psd_u16(reader, offset + HEADER_VERSION);
	if (strcmp(signature, PSD_SIGNATURE) || (version != PSD_VERSION && version != PSB_VERSION)) {
		fail_psd(reader, TEXT_PSD_DAMAGED);
		return offset;
	}
	reader->large = version == PSB_VERSION;
	set_boolean(record, "p7", reader->large);
	set_number(record, "P9", read_psd_u16(reader, offset + HEADER_CHANNELS));
	set_number(record, "T", read_i32(reader, offset + HEADER_HEIGHT));
	set_number(record, "s", read_i32(reader, offset + HEADER_WIDTH));
	set_number(record, "g_", read_psd_u16(reader, offset + HEADER_DEPTH));
	set_number(record, "depth", get_number(record, "g_"));
	set_number(record, "Ia", read_psd_u16(reader, offset + HEADER_MODE));
	return offset + HEADER_BYTES;
}

static gsize read_color_mode_data(PsdReader *reader, gsize offset) {
	gint32 length = read_i32(reader, offset);
	offset += sizeof(guint32);
	if (length != 0) {
		if (length < 0 || !has_psd_bytes(reader, offset, length)) {
			fail_psd(reader, TEXT_PSD_DAMAGED);
			return offset;
		}
		set_record(reader->document->record, "rm", create_byte_array(reader, offset, length));
	}
	return offset + length;
}

static gsize read_image_resources(PsdReader *reader, gsize offset) {
	guint32 total = read_psd_u32(reader, offset);
	offset += sizeof(guint32);
	Record *resources = get_record(reader->document->record, "Pz");
	g_autoptr(GArray) unknown = g_array_new(FALSE, FALSE, sizeof(guint));
	for (gsize position = 0; position < total && !reader->failed;) {
		char signature[PSD_KEY_SIZE];
		read_key(reader, offset + position, signature);
		position += PSD_KEY_LENGTH;
		guint identifier = read_psd_u16(reader, offset + position);
		position += sizeof(guint16);
		gsize name_length;
		g_free(read_pascal(reader, offset + position, &name_length));
		position += name_length;
		gboolean image_ready = identifier == IMAGE_READY_FIRST || identifier == IMAGE_READY_LAST;
		if (!(!strcmp(signature, IMAGE_READY_SIGNATURE) && image_ready) && strcmp(signature, BLOCK_SIGNATURE))
			g_array_append_val(unknown, identifier);
		guint32 size = read_psd_u32(reader, offset + position);
		position += sizeof(guint32);
		g_autofree char *key = g_strdup_printf("r%u", identifier);
		if (!has_psd_bytes(reader, offset + position, size))
			break;
		set_record(resources, key, create_byte_array(reader, offset + position, size));
		position += size + size % 2;
	}
	for (guint index = 0; index < unknown->len; index++) {
		g_autofree char *key = g_strdup_printf("r%u", g_array_index(unknown, guint, index));
		remove_record(resources, key);
	}
	remove_record(resources, "r1092");
	return offset + total;
}

/* ============================== Resource fields ================================ */

/* Resource views read missing bytes as zero, unlike the strict reader of the whole file */
static PsdReader create_resource_view(const Record *resource) {
	return (PsdReader){.data = resource->value.buffer.data, .length = resource->value.buffer.length, .lenient = TRUE};
}

static Record *read_guides(PsdReader *reader, const Record *resource) {
	PsdReader guides = create_resource_view(resource);
	gsize offset = GRID_COUNT_AT;
	guint32 count = read_psd_u32(&guides, offset);
	offset += sizeof(guint32);
	count = MIN(count, (guint32)((guides.length > offset ? guides.length - offset : 0) / GRID_RECORD_BYTES));
	Record *list = create_array();
	for (guint32 index = 0; index < count; index++) {
		gsize at = offset + index * GRID_RECORD_BYTES;
		Record *guide = create_array();
		append_record(guide, create_number(MIN(guides.data[at + sizeof(guint32)], 1)));
		append_record(guide, create_number(read_i32(&guides, at) / (double)UNITS_PER_PIXEL));
		append_record(list, guide);
	}
	return list;
}

static void apply_image_resources(PsdReader *reader) {
	Record *record = reader->document->record, *resources = get_record(record, "Pz"), *layers = get_record(record, "Z");
	Record *resource;
	if ((resource = get_record(resources, "r1005"))) {
		PsdReader view = create_resource_view(resource);
		set_number(record, "Qf", read_fixed(&view, 0));
	}
	if ((resource = get_record(resources, "r1026"))) {
		PsdReader view = create_resource_view(resource);
		for (gsize index = 0; index + 1 < view.length; index += sizeof(guint16)) {
			Record *layer = get_item(layers, index / sizeof(guint16));
			if (layer)
				set_number(layer, "kr", read_psd_u16(&view, index));
		}
		remove_record(resources, "r1026");
	}
	if ((resource = get_record(resources, "r1032")))
		set_record(record, "LM", read_guides(reader, resource));
	remove_record(resources, "r1036");
	set_record(record, "fI", create_null());
	if ((resource = get_record(resources, "r1045"))) {
		PsdReader view = create_resource_view(resource);
		Record *names = create_array();
		for (gsize offset = 0; offset < view.length;) {
			guint32 count = read_psd_u32(&view, offset);
			guint32 units = count ? count - 1 : 0;
			/* Each name counts its closing zero, which is skipped */
			g_autofree char *name = read_utf16(&view, offset + sizeof(guint32), units);
			append_record(names, create_string(name));
			offset += sizeof(guint32) + (gsize)(units + 1) * sizeof(gunichar2);
		}
		set_record(record, "Wa", names);
	}
	if ((resource = get_record(resources, "r1069"))) {
		PsdReader view = create_resource_view(resource);
		Record *selected = create_array();
		for (gsize offset = sizeof(guint16); offset < view.length; offset += sizeof(guint32)) {
			double identifier = read_psd_u32(&view, offset);
			for (guint index = 0; index < count_records(layers); index++) {
				Record *descriptors = get_record(get_item(layers, index), "add");
				gboolean listed = FALSE;
				for (guint item = 0; item < count_records(selected); item++)
					if (get_item(selected, item)->value.number == index)
						listed = TRUE;
				if (has_record(descriptors, "lyid") && get_number(descriptors, "lyid") == identifier &&
					get_number(descriptors, "lsct") != SECTION_GROUP_END && !listed)
					append_record(selected, create_number(index));
			}
		}
		set_record(record, "j", selected);
	}
	if ((resource = get_record(resources, "r1072"))) {
		for (gsize index = 0; index < resource->value.buffer.length; index++) {
			Record *layer = get_item(layers, index);
			if (layer)
				set_boolean(layer, "nQ", ((guint8 *)resource->value.buffer.data)[index] == 1);
		}
		remove_record(resources, "r1072");
	}
	if ((resource = get_record(resources, "r1073"))) {
		PsdReader view = create_resource_view(resource);
		/* Version 2 and later keep floats, earlier ones fixed point numbers */
		gint32 version = read_i32(&view, 0), count = read_i32(&view, sizeof(guint32));
		Record *guides = get_record(record, "Jd");
		for (gint32 index = 0, offset = 2 * sizeof(guint32); index < MIN(count, G_MAXUINT16 + 1);
			 index++, offset += GUIDE_RECORD_BYTES) {
			gsize y = offset + sizeof(guint16), x = y + sizeof(guint32);
			Record *guide = create_object(RECORD_PLAIN);
			set_number(guide, "y", version >= 2 ? read_float(&view, y) : read_fixed(&view, y));
			set_number(guide, "x", version >= 2 ? read_float(&view, x) : read_fixed(&view, x));
			append_record(guides, guide);
		}
		remove_record(resources, "r1073");
	}
	if ((resource = get_record(resources, "r1065"))) {
		PsdReader view = create_resource_view(resource);
		Record *comps = create_object(RECORD_PLAIN);
		read_descriptor(&view, DESCRIPTOR_AT, comps);
		set_record(record, "gL", comps);
	}
	if ((resource = get_record(resources, "r1074"))) {
		PsdReader view = create_resource_view(resource);
		Record *options = create_object(RECORD_PLAIN);
		read_descriptor(&view, DESCRIPTOR_AT, options);
		set_record(record, "S8", options);
		remove_record(resources, "r1074");
	}
	if ((resource = get_record(resources, "r1076"))) {
		PsdReader view = create_resource_view(resource);
		g_autoptr(Record) options = create_object(RECORD_PLAIN);
		read_descriptor(&view, DESCRIPTOR_AT, options);
		Record *sheets = get_record(get_record(options, "sheetTimelineOptions"), "v");
		for (guint index = 0; index < count_records(sheets); index++) {
			Record *sheet = get_record(get_item(sheets, index), "v");
			double identifier = get_number(get_record(sheet, "sheetID"), "v");
			for (guint layer_index = 0; layer_index < count_records(layers); layer_index++) {
				Record *layer = get_item(layers, layer_index);
				if (get_number(get_record(layer, "add"), "lyid") != identifier)
					continue;
				Record *disclosed = create_array();
				append_record(disclosed, create_boolean(get_boolean(get_record(sheet, "sheetDisclosed"), "v")));
				set_record(layer, "md", disclosed);
			}
		}
		remove_record(resources, "r1076");
	}
	/* Paths, slices, timelines, metadata and variables stay as raw resources, written back as they were read */
}

/* ============================== Post processing ================================= */

static Record *get_editable_layer_mask(const Record *layer) {
	Record *alternate = get_record(layer, "E_"), *vector = get_record(get_record(layer, "add"), "vmsk");
	if (alternate && alternate->type == RECORD_OBJECT)
		return alternate;
	if (vector && vector->type == RECORD_OBJECT && get_boolean(vector, "isEnabled"))
		return NULL;
	Record *mask = get_record(layer, "yy");
	return mask && mask->type == RECORD_OBJECT ? mask : NULL;
}

static void finish_layers(PsdReader *reader) {
	Document *document = reader->document;
	Record *record = document->record, *layers = get_record(record, "Z");
	remove_record(record, "p7");
	if (!count_records(layers)) {
		Record *background = create_document_layer(document, FALSE);
		name_layer(background, "Background");
		append_record(layers, background);
		set_record(background, "buffer", copy_record(get_record(record, "buffer")));
		set_record(background, "V", create_extent(0, 0, get_number(record, "s"), get_number(record, "T")));
	}
	int balance = 0;
	for (guint index = 0; index < count_records(layers); index++) {
		double section = get_number(get_record(get_item(layers, index), "add"), "lsct");
		if (section == SECTION_GROUP_END)
			balance++;
		else if (section == SECTION_OPEN || section == SECTION_CLOSED)
			balance--;
	}
	for (; balance < 0; balance++)
		g_ptr_array_insert(layers->value.array, 0, create_group_closing(document, FALSE));
	set_record(record, "vA", create_null());
	gboolean renamed = FALSE;
	for (guint index = 0; index < count_records(layers); index++) {
		Record *layer = get_item(layers, index), *descriptors = get_record(layer, "add");
		double section = get_number(descriptors, "lsct");
		if (section == SECTION_GROUP_END)
			set_number(descriptors, "lspf", 0);
		if (!g_strcmp0(get_string(layer, "name"), "")) {
			g_autofree char *name = g_strdup_printf("Layer %u", index + 1);
			set_string(layer, "name", name);
			renamed = TRUE;
		}
		if (renamed && (section == SECTION_OPEN || section == SECTION_CLOSED) && !g_strcmp0(get_string(layer, "qu"), "norm"))
			set_string(layer, "qu", "pass");
		Record *mask = get_record(layer, "yy"), *editable = get_editable_layer_mask(layer);
		if (!editable && mask && mask->type == RECORD_OBJECT && !get_boolean(mask, "L7")) {
			set_boolean(mask, "L7", TRUE);
			set_record(layer, "E_", copy_mask(mask));
		}
		editable = get_editable_layer_mask(layer);
		Record *parameters = get_record(layer, "kz");
		if (parameters && parameters->type == RECORD_ARRAY) {
			if (editable) {
				set_number(editable, "mU", get_item(parameters, 0)->value.number);
				set_number(editable, "O1", get_item(parameters, 1)->value.number);
			}
			set_record(layer, "kz", create_null());
		}
	}
	double highest = 0;
	g_autoptr(GArray) duplicates = g_array_new(FALSE, FALSE, sizeof(guint));
	g_autoptr(GArray) identifiers = g_array_new(FALSE, FALSE, sizeof(double));
	for (guint index = 0; index < count_records(layers); index++) {
		Record *descriptors = get_record(get_item(layers, index), "add");
		if (!has_record(descriptors, "lyid") || get_record(descriptors, "lyid")->type == RECORD_NULL)
			continue;
		double identifier = get_number(descriptors, "lyid");
		highest = MAX(highest, identifier);
		gboolean seen = FALSE;
		for (guint item = 0; item < identifiers->len; item++)
			if (g_array_index(identifiers, double, item) == identifier)
				seen = TRUE;
		if (seen)
			g_array_append_val(duplicates, index);
		else
			g_array_append_val(identifiers, identifier);
	}
	for (guint index = 0; index < duplicates->len; index++)
		set_number(get_record(get_item(layers, g_array_index(duplicates, guint, index)), "add"), "lyid",
				   highest + 1 + index);
}

Document *read_psd_document(const guint8 *data, gsize length, const char *name, GCancellable *cancellable,
							GError **error) {
	Document *document = create_document_record(name, DOCUMENT_RASTER);
	document->unsupported_features = g_ptr_array_new_with_free_func(g_free);
	PsdReader reader = {
		.data = data, .length = length, .error = error, .document = document, .cancellable = cancellable};
	gsize offset = read_header(&reader, 0);
	Record *record = document->record;
	int mode = get_number(record, "Ia"), depth = get_number(record, "g_");
	double width = get_number(record, "s"), height = get_number(record, "T");
	if (!reader.failed && (mode < PSD_GRAY_MODE || mode > PSD_RGB_MODE || (depth != BYTE_DEPTH && depth != WORD_DEPTH && depth != FLOAT_DEPTH))) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_PSD_COLOR_MODE);
		reader.failed = TRUE;
	}
	if (!reader.failed && (width <= 0 || height <= 0 || width * height > DOCUMENT_MAX_PIXELS))
		fail_psd(&reader, TEXT_PIXEL_LIMIT);
	if (!reader.failed)
		offset = read_color_mode_data(&reader, offset);
	if (!reader.failed)
		offset = read_image_resources(&reader, offset);
	if (!reader.failed)
		offset = read_layer_and_mask_information(&reader, offset);
	if (!reader.failed)
		apply_image_resources(&reader);
	if (!reader.failed) {
		remove_record(get_record(record, "add"), "fcmy");
		offset = read_psd_merged_image(&reader, offset, depth);
	}
	if (!reader.failed)
		finish_layers(&reader);
	if (reader.failed) {
		free_document(document);
		return NULL;
	}
	return document;
}
