#include <math.h>
#include <string.h>

#include "blend.h"
#include "history.h"
#include "layer-commands.h"
#include "pixel-selection.h"
#include "../lang/en.h"

void invalidate_document(Document *document) {
	Record *record = document->record;
	g_autoptr(Record) extent = create_extent(0, 0, document_width(document), document_height(document));
	Record *dirty = get_record(record, "ey");
	set_boolean(record, "Ma", TRUE);
	set_record(record, "ey", dirty->type == RECORD_NULL ? reference_record(extent) : union_extents(dirty, extent));
}

/* Each layer named index list gets its own value or one shared value */
static void apply_layer_values(Record *data, Document *document, Record *layer_values, const char *field,
							   gboolean descriptor) {
	Record *record = document->record;
	set_boolean(record, "di", TRUE);
	const char *position = get_string(data, "xn") + 1;
	guint index = 0;
	while (*position != ']') {
		char *end;
		guint layer_index = g_ascii_strtoull(position, &end, 10);
		Record *layer = get_item(get_record(record, "Z"), layer_index);
		Record *value = layer_values->type == RECORD_ARRAY ? get_item(layer_values, index) : layer_values;
		set_record(descriptor ? get_record(layer, "add") : layer, field, copy_record(value));
		index++;
		position = *end == ',' ? end + 1 : end;
	}
	invalidate_document(document);
}

static void toggle_layer_visibility(Record *data, Document *document) {
	Record *indices = get_record(data, "xn"), *layers = get_record(document->record, "Z");
	set_boolean(document->record, "di", TRUE);
	for (guint index = 0; index < count_records(indices); index++) {
		Record *layer = get_item(layers, get_item(indices, index)->value.number);
		set_layer_visible(layer, !is_layer_visible(layer));
	}
	invalidate_document(document);
}

static void reset_channel_targets(Document *document);

static void apply_layer_list(Record *data, Document *document, gboolean redo) {
	Record *record = document->record;
	set_boolean(record, "di", TRUE);
	if (redo)
		reset_channel_targets(document);
	set_record(record, "j", copy_array_shallow(get_record(data, redo ? "vn" : "FD")));
	Record *selections = get_record(data, "Ra");
	if (selections && selections->type == RECORD_ARRAY) {
		set_record(record, "g", reference_record(get_item(selections, redo ? 1 : 0)));
		document->selection_changed = TRUE;
	}
	set_record(record, "AK", create_null());
	invalidate_document(document);
	replace_document_layers(document, get_record(data, redo ? "qr" : "Zl"), NULL);
}

static void apply_layer_names(Record *data, gboolean redo) {
	Record *changes = get_record(data, "$R");
	for (guint index = 0; index < count_records(changes); index++) {
		Record *change = get_item(changes, index);
		Record *layer = get_record(change, "layer");
		name_layer(layer, get_string(change, redo ? "next" : "previous"));
		Record *source = get_record(change, redo ? "next_source" : "previous_source");
		if (source && source->type == RECORD_STRING)
			set_string(get_record(layer, "add"), "lnsr", source->value.string);
		else
			remove_record(get_record(layer, "add"), "lnsr");
	}
}

static void toggle_layer_clipping(Record *data, Document *document) {
	Record *layer = get_item(get_record(document->record, "Z"), get_number(data, "z"));
	set_boolean(layer, "jl", !get_boolean(layer, "jl"));
	invalidate_document(document);
}

static void apply_layer_colors(Record *data, Document *document, gboolean redo) {
	Record *indices = get_record(data, "Z"), *colors = get_record(data, redo ? "ct" : "a4w");
	Record *layers = get_record(document->record, "Z");
	for (guint index = 0; index < count_records(indices); index++) {
		Record *descriptors = get_record(get_item(layers, get_item(indices, index)->value.number), "add");
		Record *color = get_item(colors, index);
		if (color->type == RECORD_NUMBER)
			set_number(descriptors, "lclr", color->value.number);
		else
			remove_record(descriptors, "lclr");
	}
}

static void apply_layer_command(Record *data, Document *document, gboolean redo) {
	const char *kind = get_string(data, "S");
	Record *values = get_record(data, redo ? "zQ" : "kg");
	if (!g_strcmp0(kind, "20"))
		apply_layer_values(data, document, values, "qu", FALSE);
	if (!g_strcmp0(kind, "21"))
		apply_layer_values(data, document, values, "nv", FALSE);
	if (!g_strcmp0(kind, "22"))
		apply_layer_values(data, document, values, "iOpa", TRUE);
	if (!g_strcmp0(kind, "23"))
		toggle_layer_visibility(data, document);
	if (!g_strcmp0(kind, "31"))
		toggle_layer_clipping(data, document);
	if (!g_strcmp0(kind, "40"))
		apply_layer_colors(data, document, redo);
	if (!g_strcmp0(kind, "38"))
		apply_layer_names(data, redo);
	if (!g_strcmp0(kind, "39"))
		apply_layer_list(data, document, redo);
	set_boolean(document->record, "Ma", TRUE);
}

static void undo_layer_command(Record *data, Document *document) {
	apply_layer_command(data, document, FALSE);
}

