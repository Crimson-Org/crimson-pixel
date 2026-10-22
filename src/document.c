#include <math.h>
#include <string.h>

#include "document.h"
#include "history.h"
#include "../lang/en.h"

static void initialize_fields(Record *record, const char *names, RecordType type) {
	g_auto(GStrv) fields = g_strsplit(names, " ", -1);
	for (guint index = 0; fields[index]; index++) {
		Record *value = type == RECORD_NULL		? create_null()
						: type == RECORD_ARRAY	? create_array()
						: type == RECORD_OBJECT ? create_object(RECORD_PLAIN)
						: type == RECORD_NUMBER ? create_number(0)
												: create_boolean(FALSE);
		set_record(record, fields[index], value);
	}
}

static Record *number_array(const double *numbers, guint length) {
	Record *array = create_array();
	for (guint index = 0; index < length; index++)
		append_record(array, create_number(numbers[index]));
	return array;
}

static Record *create_work_path(void) {
	Record *path = create_object(RECORD_PLAIN);
	set_string(path, "name", "Work Path");
	set_number(path, "ny", 0);
	set_record(path, "VJ", number_array((double[]){0, 0}, 2));
	Record *descriptors = create_object(RECORD_PLAIN);
	set_record(descriptors, "vmsk", create_vector_mask());
	set_record(descriptors, "vogk", create_array());
	set_record(path, "add", descriptors);
	return path;
}

static Record *create_view_record(Record *document) {
	Record *view = create_object(RECORD_VIEW);
	set_record(view, "r6", create_reference(document));
	initialize_fields(view, "vl Er fL tx", RECORD_NUMBER);
	initialize_fields(view, "Cz H1 mB nu", RECORD_NULL);
	set_record(view, "PW", create_point(0, 0));
	set_record(view, "Y6", create_point(0, 0));
	set_record(view, "i", create_point(0, 0));
	set_record(view, "b7", create_point(0, 0));
	set_number(view, "ax9", 1);
	set_number(view, "r7", 1);
	set_record(view, "RD", number_array((double[]){1, 1, 1}, 3));
	set_record(view, "cK", create_extent(0, 0, 1, 1));
	set_record(view, "a3r", create_extent(0, 0, get_number(document, "s"), get_number(document, "T")));
	return view;
}

Document *create_document_record(const char *name, DocumentMode mode) {
	Document *document = g_new0(Document, 1);
	document->mode = mode;
	Record *record = document->record = create_object(RECORD_DOCUMENT);
	set_string(record, "UM", "psd");
	set_string(record, "name", name);
	set_number(record, "g_", 8);
	set_number(record, "depth", 8);
	set_number(record, "Ia", -1);
	set_number(record, "P9", 4);
	set_number(record, "Qf", 72);
	set_number(record, "vt", -1);
	initialize_fields(record, "s T aBu akI NP eN aH2 aKe", RECORD_NUMBER);
	initialize_fields(record, "fK zn Jg Rx aAI aCN at0 _s z3 Ma di kA Ir qW", RECORD_BOOLEAN);
	initialize_fields(record, "cm Em _e Zt buffer aI7 rm AK S8 vA ST ey ab6 g oG fI ail", RECORD_NULL);
	initialize_fields(record, "Z Jd _y j LM Xd z4 y_ vC H5 ZG mZ J8 In history", RECORD_ARRAY);
	initialize_fields(record, "Pz add dJ IH", RECORD_OBJECT);
	append_record(get_record(record, "y_"), create_work_path());
	Record *compositions = create_object(RECORD_PLAIN);
	set_string(compositions, "classID", "CompList");
	Record *list = create_object(RECORD_PLAIN);
	set_string(list, "t", "VlLs");
	set_record(list, "v", create_array());
	set_record(compositions, "list", list);
	set_record(record, "gL", compositions);
	Record *media = create_object(RECORD_PLAIN);
	set_number(media, "op", 960);
	set_number(media, "kW", 120);
	set_record(media, "jO", number_array((double[]){4, 4}, 2));
	set_boolean(media, "b6", FALSE);
	set_boolean(media, "PD", FALSE);
	set_number(media, "eo", 0);
	set_record(record, "m", media);
	Record *options = create_object(RECORD_PLAIN);
	set_record(options, "lU", create_object(RECORD_PLAIN));
	initialize_fields(options, "Dr bd El GS Wm Po Ws", RECORD_ARRAY);
	initialize_fields(options, "Xt vq zh X9 cY Ug mT $i Pg", RECORD_NULL);
	set_record(record, "O", options);
	double now = g_get_real_time() / 1000;
	append_record(get_record(record, "history"),
				  create_history_entry(number_array((double[]){1, 0}, 2), NULL, FALSE, now));
	set_number(record, "aNe", now);
	set_record(record, "u", create_view_record(record));
	return document;
}

