#include <math.h>
#include <string.h>

#include "blend.h"
#include "history.h"
#include "layer-commands.h"
#include "move-tool.h"
#include "pixel-selection.h"
#include "../lang/en.h"

static const char *MOVE_OPERATION = "0";

static void mark_moved(Document *document, Record *layer, RasterExtent extent) {
	if (empty_extent(extent))
		return;
	g_autoptr(Record) region = create_extent(extent.x, extent.y, extent.width, extent.height);
	Record *owners[] = {get_record(layer, "CS"), document->record};
	for (guint index = 0; index < G_N_ELEMENTS(owners); index++) {
		Record *dirty = get_record(owners[index], "ey");
		gboolean empty = !dirty || dirty->type == RECORD_NULL;
		set_record(owners[index], "ey", empty ? reference_record(region) : union_extents(dirty, region));
	}
	document->changed = empty_extent(document->changed) ? extent : union_region(document->changed, extent);
}

static gboolean is_pixel_layer(Record *layer) {
	return !is_layer_group(layer) && get_number(get_record(layer, "add"), "lsct") == 0;
}

/* ============================== Picking layers ============================== */

int find_layer_at(Document *document, double x, double y) {
	Record *layers = get_record(document->record, "Z");
	int column = floor(x), row = floor(y);
	for (int index = (int)count_records(layers) - 1; index >= 0; index--) {
		Record *layer = get_item(layers, index);
		if (!is_pixel_layer(layer) || !is_layer_visible(layer))
			continue;
		gboolean hidden = FALSE;
		for (LayerTree *node = g_ptr_array_index(document->lookup, index); node->parent; node = node->parent)
			if (node->parent->layer && !is_layer_visible(node->parent->layer))
				hidden = TRUE;
		if (hidden)
			continue;
		RasterExtent extent = read_extent(get_record(layer, "V"));
		if (column < extent.x || row < extent.y || column >= extent.x + extent.width || row >= extent.y + extent.height)
			continue;
		if (read_channel(get_record(layer, "buffer"),
						 ((gsize)(row - extent.y) * extent.width + column - extent.x) * 4 + 3))
			return index;
	}
	return -1;
}

static void set_selected_layers(Document *document, GArray *indices) {
	Record *selected = create_array();
	for (guint index = 0; index < indices->len; index++)
		append_record(selected, create_number(g_array_index(indices, int, index)));
	set_record(document->record, "j", selected);
	set_boolean(document->record, "di", TRUE);
}

void pick_layer_at(Document *document, double x, double y, gboolean toggle, gboolean whole_group) {
	int found = find_layer_at(document, x, y);
	if (found < 0)
		return;
	LayerTree *node = g_ptr_array_index(document->lookup, found);
	while (whole_group && node->parent && node->parent->depth != 0 &&
		   !has_record(get_record(node->parent->layer, "add"), "artb"))
		node = node->parent;
	found = node->index;
	Record *selected = get_record(document->record, "j");
	g_autoptr(GArray) indices = g_array_new(FALSE, FALSE, sizeof(int));
	int position = -1;
	for (guint index = 0; index < count_records(selected); index++) {
		int value = get_item(selected, index)->value.number;
		if (value == found)
			position = index;
		g_array_append_val(indices, value);
	}
	if (!toggle) {
		if (position >= 0)
			return;
		g_array_set_size(indices, 0);
		g_array_append_val(indices, found);
	} else if (position < 0)
		g_array_append_val(indices, found);
	else if (indices->len > 1)
		g_array_remove_index(indices, position);
	set_selected_layers(document, indices);
}