static void redo_layer_command(Record *data, Document *document) {
	apply_layer_command(data, document, TRUE);
}

static double get_layer_history_size(const Record *data) {
	return 0;
}

static const HistoryOperation layer_operation = {"233", undo_layer_command, redo_layer_command, get_layer_history_size};

static Record *collect_target_layers(Document *document, int layer_index, GError **error) {
	Record *layers = get_record(document->record, "Z"), *selected = get_record(document->record, "j");
	Record *indices = create_array();
	if (layer_index >= 0)
		append_record(indices, create_number(layer_index));
	else
		for (guint index = 0; index < count_records(selected); index++)
			append_record(indices, reference_record(get_item(selected, index)));
	for (guint index = 0; index < count_records(indices); index++) {
		Record *item = get_item(indices, index);
		if (item->type != RECORD_NUMBER || !isfinite(item->value.number) || item->value.number < 0 ||
			item->value.number != trunc(item->value.number) || item->value.number >= count_records(layers)) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, TEXT_INVALID_LAYER_SELECTION);
			free_record(indices);
			return NULL;
		}
	}
	return indices;
}

gboolean change_layer_fill_opacity(Document *document, int layer_index, double opacity, gboolean extended_history,
								   GError **error) {
	Record *record = document->record, *layers = get_record(record, "Z");
	g_autoptr(Record) indices = collect_target_layers(document, layer_index, error);
	if (!indices)
		return FALSE;
	set_boolean(record, "di", TRUE);
	g_autofree char *selection = describe_record(indices);
	Record *entry = get_latest_history_entry(document);
	Record *data = get_record(entry, "data");
	if (!data || g_strcmp0(get_string(data, "S"), "22") || g_strcmp0(get_string(data, "xn"), selection)) {
		Record *before = create_array();
		for (guint index = 0; index < count_records(indices); index++) {
			Record *layer = get_item(layers, get_item(indices, index)->value.number);
			Record *fill = get_record(get_record(layer, "add"), "iOpa");
			append_record(before, create_number(!fill || fill->type == RECORD_NULL ? 255 : fill->value.number));
		}
		entry = create_history_entry(create_string("Fill Opacity Change"), "233", FALSE, g_get_real_time() / 1000);
		data = create_object(RECORD_PLAIN);
		set_string(data, "S", "22");
		set_string(data, "xn", selection);
		set_record(data, "kg", before);
		set_record(entry, "data", data);
		append_history_entry(document, entry, &layer_operation, extended_history);
	}
	set_number(data, "zQ", opacity);
	redo_layer_command(data, document);
	return TRUE;
}

static double get_timeline_time(const Record *record) {
	Record *timeline = get_record(record, "vA");
	if (!timeline || timeline->type == RECORD_NULL)
		return 0;
	Record *time = get_record(get_record(timeline, "time"), "v");
	return get_number(get_record(time, "numerator"), "v") / get_number(get_record(time, "denominator"), "v");
}

static Record *create_opacity_descriptor(void) {
	Record *descriptor = create_object(RECORD_PLAIN), *target = create_object(RECORD_PLAIN);
	Record *reference = create_object(RECORD_PLAIN), *references = create_array();
	Record *enumeration = create_object(RECORD_PLAIN), *ordinal = create_object(RECORD_PLAIN);
	set_string(descriptor, "classID", "null");
	set_string(ordinal, "classID", "Lyr");
	set_string(ordinal, "typeID", "Ordn");
	set_string(ordinal, "enum", "Trgt");
	set_string(enumeration, "t", "Enmr");
	set_record(enumeration, "v", ordinal);
	append_record(references, enumeration);
	set_string(reference, "t", "obj ");
	set_record(reference, "v", references);
	set_record(descriptor, "null", reference);
	Record *layer = create_object(RECORD_PLAIN), *amount = create_object(RECORD_PLAIN);
	Record *unit = create_object(RECORD_PLAIN);
	set_string(unit, "type", "#Prc");
	set_number(unit, "val", 0);
	set_string(amount, "t", "UntF");
	set_record(amount, "v", unit);
	set_string(layer, "classID", "Lyr");
	set_record(layer, "Opct", amount);
	set_string(target, "t", "Objc");
	set_record(target, "v", layer);
	set_record(descriptor, "T", target);
	return descriptor;
}