static void free_layer_tree(LayerTree *tree) {
	if (!tree)
		return;
	if (tree->children)
		g_ptr_array_unref(tree->children);
	g_free(tree);
}

void free_document(Document *document) {
	if (!document)
		return;
	free_layer_tree(document->root);
	g_clear_pointer(&document->lookup, g_ptr_array_unref);
	free_record(document->root_layer);
	free_record(document->root_closing);
	free_record(document->record);
	g_clear_pointer(&document->history_operations, g_hash_table_unref);
	g_clear_pointer(&document->unsupported_features, g_ptr_array_unref);
	g_clear_pointer(&document->grown_layer, free_record);
	g_free(document);
}

double next_layer_id(Document *document) {
	Record *resources = get_record(document->record, "Pz");
	Record *counter = get_record(resources, "r1044");
	if (!counter || counter->type == RECORD_NULL) {
		counter = create_channels(4, 8);
		set_record(resources, "r1044", counter);
		double maximum = 0;
		Record *layers = get_record(document->record, "Z");
		for (guint index = 0; index < count_records(layers); index++) {
			Record *id = get_record(get_record(get_item(layers, index), "add"), "lyid");
			maximum = !id || isnan(maximum) ? NAN : MAX(maximum, id->value.number);
		}
		guint32 value = isfinite(maximum) ? (guint32)fmod(maximum, 4294967296.0) : 0;
		for (guint index = 0; index < 4; index++)
			write_channel(counter, index, (value >> (24 - index * 8)) & 255);
	}
	double next = read_channel(counter, 0) * 16777216 + read_channel(counter, 1) * 65536 +
				  read_channel(counter, 2) * 256 + read_channel(counter, 3) + 1;
	guint32 value = (guint32)fmod(next, 4294967296.0);
	for (guint index = 0; index < 4; index++)
		write_channel(counter, index, (value >> (24 - index * 8)) & 255);
	return next;
}

Record *create_document_layer(Document *document, gboolean without_id) {
	Record *layer = create_layer_record();
	set_record(layer, "V", create_extent(0, 0, 0, 0));
	set_record(layer, "buffer", create_channels(0, get_number(document->record, "depth")));
	name_layer(layer, "Layer");
	Record *descriptors = get_record(layer, "add");
	if (!without_id)
		set_number(descriptors, "lyid", next_layer_id(document));
	set_number(descriptors, "lsct", 0);
	set_number(descriptors, "lclr", 0);
	set_record(descriptors, "fxrp", create_point(0, 0));
	return layer;
}

Record *create_group_closing(Document *document, gboolean without_id) {
	Record *layer = create_document_layer(document, without_id);
	name_layer(layer, "</Layer group>");
	set_number(get_record(layer, "add"), "lsct", 3);
	set_number(layer, "NF", 24);
	return layer;
}

static gboolean validate_groups(const Record *layers, GError **error) {
	guint depth = 0;
	for (guint index = 0; index < count_records(layers); index++) {
		Record *layer = get_item(layers, index);
		if (!layer || layer->class != RECORD_LAYER)
			break;
		double section = get_number(get_record(layer, "add"), "lsct");
		if (section == 3) {
			if (++depth > 512)
				break;
		} else if (section == 1 || section == 2) {
			if (!depth)
				break;
			depth--;
		}
		if (index + 1 == count_records(layers) && depth == 0)
			return TRUE;
	}
	if (!count_records(layers))
		return TRUE;
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, TEXT_INVALID_GROUPS);
	return FALSE;
}