void select_layers_in_rectangle(Document *document, RasterExtent rectangle) {
	Record *layers = get_record(document->record, "Z");
	g_autoptr(GArray) indices = g_array_new(FALSE, FALSE, sizeof(int));
	for (guint index = 0; index < count_records(layers); index++) {
		Record *layer = get_item(layers, index);
		if (!is_pixel_layer(layer) || !is_layer_visible(layer))
			continue;
		if (!empty_extent(intersect_region(read_extent(get_record(layer, "V")), rectangle)))
			g_array_append_val(indices, index);
	}
	set_selected_layers(document, indices);
}

/* ============================== Moving layers ============================== */

static void offset_layers(Document *document, GArray *indices, int x, int y) {
	Record *layers = get_record(document->record, "Z");
	for (guint index = 0; index < indices->len; index++) {
		Record *layer = get_item(layers, g_array_index(indices, int, index));
		if (!is_pixel_layer(layer))
			continue;
		RasterExtent extent = read_extent(get_record(layer, "V"));
		mark_moved(document, layer, extent);
		extent.x += x;
		extent.y += y;
		set_record(layer, "V", create_extent(extent.x, extent.y, extent.width, extent.height));
		mark_moved(document, layer, extent);
	}
}

static GArray *read_indices(const Record *list) {
	GArray *indices = g_array_new(FALSE, FALSE, sizeof(int));
	for (guint index = 0; index < count_records(list); index++) {
		int value = get_item(list, index)->value.number;
		g_array_append_val(indices, value);
	}
	return indices;
}

/* ============================== Floating pixels ============================== */

static Record *read_offset(const Record *data) {
	return get_record(data, "kv");
}

static RasterExtent get_floating_placement(Record *data) {
	Record *offset = read_offset(data);
	RasterExtent placement = read_extent(get_record(data, "xX"));
	placement.x += get_number(offset, "x");
	placement.y += get_number(offset, "y");
	return placement;
}

/* Without the previously composed placement the whole old and new layer extents are redrawn */
void compose_floating(Document *document, Record *data, const RasterExtent *previous) {
	Record *layer = get_item(get_record(document->record, "Z"), get_number(data, "z"));
	Record *offset = read_offset(data);
	int moved_x = get_number(offset, "x"), moved_y = get_number(offset, "y");
	RasterExtent base_extent = read_extent(get_record(data, "DM")), floating = get_floating_placement(data);
	RasterExtent extent = empty_extent(base_extent) ? floating : union_region(base_extent, floating);
	Record *pixels = create_channels((gsize)extent.width * extent.height * 4, 8);
	guint32 *target = pixels->value.buffer.data;
	const guint32 *base = get_record(data, "Q6")->value.buffer.data,
				  *lifted = get_record(data, "Sl")->value.buffer.data;
	for (int y = 0; y < base_extent.height; y++)
		memcpy(target + (gsize)(base_extent.y - extent.y + y) * extent.width + base_extent.x - extent.x,
			   base + (gsize)y * base_extent.width, (gsize)base_extent.width * 4);
	for (int y = 0; y < floating.height; y++)
		for (int x = 0; x < floating.width; x++) {
			guint32 *pixel = target + (gsize)(floating.y - extent.y + y) * extent.width + floating.x - extent.x + x;
			*pixel = compose_normal_pixel(lifted[(gsize)y * floating.width + x], *pixel, 1);
		}
	if (!previous)
		mark_moved(document, layer, read_extent(get_record(layer, "V")));
	set_record(layer, "V", create_extent(extent.x, extent.y, extent.width, extent.height));
	set_record(layer, "buffer", pixels);
	/* Only the lifted area, the earlier placement and the new one differ from what was last composed */
	if (!previous)
		mark_moved(document, layer, extent);
	else {
		RasterExtent changed = union_region(read_extent(get_record(data, "xX")), floating);
		mark_moved(document, layer, empty_extent(*previous) ? changed : union_region(changed, *previous));
	}
	/* The selection shares the floating mask, so moving it never rebuilds the outline */
	Record *floating_selection = get_record(data, "xO"), *selection = create_object(RECORD_PLAIN);
	RasterExtent mask = read_extent(get_record(floating_selection, "V"));
	set_record(selection, "C", reference_record(get_record(floating_selection, "C")));
	set_record(selection, "V", create_extent(mask.x + moved_x, mask.y + moved_y, mask.width, mask.height));
	set_record(document->record, "g", selection);
	document->selection_changed = TRUE;
}