gboolean change_layer_opacity(Document *document, int layer_index, double opacity, gboolean extended_history,
							  GError **error) {
	Record *record = document->record, *layers = get_record(record, "Z");
	g_autoptr(Record) indices = collect_target_layers(document, layer_index, error);
	if (!indices)
		return FALSE;
	for (guint index = 0; index < count_records(indices); index++) {
		Record *layer = get_item(layers, get_item(indices, index)->value.number);
		Record *metadata = get_record(get_record(layer, "add"), "shmd");
		if (metadata && metadata->type == RECORD_OBJECT && has_record(metadata, "tmln")) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_UNSUPPORTED_ANIMATED_OPACITY);
			return FALSE;
		}
	}
	double time = get_timeline_time(record);
	set_boolean(record, "di", TRUE);
	g_autofree char *selection = describe_record(indices);
	Record *entry = get_latest_history_entry(document);
	Record *data = get_record(entry, "data");
	if (!data || g_strcmp0(get_string(data, "S"), "21") || g_strcmp0(get_string(data, "xn"), selection) ||
		get_number(data, "aPZ") != time) {
		Record *before = create_array();
		for (guint index = 0; index < count_records(indices); index++) {
			Record *layer = get_item(layers, get_item(indices, index)->value.number);
			append_record(before, create_number(get_number(layer, "nv")));
		}
		Record *name = create_array();
		append_record(name, create_number(6));
		append_record(name, create_number(32));
		entry = create_history_entry(name, "233", FALSE, g_get_real_time() / 1000);
		data = create_object(RECORD_PLAIN);
		set_string(data, "S", "21");
		set_string(data, "xn", selection);
		set_record(data, "kg", before);
		set_number(data, "aPZ", time);
		set_record(data, "ajJ", create_opacity_descriptor());
		set_record(entry, "data", data);
		append_history_entry(document, entry, &layer_operation, extended_history);
	}
	Record *after = create_array();
	for (guint index = 0; index < count_records(indices); index++)
		append_record(after, create_number(opacity));
	set_record(data, "zQ", after);
	Record *unit = get_record(get_record(get_record(get_record(get_record(data, "ajJ"), "T"), "v"), "Opct"), "v");
	set_number(unit, "val", round(opacity * 100 / 255));
	redo_layer_command(data, document);
	return TRUE;
}

gboolean change_layer_blend_mode(Document *document, int layer_index, const char *mode, gboolean extended_history,
								 GError **error) {
	Record *layers = get_record(document->record, "Z");
	if (find_blend_mode(mode) < 0 && g_strcmp0(mode, "pass")) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, TEXT_INVALID_LAYER_SELECTION);
		return FALSE;
	}
	g_autoptr(Record) indices = collect_target_layers(document, layer_index, error);
	if (!indices)
		return FALSE;
	Record *before = create_array(), *after = create_array();
	for (guint index = 0; index < count_records(indices); index++) {
		Record *layer = get_item(layers, get_item(indices, index)->value.number);
		const char *previous = get_string(layer, "qu");
		append_record(before, create_string(previous));
		append_record(after, create_string(!g_strcmp0(mode, "pass") && !is_layer_group(layer) ? previous : mode));
	}
	Record *name = create_array();
	append_record(name, create_number(6));
	append_record(name, create_number(33));
	Record *entry = create_history_entry(name, "233", FALSE, g_get_real_time() / 1000);
	Record *data = create_object(RECORD_PLAIN);
	g_autofree char *selection = describe_record(indices);
	set_string(data, "S", "20");
	set_string(data, "xn", selection);
	set_record(data, "kg", before);
	set_record(data, "zQ", after);
	set_record(entry, "data", data);
	append_history_entry(document, entry, &layer_operation, extended_history);
	redo_layer_command(data, document);
	return TRUE;
}

static gboolean find_visibility_target(Document *document, int layer_index, double *target, GError **error) {
	Record *record = document->record, *layers = get_record(record, "Z"), *selected = get_record(record, "j");
	*target = layer_index >= 0			? layer_index
			  : count_records(selected) ? get_item(selected, 0)->value.number
										: (double)count_records(layers) - 1;
	if (isfinite(*target) && *target >= 0 && *target == trunc(*target) && *target < count_records(layers))
		return TRUE;
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, TEXT_INVALID_LAYER_SELECTION);
	return FALSE;
}

static void record_visibility(Document *document, Record *indices, double target, gboolean extended_history) {
	set_boolean(document->record, "di", TRUE);
	Record *entry = create_history_entry(create_string("Layer visibility"), "233", TRUE, g_get_real_time() / 1000);
	Record *data = create_object(RECORD_PLAIN);
	set_string(data, "S", "23");
	set_record(data, "xn", indices);
	set_number(data, "z", target);
	set_record(entry, "data", data);
	append_history_entry(document, entry, &layer_operation, extended_history);
	redo_layer_command(data, document);
}

gboolean toggle_layer_visible(Document *document, int layer_index, gboolean extended_history, GError **error) {
	double target;
	if (!find_visibility_target(document, layer_index, &target, error))
		return FALSE;
	Record *indices = create_array();
	append_record(indices, create_number(target));
	record_visibility(document, indices, target, extended_history);
	return TRUE;
}

static gboolean step_back_over_solo(Document *document, double target) {
	Record *latest = get_latest_history_entry(document);
	Record *data = latest ? get_record(latest, "data") : NULL;
	if (!data || data->type != RECORD_OBJECT || g_strcmp0(get_string(get_record(latest, "U"), "id"), "233") ||
		g_strcmp0(get_string(data, "S"), "23") || count_records(get_record(data, "xn")) == 1)
		return FALSE;
	double previous = get_number(data, "z");
	undo_document_command(document);
	return previous == target;
}