static guint build_tree(LayerTree *tree, const Record *layers, guint position, guint depth, GPtrArray *lookup) {
	tree->depth = depth;
	tree->index = tree->closing_index = -1;
	Record *layer = get_item(layers, position);
	if (get_number(get_record(layer, "add"), "lsct") != 3) {
		tree->layer = layer;
		tree->index = (int)position - 1;
		if (tree->index >= 0)
			lookup->pdata[tree->index] = tree;
		return position + 1;
	}
	tree->closing = layer;
	tree->closing_index = (int)position - 1;
	tree->children = g_ptr_array_new_with_free_func((GDestroyNotify)free_layer_tree);
	guint index = position + 1;
	while (index < count_records(layers)) {
		Record *child = get_item(layers, index);
		if (is_layer_group(child)) {
			Record *closing_add = get_record(layer, "add"), *opening_add = get_record(child, "add");
			Record *closing_id = get_record(closing_add, "lyid"), *opening_id = get_record(opening_add, "lyid");
			if ((!closing_id && !opening_id) ||
				(closing_id && opening_id && closing_id->value.number == opening_id->value.number))
				set_number(closing_add, "lyid", closing_id ? closing_id->value.number + 16777215 : NAN);
			tree->layer = child;
			tree->index = (int)index - 1;
			if (tree->index >= 0)
				lookup->pdata[tree->index] = tree;
			if (tree->closing_index >= 0)
				lookup->pdata[tree->closing_index] = tree;
			return index + 1;
		}
		LayerTree *branch = g_new0(LayerTree, 1);
		branch->parent = tree;
		index = build_tree(branch, layers, index, depth + 1, lookup);
		g_ptr_array_add(tree->children, branch);
	}
	g_assert_not_reached();
}

gboolean rebuild_layer_tree(Document *document, GError **error) {
	Record *layers = get_record(document->record, "Z");
	if (!validate_groups(layers, error))
		return FALSE;
	free_layer_tree(document->root);
	g_clear_pointer(&document->lookup, g_ptr_array_unref);
	free_record(document->root_layer);
	free_record(document->root_closing);
	document->root_layer = create_document_layer(document, TRUE);
	set_string(document->root_layer, "qu", "pass");
	set_number(get_record(document->root_layer, "add"), "lsct", 1);
	name_layer(document->root_layer, "");
	document->root_closing = create_group_closing(document, TRUE);
	g_autoptr(Record) stream = create_array();
	append_record(stream, reference_record(document->root_closing));
	for (guint index = 0; index < count_records(layers); index++)
		append_record(stream, reference_record(get_item(layers, index)));
	append_record(stream, reference_record(document->root_layer));
	document->lookup = g_ptr_array_sized_new(count_records(layers) + 1);
	g_ptr_array_set_size(document->lookup, count_records(layers) + 1);
	document->root = g_new0(LayerTree, 1);
	build_tree(document->root, stream, 0, 0, document->lookup);
	if (!count_records(get_record(document->record, "j")))
		append_record(get_record(document->record, "j"), create_number((int)count_records(layers) - 1));
	return TRUE;
}

static Record *typed_value(const char *type, Record *value) {
	Record *descriptor = create_object(RECORD_PLAIN);
	set_string(descriptor, "t", type);
	set_record(descriptor, "v", value);
	return descriptor;
}

static Record *create_artboard_origin(void) {
	Record *origin = create_object(RECORD_PLAIN);
	set_string(origin, "classID", "Pnt");
	set_record(origin, "Hrzn", typed_value("doub", create_number(0)));
	set_record(origin, "Vrtc", typed_value("doub", create_number(0)));
	return typed_value("Objc", origin);
}

static void count_document_artboards(Document *document, const Record *layers) {
	guint count = 0;
	for (guint index = 0; index < count_records(layers); index++)
		if (has_record(get_record(get_item(layers, index), "add"), "artb"))
			count++;
	Record *descriptors = get_record(document->record, "add");
	if (!count) {
		remove_record(descriptors, "artd");
		return;
	}
	Record *artboards = create_object(RECORD_PLAIN);
	set_string(artboards, "classID", "null");
	set_record(artboards, "Cnt", typed_value("long", create_number(count)));
	set_record(artboards, "autoExpandOffset", create_artboard_origin());
	set_record(artboards, "origin", create_artboard_origin());
	set_record(artboards, "autoExpandEnabled", typed_value("bool", create_boolean(TRUE)));
	set_record(artboards, "autoNestEnabled", typed_value("bool", create_boolean(TRUE)));
	set_record(artboards, "autoPositionEnabled", typed_value("bool", create_boolean(TRUE)));
	set_record(descriptors, "artd", artboards);
}