static void restore_floating(Document *document, Record *data) {
	Record *layer = get_item(get_record(document->record, "Z"), get_number(data, "z"));
	mark_moved(document, layer, read_extent(get_record(layer, "V")));
	set_record(layer, "V", copy_record(get_record(data, "a9p")));
	set_record(layer, "buffer", copy_record(get_record(data, "at1")));
	mark_moved(document, layer, read_extent(get_record(layer, "V")));
	set_record(document->record, "g", reference_record(get_record(data, "DN")));
	document->selection_changed = TRUE;
}

Record *lift_pixels(Document *document, Record *layer, gboolean copy, GError **error) {
	Record *selection = get_document_selection(document), *buffer = get_record(layer, "buffer");
	Record *mask = get_record(selection, "C");
	RasterExtent layer_extent = read_extent(get_record(layer, "V")),
				 mask_extent = read_extent(get_record(selection, "V"));
	RasterExtent overlap = intersect_region(layer_extent, mask_extent);
	int left = overlap.width, right = -1, top = overlap.height, bottom = -1;
	g_autofree guint8 *alpha = g_malloc0((gsize)MAX(0, overlap.width) * MAX(0, overlap.height));
	for (int y = 0; y < overlap.height; y++)
		for (int x = 0; x < overlap.width; x++) {
			gsize at =
				((gsize)(overlap.y - layer_extent.y + y) * layer_extent.width + overlap.x - layer_extent.x + x) * 4 + 3;
			double covered = read_channel(mask, (gsize)(overlap.y - mask_extent.y + y) * mask_extent.width + overlap.x -
													mask_extent.x + x);
			guint8 value = (guint8)trunc(covered * read_channel(buffer, at) / 255);
			alpha[(gsize)y * overlap.width + x] = value;
			if (!value)
				continue;
			left = MIN(left, x);
			right = MAX(right, x);
			top = MIN(top, y);
			bottom = MAX(bottom, y);
		}
	if (right < 0) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, TEXT_SELECTED_AREA_EMPTY);
		return NULL;
	}
	RasterExtent extent = {overlap.x + left, overlap.y + top, right - left + 1, bottom - top + 1};
	Record *lifted = create_channels((gsize)extent.width * extent.height * 4, 8);
	Record *lifted_mask = create_channels((gsize)extent.width * extent.height, 8);
	for (int y = 0; y < extent.height; y++)
		for (int x = 0; x < extent.width; x++) {
			gsize from =
				((gsize)(extent.y - layer_extent.y + y) * layer_extent.width + extent.x - layer_extent.x + x) * 4;
			gsize to = (gsize)y * extent.width + x;
			guint8 value = alpha[(gsize)(extent.y - overlap.y + y) * overlap.width + extent.x - overlap.x + x];
			memcpy((guint8 *)lifted->value.buffer.data + to * 4, (guint8 *)buffer->value.buffer.data + from, 3);
			((guint8 *)lifted->value.buffer.data)[to * 4 + 3] = value;
			((guint8 *)lifted_mask->value.buffer.data)[to] = value;
		}
	Record *base = copy_record(buffer);
	if (!copy)
		for (int y = 0; y < overlap.height; y++)
			for (int x = 0; x < overlap.width; x++) {
				gsize at =
					((gsize)(overlap.y - layer_extent.y + y) * layer_extent.width + overlap.x - layer_extent.x + x) *
						4 +
					3;
				guint keep =
					255 - (guint)read_channel(mask, (gsize)(overlap.y - mask_extent.y + y) * mask_extent.width +
														overlap.x - mask_extent.x + x);
				guint value = ((guint8 *)base->value.buffer.data)[at] * keep;
				((guint8 *)base->value.buffer.data)[at] = (value + 1 + (value >> 8)) >> 8;
			}
	Record *data = create_object(RECORD_PLAIN);
	set_number(data, "S", 1);
	set_record(data, "Sl", lifted);
	set_record(data, "xX", create_extent(extent.x, extent.y, extent.width, extent.height));
	set_record(data, "Q6", base);
	set_record(data, "DM", create_extent(layer_extent.x, layer_extent.y, layer_extent.width, layer_extent.height));
	/* Composing the floating pixels gives the layer a new buffer, so the original one is kept unchanged */
	set_record(data, "at1", reference_record(buffer));
	set_record(data, "a9p", create_extent(layer_extent.x, layer_extent.y, layer_extent.width, layer_extent.height));
	set_record(data, "DN", reference_record(get_record(document->record, "g")));
	Record *selection_after = create_object(RECORD_PLAIN);
	set_record(selection_after, "C", lifted_mask);
	set_record(selection_after, "V", create_extent(extent.x, extent.y, extent.width, extent.height));
	set_record(data, "xO", selection_after);
	Record *offset = create_object(RECORD_PLAIN);
	set_number(offset, "x", 0);
	set_number(offset, "y", 0);
	set_record(data, "kv", offset);
	return data;
}