gboolean solo_layer_visible(Document *document, int layer_index, gboolean extended_history, GError **error) {
	double target;
	if (!find_visibility_target(document, layer_index, &target, error))
		return FALSE;
	if (step_back_over_solo(document, target))
		return TRUE;
	Record *layers = get_record(document->record, "Z");
	LayerTree *node = g_ptr_array_index(document->lookup, (guint)target);
	g_autoptr(GArray) shown = g_array_new(FALSE, FALSE, sizeof(int));
	g_array_append_val(shown, (int){target});
	for (LayerTree *branch = node; branch->parent != document->root; branch = branch->parent)
		g_array_append_val(shown, branch->parent->index);
	Record *indices = create_array();
	for (guint index = 0; index < count_records(layers); index++) {
		Record *layer = get_item(layers, index);
		LayerTree *other = g_ptr_array_index(document->lookup, index);
		gboolean keep = FALSE;
		for (guint position = 0; position < shown->len; position++)
			keep |= g_array_index(shown, int, position) == (int)index;
		if (!keep)
			keep = is_layer_visible(layer) && node->depth < other->depth;
		if (keep != is_layer_visible(layer))
			append_record(indices, create_number(index));
	}
	record_visibility(document, indices, target, extended_history);
	return TRUE;
}

static Record *get_editable_mask(const Record *layer) {
	Record *alternate = get_record(layer, "E_"), *vector = get_record(get_record(layer, "add"), "vmsk");
	if (alternate && alternate->type == RECORD_OBJECT)
		return alternate;
	if (vector && vector->type == RECORD_OBJECT && get_boolean(vector, "isEnabled"))
		return NULL;
	Record *mask = get_record(layer, "yy");
	return mask && mask->type == RECORD_OBJECT ? mask : NULL;
}

static Record *get_smart_filter_entry(const Record *layer, const Record *document) {
	Record *smart = get_record(get_record(layer, "add"), "SoLd");
	if (!smart || smart->type != RECORD_OBJECT)
		return NULL;
	const char *placed = get_string(get_record(smart, "placed"), "v");
	Record *entries = get_record(get_record(document, "add"), "FEid");
	for (guint index = 0; entries && placed && index < count_records(entries); index++)
		if (!g_strcmp0(get_string(get_item(entries, index), "id"), placed))
			return get_item(entries, index);
	return NULL;
}

static void clear_mask_highlight(Record *mask, Record *document) {
	if (!mask || mask->type != RECORD_OBJECT || !get_boolean(mask, "Hl"))
		return;
	set_boolean(mask, "Hl", FALSE);
	set_boolean(document, "z3", TRUE);
}

static void clear_array_field(Record *object, const char *key) {
	Record *array = get_record(object, key);
	if (array && array->type == RECORD_ARRAY && count_records(array))
		set_record(object, key, create_array());
}

static void reset_channel_targets(Document *document) {
	Record *record = document->record, *channels = get_record(get_record(record, "u"), "RD");
	GString *joined = g_string_new(NULL);
	for (guint index = 0; index < count_records(channels); index++) {
		g_autofree char *text = describe_record(get_item(channels, index));
		g_string_append(joined, text);
	}
	if (strcmp(joined->str, "111")) {
		Record *all = create_array();
		for (int channel = 0; channel < 3; channel++)
			append_record(all, create_number(1));
		set_record(get_record(record, "u"), "RD", all);
		set_boolean(record, "z3", TRUE);
	}
	g_string_free(joined, TRUE);
	Record *layers = get_record(record, "Z");
	for (guint index = 0; index < count_records(layers); index++) {
		Record *layer = get_item(layers, index), *smart = get_record(get_record(layer, "add"), "SoLd");
		if (smart && smart->type == RECORD_OBJECT && has_record(smart, "filterFX") &&
			get_record(smart, "filterFX")->type != RECORD_NULL) {
			Record *entry = get_smart_filter_entry(layer, record);
			if (entry)
				clear_mask_highlight(get_record(entry, "yy"), record);
		}
		clear_mask_highlight(get_editable_mask(layer), record);
		Record *vector = get_record(get_record(layer, "add"), "vmsk");
		if (!vector || vector->type != RECORD_OBJECT)
			continue;
		clear_array_field(vector, "j");
		clear_array_field(vector, "sW");
	}
}

static void open_selected_groups(Document *document) {
	Record *record = document->record, *selected = get_record(record, "j");
	if (count_records(selected) != 1)
		return;
	LayerTree *node = g_ptr_array_index(document->lookup, (guint)get_item(selected, 0)->value.number);
	for (; node->parent; node = node->parent)
		set_number(get_record(node->parent->layer, "add"), "lsct", 1);
	set_boolean(record, "z3", TRUE);
	set_boolean(record, "azR", TRUE);
}

static gint compare_layer_indices(gconstpointer first, gconstpointer second) {
	double a = (*(Record *const *)first)->value.number, b = (*(Record *const *)second)->value.number;
	return a < b ? -1 : a > b;
}

static int find_selected(const Record *selected, int layer_index) {
	for (guint index = 0; index < count_records(selected); index++)
		if (get_item(selected, index)->value.number == layer_index)
			return index;
	return -1;
}

