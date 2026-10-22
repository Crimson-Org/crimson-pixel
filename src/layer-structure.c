#include "layer-structure.h"
#include "layer-commands.h"
#include "blend.h"
#include "history.h"
#include "paint.h"
#include "../lang/en.h"

enum { SECTION_OPEN = 1, SECTION_CLOSED = 2 };

static int get_last_selected(Record *selected) {
	return get_item(selected, count_records(selected) - 1)->value.number;
}

static Record *create_merge_name(int item) {
	Record *name = create_history_name(6, 12);
	append_record(name, create_number(item));
	return name;
}

static Record *create_number_list(const GArray *indices) {
	Record *list = create_array();
	for (guint index = 0; index < indices->len; index++)
		append_record(list, create_number(g_array_index(indices, int, index)));
	return list;
}

gboolean group_selected_layers(Document *document, gboolean extended_history, GError **error) {
	Record *record = document->record, *layers = get_record(record, "Z"), *selected = get_record(record, "j");
	if (!count_records(selected))
		return FALSE;
	g_autoptr(GArray) members = collect_selected_subtrees(document);
	if (!members->len)
		return FALSE;
	g_array_sort(members, compare_indices);
	Record *group = create_document_layer(document, FALSE);
	g_autofree char *name =
		g_strdup_printf("%s %d", TEXT_GROUP_NAME, find_highest_suffix(layers, TEXT_GROUP_NAME " ") + 1);
	name_layer(group, name);
	set_string(group, "qu", "pass");
	set_number(get_record(group, "add"), "lsct", count_records(selected) == 1 ? SECTION_OPEN : SECTION_CLOSED);
	set_number(group, "NF", 24);
	Record *next = create_array();
	for (guint index = 0; index < count_records(layers); index++)
		if (!contains_index(members, index))
			append_record(next, reference_record(get_item(layers, index)));
	int place = get_last_selected(selected) - (int)members->len + 1;
	place = CLAMP(place, 0, (int)count_records(next));
	g_ptr_array_insert(next->value.array, place, create_group_closing(document, FALSE));
	for (guint index = 0; index < members->len; index++)
		g_ptr_array_insert(next->value.array, place + 1 + index,
						   reference_record(get_item(layers, g_array_index(members, int, index))));
	int position = place + 1 + members->len;
	g_ptr_array_insert(next->value.array, position, group);
	set_boolean(record, "di", TRUE);
	record_layer_list(document, create_history_name(6, 9), next, copy_array_shallow(selected),
					  create_index_list(position), extended_history);
	return TRUE;
}

gboolean ungroup_selected_layer(Document *document, gboolean extended_history, GError **error) {
	Record *record = document->record, *layers = get_record(record, "Z"), *selected = get_record(record, "j");
	if (count_records(selected) != 1)
		return FALSE;
	int group = get_item(selected, 0)->value.number;
	LayerTree *node = g_ptr_array_index(document->lookup, group);
	if (!node->closing || node->index != group)
		return FALSE;
	Record *next = copy_array_shallow(layers), *children = create_array();
	g_ptr_array_remove_index(next->value.array, group);
	g_ptr_array_remove_index(next->value.array, node->closing_index);
	for (int index = node->closing_index; index < group - 1; index++)
		append_record(children, create_number(index));
	set_boolean(record, "di", TRUE);
	record_layer_list(document, create_string(TEXT_UNGROUP_LAYERS), next, copy_array_shallow(selected), children,
					  extended_history);
	return TRUE;
}

static Record *choose_merged_layers(Document *document, MergeKind kind, gboolean *down) {
	Record *record = document->record, *layers = get_record(record, "Z"), *selected = get_record(record, "j");
	Record *chosen = create_array();
	*down = kind == MERGE_SELECTED && count_records(selected) == 1 &&
			!is_layer_group(get_item(layers, get_item(selected, 0)->value.number));
	if (*down) {
		int index = get_item(selected, 0)->value.number;
		if (index > 0) {
			append_record(chosen, create_number(index));
			append_record(chosen, create_number(index - 1));
		}
	} else if (kind == MERGE_VISIBLE) {
		for (guint index = 0; index < count_records(layers); index++)
			if (is_layer_visible(get_item(layers, index)))
				append_record(chosen, create_number(index));
	} else
		for (guint index = 0; index < count_records(selected); index++)
			append_record(chosen, reference_record(get_item(selected, index)));
	return chosen;
}