/* ============================== History ============================== */

static void apply_move(Record *data, Document *document, gboolean redo) {
	if (get_number(data, "S") == 1) {
		if (redo)
			compose_floating(document, data, NULL);
		else
			restore_floating(document, data);
	} else {
		g_autoptr(GArray) indices = read_indices(get_record(data, "j"));
		int sign = redo ? 1 : -1;
		offset_layers(document, indices, sign * get_number(data, "x"), sign * get_number(data, "y"));
	}
	set_boolean(document->record, "di", TRUE);
}

static void undo_move(Record *data, Document *document) {
	apply_move(data, document, FALSE);
}

static void redo_move(Record *data, Document *document) {
	apply_move(data, document, TRUE);
}

static double get_move_history_size(const Record *data) {
	if (get_number(data, "S") != 1)
		return 0;
	return get_record(data, "Sl")->value.buffer.length + get_record(data, "Q6")->value.buffer.length +
		   get_record(data, "at1")->value.buffer.length;
}

static const HistoryOperation move_operation = {"0", undo_move, redo_move, get_move_history_size};

static Record *get_latest_move(Document *document, int mode) {
	Record *latest = get_latest_history_entry(document), *data = latest ? get_record(latest, "data") : NULL;
	if (!latest || g_strcmp0(get_string(get_record(latest, "U"), "id"), MOVE_OPERATION) || !data ||
		data->type != RECORD_OBJECT)
		return NULL;
	return get_number(data, "S") == mode ? data : NULL;
}

void append_move(Document *document, Record *data, int group, int item, gboolean extended_history) {
	Record *entry =
		create_history_entry(create_history_name(group, item), MOVE_OPERATION, FALSE, g_get_real_time() / 1000);
	set_record(entry, "data", data);
	append_history_entry(document, entry, &move_operation, extended_history);
}

/* ============================== Dragging ============================== */