gboolean select_layer(Document *document, int layer_index, gboolean add, gboolean range, GError **error) {
	Record *record = document->record, *layers = get_record(record, "Z");
	if (layer_index < 0) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, TEXT_INVALID_LAYER_SELECTION);
		return FALSE;
	}
	set_boolean(record, "di", TRUE);
	reset_channel_targets(document);
	Record *selected = get_record(record, "j");
	if (add) {
		int position = find_selected(selected, layer_index);
		if (position < 0)
			append_record(selected, create_number(layer_index));
		else if (count_records(selected) > 1)
			g_ptr_array_remove_index(selected->value.array, position);
		g_ptr_array_sort(selected->value.array, compare_layer_indices);
	} else if (range && count_records(selected)) {
		int first = MIN(layer_index, (int)get_item(selected, 0)->value.number);
		int last = MAX(layer_index, (int)get_item(selected, count_records(selected) - 1)->value.number);
		Record *chosen = create_array();
		for (int index = first; index <= last && (guint)index < count_records(layers); index++)
			if (get_number(get_record(get_item(layers, index), "add"), "lsct") != 3)
				append_record(chosen, create_number(index));
		set_record(record, "j", chosen);
	} else {
		if ((guint)layer_index >= count_records(layers))
			return TRUE;
		Record *layer = get_item(layers, layer_index);
		if (count_records(selected) == 1 && get_item(selected, 0)->value.number == layer_index &&
			get_number(layer, "A8") == 0) {
			set_boolean(record, "di", FALSE);
			return TRUE;
		}
		Record *single = create_array();
		append_record(single, create_number(layer_index));
		set_record(record, "j", single);
		open_selected_groups(document);
	}
	selected = get_record(record, "j");
	gboolean single = count_records(selected) == 1;
	int kept = single ? (int)get_item(selected, 0)->value.number : -1;
	for (guint index = 0; index < count_records(layers); index++) {
		if ((int)index == kept)
			continue;
		set_number(get_item(layers, index), "A8", 0);
		set_boolean(get_item(layers, index), "WF", FALSE);
	}
	if (single && (guint)kept < count_records(layers))
		set_number(get_item(layers, kept), "A8", 0);
	set_record(record, "AK", create_null());
	set_boolean(record, "Ma", TRUE);
	return TRUE;
}

void toggle_group_open(Document *document, int layer_index, gboolean all_siblings) {
	Record *record = document->record, *layer = get_item(get_record(record, "Z"), layer_index);
	set_boolean(record, "di", TRUE);
	double state = get_number(get_record(layer, "add"), "lsct") == 1 ? 2 : 1;
	LayerTree *node = g_ptr_array_index(document->lookup, layer_index);
	if (all_siblings) {
		GPtrArray *siblings = node->parent->children;
		for (guint index = 0; index < siblings->len; index++) {
			Record *sibling = ((LayerTree *)g_ptr_array_index(siblings, index))->layer;
			if (is_layer_group(sibling))
				set_number(get_record(sibling, "add"), "lsct", state);
		}
	} else
		set_number(get_record(layer, "add"), "lsct", state);
	Record *selected = get_record(record, "j");
	gboolean selection_inside = FALSE;
	for (guint index = 0; index < count_records(selected); index++) {
		double selected_index = get_item(selected, index)->value.number;
		if (node->closing_index < selected_index && selected_index < node->index)
			selection_inside = TRUE;
	}
	if (selection_inside) {
		Record *group = create_array();
		append_record(group, create_number(node->index));
		set_record(record, "j", group);
	}
	set_boolean(record, "Ma", TRUE);
}

/* ============================== Layer list commands ============================== */

Record *create_layer_list_data(Document *document, Record *next, Record *previous_selection, Record *next_selection) {
	Record *data = create_object(RECORD_PLAIN);
	set_string(data, "S", "39");
	set_record(data, "Zl", copy_array_shallow(get_record(document->record, "Z")));
	set_record(data, "qr", next);
	set_record(data, "FD", previous_selection);
	set_record(data, "vn", next_selection);
	return data;
}

void commit_layer_list(Document *document, Record *data, Record *name, gboolean extended_history) {
	Record *entry = create_history_entry(name, "233", FALSE, g_get_real_time() / 1000);
	set_record(entry, "data", data);
	append_history_entry(document, entry, &layer_operation, extended_history);
	redo_layer_command(data, document);
}

void record_layer_list(Document *document, Record *name, Record *next, Record *previous_selection,
					   Record *next_selection, gboolean extended_history) {
	commit_layer_list(document, create_layer_list_data(document, next, previous_selection, next_selection), name,
					  extended_history);
}

Record *create_index_list(int first) {
	Record *list = create_array();
	append_record(list, create_number(first));
	return list;
}

int find_highest_suffix(const Record *layers, const char *prefix) {
	int highest = 0;
	for (guint index = 0; index < count_records(layers); index++) {
		const char *name = get_layer_name(get_item(layers, index));
		if (!name || !g_str_has_prefix(name, prefix))
			continue;
		g_autofree char *rest = g_strstrip(g_strdup(name + strlen(prefix)));
		char *end;
		long number = strtol(rest, &end, 10);
		g_autofree char *again = g_strdup_printf("%ld", number);
		if (*rest && !*end && !strcmp(again, rest) && number > highest)
			highest = number;
	}
	return highest;
}