gboolean merge_layers(Document *document, MergeKind kind, gboolean extended_history, GError **error) {
	Record *record = document->record, *layers = get_record(record, "Z");
	gboolean down;
	g_autoptr(Record) chosen = choose_merged_layers(document, kind, &down);
	if (!count_records(chosen))
		return FALSE;
	g_autoptr(GArray) members = collect_subtrees(document, chosen);
	g_array_sort(members, compare_indices);
	RasterExtent bounds = {0};
	for (guint index = 0; index < members->len; index++) {
		Record *layer = get_item(layers, g_array_index(members, int, index));
		if (is_layer_group(layer) || !g_strcmp0(get_layer_name(layer), "</Layer group>"))
			continue;
		RasterExtent extent = read_extent(get_record(layer, "V"));
		if (!empty_extent(extent))
			bounds = empty_extent(bounds) ? extent : union_region(bounds, extent);
	}
	Record *merged = create_document_layer(document, FALSE);
	int top = g_array_index(members, int, members->len - 1);
	Record *named = get_item(layers, down ? top - 1 : top);
	name_layer(merged, get_layer_name(named));
	if (down && has_record(get_record(named, "add"), "lclr"))
		set_record(get_record(merged, "add"), "lclr", copy_record(get_record(get_record(named, "add"), "lclr")));
	if (!empty_extent(bounds)) {
		g_autoptr(Record) indices = create_number_list(members);
		Record *pixels = compose_layer_region(document, indices, bounds, error);
		if (!pixels) {
			free_record(merged);
			return FALSE;
		}
		set_record(merged, "V", create_extent(bounds.x, bounds.y, bounds.width, bounds.height));
		set_record(merged, "buffer", pixels);
		trim_layer(merged);
	}
	Record *next = create_array();
	for (guint index = 0; index < count_records(layers); index++)
		if (!contains_index(members, index))
			append_record(next, reference_record(get_item(layers, index)));
	Record *above = top + 1 < (int)count_records(layers) ? get_item(layers, top + 1) : NULL;
	int position = count_records(next);
	for (guint index = 0; above && index < count_records(next); index++)
		if (get_item(next, index) == above)
			position = index;
	g_ptr_array_insert(next->value.array, position, merged);
	set_boolean(record, "di", TRUE);
	record_layer_list(document, create_merge_name(0), next, copy_array_shallow(get_record(record, "j")),
					  create_index_list(position), extended_history);
	return TRUE;
}

gboolean flatten_image(Document *document, gboolean extended_history, GError **error) {
	Record *record = document->record;
	int width = document_width(document), height = document_height(document);
	g_autoptr(Record) everything = create_extent(0, 0, width, height);
	if (!compose_raster_document(document, everything, NULL, error))
		return FALSE;
	Record *background = create_document_layer(document, FALSE),
		   *pixels = create_channels((gsize)width * height * 4, 8);
	const guint32 *composite = get_record(record, "buffer")->value.buffer.data;
	guint32 *target = pixels->value.buffer.data;
	for (gsize index = 0; index < (gsize)width * height; index++)
		target[index] = compose_normal_pixel(composite[index], 0xffffffffu, 1);
	set_record(background, "V", create_extent(0, 0, width, height));
	set_record(background, "buffer", pixels);
	name_layer(background, TEXT_BACKGROUND_NAME);
	set_number(get_record(background, "add"), "lspf", 1 << 2);
	Record *next = create_array();
	append_record(next, background);
	set_boolean(record, "di", TRUE);
	record_layer_list(document, create_merge_name(1), next, copy_array_shallow(get_record(record, "j")),
					  create_index_list(0), extended_history);
	return TRUE;
}