gboolean replace_document_layers(Document *document, Record *layers, GError **error) {
	if (!validate_groups(layers, error))
		return FALSE;
	Record *old_layers = get_record(document->record, "Z");
	for (guint index = 0; index < count_records(old_layers); index++) {
		Record *layer = get_item(old_layers, index);
		if (g_ptr_array_find(layers->value.array, layer, NULL))
			continue;
		Record *cache = get_record(layer, "CS");
		set_record(cache, "pz", create_object(RECORD_PLAIN));
		set_record(cache, "qI", create_channels(0, 8));
		set_record(cache, "aPY", create_channels(0, 8));
		set_record(cache, "Bw", create_null());
		set_record(cache, "dM", create_null());
		Record *dirty = get_record(cache, "ey");
		set_record(cache, "ey",
				   dirty && dirty->type != RECORD_NULL ? union_extents(dirty, get_record(layer, "V"))
													   : copy_record(get_record(layer, "V")));
	}
	count_document_artboards(document, layers);
	set_record(document->record, "Z", copy_array_shallow(layers));
	return rebuild_layer_tree(document, error);
}

Record *get_active_layer(const Document *document) {
	Record *selected = get_record(document->record, "j");
	if (count_records(selected) != 1)
		return NULL;
	double index = get_item(selected, 0)->value.number;
	return index >= 0 ? get_item(get_record(document->record, "Z"), index) : NULL;
}

gboolean get_valid_document_dimensions(const Document *document, int *width, int *height) {
	Record *horizontal = get_record(document->record, "s");
	Record *vertical = get_record(document->record, "T");
	if (!horizontal || horizontal->type != RECORD_NUMBER || !vertical || vertical->type != RECORD_NUMBER)
		return FALSE;
	double columns = horizontal->value.number, rows = vertical->value.number;
	if (!isfinite(columns) || !isfinite(rows) || columns != trunc(columns) || rows != trunc(rows) || columns <= 0 ||
		rows <= 0 || columns > 32768 || rows > 32768 || columns * rows > DOCUMENT_MAX_PIXELS)
		return FALSE;
	*width = (int)columns;
	*height = (int)rows;
	return TRUE;
}

int document_width(const Document *document) {
	return get_number(document->record, "s");
}

int document_height(const Document *document) {
	return get_number(document->record, "T");
}

Document *create_transparent_document(int width, int height, unsigned depth, DocumentMode mode, const char *name,
									  GError **error) {
	if (width <= 0 || height <= 0 || width > 32768 || height > 32768 || (guint64)width * height > DOCUMENT_MAX_PIXELS ||
		(guint64)width * height * (depth / 2 + 4) > DOCUMENT_MAX_BYTES || (depth != 8 && depth != 16 && depth != 32)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, TEXT_INVALID_SIZE_DEPTH);
		return NULL;
	}
	/* This is ic.aBy's explicit 72 dpi RGB request without profile or artboard fields */
	g_autofree char *full_name = g_strconcat(name, mode == DOCUMENT_RASTER ? ".psd" : ".ai", NULL);
	Document *document = create_document_record(full_name, mode);
	set_number(document->record, "s", width);
	set_number(document->record, "T", height);
	set_number(document->record, "depth", depth);
	if (mode == DOCUMENT_VECTOR)
		return document;
	g_autoptr(Record) layers = create_array();
	Record *layer = create_document_layer(document, FALSE);
	name_layer(layer, "Background");
	set_number(get_record(layer, "add"), "lspf", 4);
	append_record(layers, layer);
	if (!replace_document_layers(document, layers, error)) {
		free_document(document);
		return NULL;
	}

	Record *composite = create_channels((gsize)width * height * 4, depth);
	if (!composite)
		goto allocation_failure;
	set_record(document->record, "buffer", composite);
	return document;
allocation_failure:
	free_document(document);
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, TEXT_CHANNEL_ALLOCATION_FAILED);
	return NULL;
}