gboolean add_new_layer(Document *document, gboolean below, gboolean extended_history, GError **error) {
	Record *record = document->record, *layers = get_record(record, "Z"), *selected = get_record(record, "j");
	set_boolean(record, "di", TRUE);
	Record *layer = create_document_layer(document, FALSE);
	g_autofree char *name = g_strdup_printf("Layer %d", find_highest_suffix(layers, "Layer ") + 1);
	name_layer(layer, name);
	int last = count_records(selected) ? get_item(selected, count_records(selected) - 1)->value.number
									   : (int)count_records(layers) - 1;
	int position = last + (below ? 0 : 1);
	Record *anchor = get_item(layers, last);
	if (anchor && get_number(get_record(anchor, "add"), "lsct") == 1)
		position--;
	position = CLAMP(position, 0, (int)count_records(layers));
	Record *next = copy_array_shallow(layers);
	g_ptr_array_insert(next->value.array, position, layer);
	record_layer_list(document, create_history_name(6, 13), next, copy_array_shallow(selected),
					  create_index_list(position), extended_history);
	return TRUE;
}

GArray *collect_subtrees(Document *document, const Record *chosen) {
	GArray *indices = g_array_new(FALSE, FALSE, sizeof(int));
	for (guint index = 0; index < count_records(chosen); index++) {
		LayerTree *node = g_ptr_array_index(document->lookup, (guint)get_item(chosen, index)->value.number);
		GPtrArray *pending = g_ptr_array_new();
		g_ptr_array_add(pending, node);
		while (pending->len) {
			LayerTree *current = g_ptr_array_steal_index(pending, 0);
			gboolean seen = FALSE;
			for (guint item = 0; item < indices->len; item++)
				if (g_array_index(indices, int, item) == current->index)
					seen = TRUE;
			if (!seen)
				g_array_append_val(indices, current->index);
			if (current->closing && current->children) {
				g_array_append_val(indices, current->closing_index);
				for (guint child = 0; child < current->children->len; child++)
					g_ptr_array_insert(pending, child, g_ptr_array_index(current->children, child));
			}
		}
		g_ptr_array_unref(pending);
	}
	return indices;
}

GArray *collect_selected_subtrees(Document *document) {
	return collect_subtrees(document, get_record(document->record, "j"));
}

gboolean contains_index(const GArray *indices, int value) {
	for (guint index = 0; index < indices->len; index++)
		if (g_array_index(indices, int, index) == value)
			return TRUE;
	return FALSE;
}

gboolean delete_selected_layers(Document *document, gboolean extended_history, GError **error) {
	Record *record = document->record, *layers = get_record(record, "Z");
	g_autoptr(GArray) removed = collect_selected_subtrees(document);
	if (!removed->len)
		return FALSE;
	if (removed->len == count_records(layers)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, TEXT_KEEP_ONE_LAYER);
		return FALSE;
	}
	Record *next = create_array();
	int lowest = G_MAXINT;
	for (guint index = 0; index < count_records(layers); index++)
		if (!contains_index(removed, index))
			append_record(next, reference_record(get_item(layers, index)));
	for (guint index = 0; index < removed->len; index++)
		lowest = MIN(lowest, g_array_index(removed, int, index));
	while (lowest > (int)count_records(next) - 1)
		lowest--;
	lowest = MAX(lowest - 1, 0);
	while ((guint)lowest < count_records(next) - 1 &&
		   !g_strcmp0(get_string(get_item(next, lowest), "name"), "</Layer group>"))
		lowest++;
	set_boolean(record, "di", TRUE);
	record_layer_list(document, create_history_name(6, 31), next, copy_array_shallow(get_record(record, "j")),
					  create_index_list(lowest), extended_history);
	return TRUE;
}

gint compare_indices(gconstpointer first, gconstpointer second) {
	return *(const int *)first - *(const int *)second;
}

static char *choose_copy_name(const char *name, GHashTable *names) {
	gsize length = strlen(name), digits = length;
	while (digits > 0 && g_ascii_isdigit(name[digits - 1]))
		digits--;
	long number = 0;
	g_autofree char *stem = NULL;
	if (digits == length) {
		if (g_str_has_suffix(name, " copy")) {
			number = 1;
			stem = g_strndup(name, length - 5);
		} else
			stem = g_strdup(name);
	} else {
		number = strtol(name + digits, NULL, 10);
		g_autofree char *before = g_strndup(name, digits);
		if (g_str_has_suffix(before, " copy "))
			stem = g_strndup(name, digits - 6);
		else {
			number = 0;
			stem = g_strdup(name);
		}
	}
	for (number++;; number++) {
		char *candidate = number == 1 ? g_strdup_printf("%s copy", stem) : g_strdup_printf("%s copy %ld", stem, number);
		if (!g_hash_table_contains(names, candidate))
			return candidate;
		g_free(candidate);
	}
}