gboolean begin_move(MoveDrag *drag, Document *document, gboolean inside_selection, gboolean copy,
					gboolean extended_history, GError **error) {
	clear_move(drag);
	drag->layers = collect_selected_subtrees(document);
	Record *layers = get_record(document->record, "Z");
	for (guint index = 0; index < drag->layers->len; index++) {
		Record *layer = get_item(layers, g_array_index(drag->layers, int, index));
		guint locks = (guint)get_number(get_record(layer, "add"), "lspf");
		if (locks & 4 || locks >> 31) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, TEXT_LAYER_LOCKED);
			clear_move(drag);
			return FALSE;
		}
	}
	Record *selected = get_record(document->record, "j");
	drag->mode = 0;
	if (!inside_selection || count_records(selected) != 1 || get_number(document->record, "depth") != 8)
		return TRUE;
	int index = get_item(selected, 0)->value.number;
	Record *layer = get_item(layers, index);
	if (!is_pixel_layer(layer) || get_number(layer, "A8") > 0)
		return TRUE;
	drag->mode = 1;
	Record *latest = get_latest_move(document, 1);
	if (latest && get_number(latest, "z") == index && !copy) {
		drag->floating = reference_record(latest);
		drag->composed = get_floating_placement(latest);
		return TRUE;
	}
	Record *data = lift_pixels(document, layer, copy, error);
	if (!data) {
		clear_move(drag);
		return FALSE;
	}
	set_number(data, "z", index);
	append_move(document, data, 12, copy ? 53 : 54, extended_history);
	drag->floating = reference_record(data);
	compose_floating(document, data, &(RasterExtent){0});
	drag->composed = get_floating_placement(data);
	return TRUE;
}

/* The drag offset is absolute from the press, so each update applies only the change since the last one */
void update_move(MoveDrag *drag, Document *document, int x, int y) {
	if (!drag->layers)
		return;
	int change_x = x - drag->moved_x, change_y = y - drag->moved_y;
	if (!change_x && !change_y)
		return;
	drag->moved_x = x;
	drag->moved_y = y;
	if (drag->mode == 1) {
		Record *offset = read_offset(drag->floating);
		set_number(offset, "x", get_number(offset, "x") + change_x);
		set_number(offset, "y", get_number(offset, "y") + change_y);
		if (!drag->deferred) {
			compose_floating(document, drag->floating, &drag->composed);
			drag->composed = get_floating_placement(drag->floating);
		} else {
			Record *selection = get_document_selection(document);
			RasterExtent extent = selection ? read_extent(get_record(selection, "V")) : (RasterExtent){0};
			if (selection)
				set_record(selection, "V",
						   create_extent(extent.x + change_x, extent.y + change_y, extent.width, extent.height));
		}
	} else
		offset_layers(document, drag->layers, change_x, change_y);
	set_boolean(document->record, "di", TRUE);
}

gboolean finish_move(MoveDrag *drag, Document *document, gboolean extended_history) {
	if (!drag->layers)
		return FALSE;
	gboolean moved = drag->moved_x || drag->moved_y;
	if (drag->mode == 1 && drag->deferred && moved)
		compose_floating(document, drag->floating, &drag->composed);
	if (drag->mode == 0 && moved) {
		Record *latest = get_latest_move(document, 0);
		g_autoptr(GArray) previous = latest ? read_indices(get_record(latest, "j")) : NULL;
		gboolean same = previous && previous->len == drag->layers->len &&
						!memcmp(previous->data, drag->layers->data, previous->len * sizeof(int));
		if (same) {
			set_number(latest, "x", get_number(latest, "x") + drag->moved_x);
			set_number(latest, "y", get_number(latest, "y") + drag->moved_y);
		} else {
			Record *data = create_object(RECORD_PLAIN), *indices = create_array();
			set_number(data, "S", 0);
			for (guint index = 0; index < drag->layers->len; index++)
				append_record(indices, create_number(g_array_index(drag->layers, int, index)));
			set_record(data, "j", indices);
			set_number(data, "x", drag->moved_x);
			set_number(data, "y", drag->moved_y);
			append_move(document, data, 12, 54, extended_history);
		}
	}
	clear_move(drag);
	return moved;
}

void clear_move(MoveDrag *drag) {
	g_clear_pointer(&drag->layers, g_array_unref);
	g_clear_pointer(&drag->floating, free_record);
	drag->moved_x = drag->moved_y = 0;
	drag->mode = 0;
	drag->deferred = FALSE;
}