gboolean duplicate_selected_layers(Document *document, gboolean extended_history, GError **error) {
	Record *record = document->record, *layers = get_record(record, "Z"), *selected = get_record(record, "j");
	if (!count_records(selected))
		return FALSE;
	g_autoptr(GArray) sources = collect_selected_subtrees(document);
	g_array_sort(sources, compare_indices);
	g_autoptr(GHashTable) names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (guint index = 0; index < count_records(layers); index++)
		g_hash_table_add(names, g_strdup(get_layer_name(get_item(layers, index))));
	g_autoptr(GPtrArray) copies = g_ptr_array_new();
	for (guint index = 0; index < sources->len; index++) {
		int source = g_array_index(sources, int, index);
		Record *copy = copy_layer_record(get_item(layers, source));
		set_number(get_record(copy, "add"), "lyid", next_layer_id(document));
		set_number(get_record(copy, "add"), "lspf", 0);
		LayerTree *node = g_ptr_array_index(document->lookup, source);
		gboolean inside = node->parent && contains_index(sources, node->parent->index);
		const char *name = get_layer_name(get_item(layers, source));
		char *chosen = choose_copy_name(name, names);
		if (!strcmp(name, "Background") && count_records(layers) == 1) {
			g_free(chosen);
			chosen = g_strdup("Layer 1");
		}
		if (!inside)
			name_layer(copy, chosen);
		g_hash_table_add(names, chosen);
		g_ptr_array_add(copies, copy);
	}
	g_autoptr(GArray) previous = g_array_new(FALSE, FALSE, sizeof(int));
	for (guint index = 0; index < count_records(selected); index++) {
		int value = get_item(selected, index)->value.number;
		g_array_append_val(previous, value);
	}
	g_array_sort(previous, compare_indices);
	int position = g_array_index(previous, int, previous->len - 1) + 1;
	Record *next = copy_array_shallow(layers), *chosen = create_array();
	int depth = 0, groups = 0;
	for (guint index = 0; index < copies->len; index++) {
		Record *copy = g_ptr_array_index(copies, index);
		g_ptr_array_insert(next->value.array, position, copy);
		append_record(chosen, create_number(position++));
		double section = get_number(get_record(copy, "add"), "lsct");
		if (section == 3) {
			if (depth == 0)
				groups++;
			depth++;
		} else if (section == 1 || section == 2)
			depth--;
	}
	if (groups == 1) {
		free_record(chosen);
		chosen = create_index_list(position - 1);
	}
	Record *previous_selection = create_array();
	for (guint index = 0; index < previous->len; index++)
		append_record(previous_selection, create_number(g_array_index(previous, int, index)));
	set_boolean(record, "di", TRUE);
	record_layer_list(document, create_history_name(6, 0), next, previous_selection, chosen, extended_history);
	return TRUE;
}

gboolean rename_layer(Document *document, int layer_index, const char *name, gboolean extended_history,
					  GError **error) {
	Record *record = document->record, *layer = get_item(get_record(record, "Z"), layer_index);
	if (!layer) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, TEXT_INVALID_LAYER_SELECTION);
		return FALSE;
	}
	g_autofree char *trimmed = g_utf8_substring(name, 0, MIN(g_utf8_strlen(name, -1), 255));
	if (!g_strcmp0(get_layer_name(layer), trimmed))
		return FALSE;
	set_boolean(record, "di", TRUE);
	Record *change = create_object(RECORD_PLAIN), *changes = create_array(), *data = create_object(RECORD_PLAIN);
	set_record(change, "layer", reference_record(layer));
	set_string(change, "previous", get_layer_name(layer));
	set_string(change, "next", trimmed);
	const char *source = get_string(get_record(layer, "add"), "lnsr");
	set_record(change, "previous_source", source ? create_string(source) : create_null());
	set_record(change, "next_source", create_null());
	append_record(changes, change);
	set_string(data, "S", "38");
	set_record(data, "$R", changes);
	Record *entry = create_history_entry(create_history_name(6, 37), "233", FALSE, g_get_real_time() / 1000);
	set_record(entry, "data", data);
	append_history_entry(document, entry, &layer_operation, extended_history);
	redo_layer_command(data, document);
	return TRUE;
}

/* ============================== Layer order ============================== */

static GArray *collect_subtree(Document *document, int index) {
	GArray *indices = g_array_new(FALSE, FALSE, sizeof(int));
	if (index < 0 || (guint)index >= document->lookup->len)
		return indices;
	LayerTree *node = g_ptr_array_index(document->lookup, index);
	if (!node || node->index != index)
		return indices;
	GPtrArray *pending = g_ptr_array_new();
	g_ptr_array_add(pending, node);
	while (pending->len) {
		LayerTree *current = g_ptr_array_steal_index(pending, 0);
		g_array_append_val(indices, current->index);
		if (current->closing && current->children) {
			g_array_append_val(indices, current->closing_index);
			for (guint child = 0; child < current->children->len; child++)
				g_ptr_array_insert(pending, child, g_ptr_array_index(current->children, child));
		}
	}
	g_ptr_array_unref(pending);
	return indices;
}

gboolean move_layers_to(Document *document, int source, int target, gboolean below, gboolean copy,
						gboolean extended_history, GError **error) {
	Record *record = document->record, *layers = get_record(record, "Z"), *selected = get_record(record, "j");
	if (target < 0 || target >= (int)count_records(layers) || source < 0 || source >= (int)count_records(layers))
		return FALSE;
	gboolean in_selection = FALSE;
	for (guint index = 0; index < count_records(selected); index++)
		if (get_item(selected, index)->value.number == source)
			in_selection = TRUE;
	g_autoptr(GArray) moving = in_selection ? collect_selected_subtrees(document) : collect_subtree(document, source);
	g_array_sort(moving, compare_indices);
	if (!moving->len || (contains_index(moving, target) && !copy))
		return FALSE;
	Record *anchor = get_item(layers, target), *next = create_array();
	for (guint index = 0; index < count_records(layers); index++)
		if (copy || !contains_index(moving, index))
			append_record(next, reference_record(get_item(layers, index)));
	int position = 0;
	while (position < (int)count_records(next) && get_item(next, position) != anchor)
		position++;
	g_autoptr(GHashTable) names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	for (guint index = 0; copy && index < count_records(layers); index++)
		g_hash_table_add(names, g_strdup(get_layer_name(get_item(layers, index))));
	Record *chosen = create_array();
	for (guint index = 0; index < moving->len; index++) {
		int at = position + (below ? 0 : 1) + index;
		Record *layer = get_item(layers, g_array_index(moving, int, index));
		if (copy) {
			layer = copy_layer_record(layer);
			set_number(get_record(layer, "add"), "lyid", next_layer_id(document));
			set_number(get_record(layer, "add"), "lspf", 0);
			if (get_number(get_record(layer, "add"), "lsct") != 3) {
				char *name = choose_copy_name(get_layer_name(layer), names);
				name_layer(layer, name);
				g_hash_table_add(names, name);
			}
		} else
			layer = reference_record(layer);
		g_ptr_array_insert(next->value.array, at, layer);
		append_record(chosen, create_number(at));
	}
	set_boolean(record, "di", TRUE);
	record_layer_list(document, create_history_name(6, copy ? 0 : 35), next, copy_array_shallow(selected), chosen,
					  extended_history);
	return TRUE;
}

gboolean arrange_layers(Document *document, LayerArrangement arrangement, gboolean extended_history, GError **error) {
	Record *record = document->record, *layers = get_record(record, "Z"), *selected = get_record(record, "j");
	if (!count_records(selected))
		return FALSE;
	int source = get_item(selected, 0)->value.number, count = count_records(layers), target = 0;
	g_autoptr(GArray) moving = collect_selected_subtrees(document);
	g_array_sort(moving, compare_indices);
	gboolean below = arrangement == ARRANGE_BACKWARD || arrangement == ARRANGE_BACK;
	if (arrangement == ARRANGE_FRONT)
		target = count - 1;
	if (arrangement == ARRANGE_FORWARD || arrangement == ARRANGE_BACKWARD) {
		target = arrangement == ARRANGE_FORWARD ? g_array_index(moving, int, moving->len - 1) + 1
												: g_array_index(moving, int, 0) - 1;
		g_autoptr(GArray) passed = collect_subtree(document, target);
		if (!contains_index(passed, source) && passed->len)
			target += (arrangement == ARRANGE_FORWARD ? 1 : -1) * ((int)passed->len - 1);
	}
	if (arrangement == ARRANGE_BACK && count &&
		!g_strcmp0(get_string(get_record(get_item(layers, 0), "add"), "lnsr"), "bgnd"))
		target = 1;
	if (target < 0 || target > count - 1 || contains_index(moving, target))
		return FALSE;
	return move_layers_to(document, source, target, below, FALSE, extended_history, error);
}

gboolean can_clip_layer(Document *document, int layer_index) {
	Record *layers = get_record(document->record, "Z");
	if (layer_index < 0 || layer_index >= (int)count_records(layers))
		return FALSE;
	LayerTree *node = g_ptr_array_index(document->lookup, layer_index);
	if (!node || !node->parent || is_layer_group(get_item(layers, layer_index)))
		return FALSE;
	return g_ptr_array_index(node->parent->children, 0) != node;
}

gboolean toggle_clipping_mask(Document *document, gboolean extended_history, GError **error) {
	Record *layer = get_active_layer(document);
	int index = layer ? (int)get_item(get_record(document->record, "j"), 0)->value.number : -1;
	if (!layer || !can_clip_layer(document, index))
		return FALSE;
	Record *data = create_object(RECORD_PLAIN);
	set_string(data, "S", "31");
	set_number(data, "z", index);
	commit_layer_list(document, data, create_history_name(6, get_boolean(layer, "jl") ? 19 : 18), extended_history);
	return TRUE;
}

gboolean set_layer_color(Document *document, int color, gboolean extended_history, GError **error) {
	if (!count_records(get_record(document->record, "j")))
		return FALSE;
	g_autoptr(GArray) targets = collect_selected_subtrees(document);
	Record *layers = get_record(document->record, "Z");
	Record *data = create_object(RECORD_PLAIN), *indices = create_array(), *before = create_array();
	Record *after = create_array();
	for (guint index = 0; index < targets->len; index++) {
		int target = g_array_index(targets, int, index);
		Record *previous = get_record(get_record(get_item(layers, target), "add"), "lclr");
		append_record(indices, create_number(target));
		append_record(before, previous && previous->type == RECORD_NUMBER ? create_number(previous->value.number)
																		  : create_null());
		append_record(after, create_number(color));
	}
	set_string(data, "S", "40");
	set_record(data, "Z", indices);
	set_record(data, "a4w", before);
	set_record(data, "ct", after);
	commit_layer_list(document, data, create_history_name(6, 38), extended_history);
	return TRUE;
}
