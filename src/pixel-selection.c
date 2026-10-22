#include <math.h>

#include "history.h"
#include "pixel-selection.h"
#include "magic-wand.h"


static const char *SELECTION_OPERATION = "1";

static double get_full_coverage(const Document *document) {
	return channel_maximum(get_number(document->record, "depth"));
}

Record *get_document_selection(const Document *document) {
	Record *selection = get_record(document->record, "g");
	return selection && selection->type == RECORD_OBJECT ? selection : NULL;
}

RasterExtent get_selection_extent(const Document *document) {
	Record *selection = get_document_selection(document);
	return selection ? read_extent(get_record(selection, "V")) : (RasterExtent){0};
}

Record *create_selection(RasterExtent extent, Record *mask) {
	Record *selection = create_object(RECORD_PLAIN);
	set_record(selection, "C", mask);
	set_record(selection, "V", create_extent(extent.x, extent.y, extent.width, extent.height));
	return selection;
}

Record *create_selection_mask(const Document *document, RasterExtent extent) {
	return create_channels((gsize)MAX(0, extent.width) * MAX(0, extent.height), get_number(document->record, "depth"));
}

gboolean selection_contains(const Document *document, double x, double y) {
	Record *selection = get_document_selection(document);
	if (!selection)
		return FALSE;
	RasterExtent extent = read_extent(get_record(selection, "V"));
	int column = floor(x) - extent.x, row = floor(y) - extent.y;
	if (column < 0 || row < 0 || column >= extent.width || row >= extent.height)
		return FALSE;
	return read_channel(get_record(selection, "C"), (gsize)row * extent.width + column) >
		   get_full_coverage(document) / 2;
}

void copy_selection_mask(const Record *source, RasterExtent from, Record *target, RasterExtent to) {
	RasterExtent overlap = intersect_region(from, to);
	for (int y = overlap.y; y < overlap.y + overlap.height; y++)
		for (int x = overlap.x; x < overlap.x + overlap.width; x++)
			write_channel(target, (gsize)(y - to.y) * to.width + x - to.x,
						  read_channel(source, (gsize)(y - from.y) * from.width + x - from.x));
}

RasterExtent find_mask_bounds(const Record *mask, RasterExtent extent) {
	int left = extent.width, right = -1, top = extent.height, bottom = -1;
	for (int y = 0; y < extent.height; y++)
		for (int x = 0; x < extent.width; x++) {
			if (!read_channel_byte(mask, (gsize)y * extent.width + x))
				continue;
			left = MIN(left, x);
			right = MAX(right, x);
			top = MIN(top, y);
			bottom = MAX(bottom, y);
		}
	if (right < 0)
		return (RasterExtent){0};
	return (RasterExtent){extent.x + left, extent.y + top, right - left + 1, bottom - top + 1};
}

Record *trim_selection(const Document *document, Record *selection) {
	if (!selection)
		return NULL;
	RasterExtent extent = read_extent(get_record(selection, "V"));
	RasterExtent bounds = find_mask_bounds(get_record(selection, "C"), extent);
	if (empty_extent(bounds)) {
		free_record(selection);
		return NULL;
	}
	if (equal_region(bounds, extent))
		return selection;
	Record *mask = create_selection_mask(document, bounds);
	copy_selection_mask(get_record(selection, "C"), extent, mask, bounds);
	free_record(selection);
	return create_selection(bounds, mask);
}

static Record *combine_selections(const Document *document, const Record *next, const Record *previous,
								  SelectionMode mode) {
	RasterExtent next_extent = read_extent(get_record(next, "V"));
	RasterExtent previous_extent = read_extent(get_record(previous, "V"));
	RasterExtent extent = mode == SELECTION_ADD		   ? union_region(next_extent, previous_extent)
						  : mode == SELECTION_SUBTRACT ? previous_extent
													   : intersect_region(next_extent, previous_extent);
	if (empty_extent(extent))
		return NULL;
	Record *first = create_selection_mask(document, extent), *second = create_selection_mask(document, extent);
	copy_selection_mask(get_record(previous, "C"), previous_extent, first, extent);
	copy_selection_mask(get_record(next, "C"), next_extent, second, extent);
	double full = get_full_coverage(document);
	gsize count = (gsize)extent.width * extent.height;
	for (gsize index = 0; index < count; index++) {
		double added = read_channel(second, index), kept = read_channel(first, index);
		double value = mode == SELECTION_ADD		? MIN(added + kept, full)
					   : mode == SELECTION_SUBTRACT ? MAX(kept - added, 0)
													: added * kept / full;
		write_channel(first, index, value);
	}
	free_record(second);
	return trim_selection(document, create_selection(extent, first));
}

/* ============================== History ============================== */

static void swap_selection(Record *data, Document *document) {
	const char *kind = get_string(data, "S");
	if (!g_strcmp0(kind, "changesel")) {
		Record *stored = reference_record(get_record(data, "g"));
		Record *current = reference_record(get_record(document->record, "g"));
		set_record(document->record, "g", stored);
		set_record(data, "g", current);
	}
	set_boolean(document->record, "_s", TRUE);
}

static void move_selection_history(Record *data, Document *document, gboolean redo) {
	Record *selection = get_document_selection(document);
	if (!selection)
		return;
	Record *place = get_record(data, redo ? "ka" : "j6");
	set_record(selection, "V", copy_record(place));
	set_boolean(document->record, "_s", TRUE);
}

static void undo_selection(Record *data, Document *document) {
	document->selection_changed = document->selection_only = TRUE;
	if (!g_strcmp0(get_string(data, "S"), "movesel"))
		move_selection_history(data, document, FALSE);
	else
		swap_selection(data, document);
}

static void redo_selection(Record *data, Document *document) {
	document->selection_changed = document->selection_only = TRUE;
	if (!g_strcmp0(get_string(data, "S"), "movesel"))
		move_selection_history(data, document, TRUE);
	else
		swap_selection(data, document);
}

static double get_selection_history_size(const Record *data) {
	Record *selection = get_record(data, "g");
	return selection && selection->type == RECORD_OBJECT ? get_record(selection, "C")->value.buffer.length : 0;
}

static const HistoryOperation selection_operation = {"1", undo_selection, redo_selection, get_selection_history_size};

gboolean record_named_selection(Document *document, Record *selection, Record *name, gboolean extended) {
	Record *previous = get_document_selection(document);
	if (!selection && !previous) {
		free_record(name);
		return FALSE;
	}
	if (!selection) {
		free_record(name);
		name = create_history_name(7, 1);
	}
	Record *data = create_object(RECORD_PLAIN);
	set_string(data, "S", "changesel");
	set_record(data, "g", selection ? selection : create_null());
	Record *entry = create_history_entry(name, SELECTION_OPERATION, FALSE, g_get_real_time() / 1000);
	set_record(entry, "data", data);
	append_history_entry(document, entry, &selection_operation, extended);
	redo_selection(data, document);
	return TRUE;
}

gboolean record_selection(Document *document, Record *selection, int group, int item, gboolean extended) {
	return record_named_selection(document, selection, create_history_name(group, item), extended);
}

/* ============================== Commands ============================== */

static Record *create_filled_selection(const Document *document, RasterExtent extent) {
	Record *mask = create_selection_mask(document, extent);
	double full = get_full_coverage(document);
	for (gsize index = 0; index < (gsize)extent.width * extent.height; index++)
		write_channel(mask, index, full);
	return create_selection(extent, mask);
}

gboolean select_rectangle(Document *document, RasterExtent rectangle, SelectionMode mode, gboolean extended_history) {
	RasterExtent canvas = {0, 0, document_width(document), document_height(document)};
	rectangle = intersect_region(rectangle, canvas);
	Record *previous = get_document_selection(document);
	Record *selection = empty_extent(rectangle) ? NULL : create_filled_selection(document, rectangle);
	if (selection && mode != SELECTION_REPLACE && previous) {
		Record *combined = combine_selections(document, selection, previous, mode);
		free_record(selection);
		selection = combined;
	} else if (!selection && mode != SELECTION_REPLACE && previous)
		return FALSE;
	return record_selection(document, selection, 10, 15, extended_history);
}

gboolean select_path_area(Document *document, const SelectionPath *path, SelectionMode mode, gboolean anti_alias,
						  int group, int item, gboolean extended_history) {
	RasterExtent bounds;
	Record *mask = rasterize_selection_path(path, get_number(document->record, "depth"), anti_alias, &bounds);
	Record *selection = mask ? create_selection(bounds, mask) : NULL;
	Record *previous = get_document_selection(document);
	if (selection && mode != SELECTION_REPLACE && previous) {
		Record *combined = combine_selections(document, selection, previous, mode);
		free_record(selection);
		selection = combined;
	} else
		selection = trim_selection(document, selection);
	return record_selection(document, selection, group, item, extended_history);
}

gboolean select_all(Document *document, gboolean extended_history) {
	RasterExtent canvas = {0, 0, document_width(document), document_height(document)};
	if (empty_extent(canvas))
		return FALSE;
	return record_selection(document, create_filled_selection(document, canvas), 7, 0, extended_history);
}

gboolean deselect(Document *document, gboolean extended_history) {
	return record_selection(document, NULL, 7, 1, extended_history);
}

gboolean invert_selection(Document *document, gboolean extended_history) {
	Record *previous = get_document_selection(document);
	if (!previous)
		return FALSE;
	RasterExtent canvas = {0, 0, document_width(document), document_height(document)};
	Record *mask = create_selection_mask(document, canvas);
	copy_selection_mask(get_record(previous, "C"), read_extent(get_record(previous, "V")), mask, canvas);
	double full = get_full_coverage(document);
	for (gsize index = 0; index < (gsize)canvas.width * canvas.height; index++)
		write_channel(mask, index, full - read_channel(mask, index));
	Record *selection = trim_selection(document, create_selection(canvas, mask));
	return record_selection(document, selection, 7, 2, extended_history);
}

void move_selection(Document *document, int x, int y, gboolean extended_history) {
	Record *selection = get_document_selection(document);
	if (!selection)
		return;
	RasterExtent extent = read_extent(get_record(selection, "V"));
	Record *latest = get_latest_history_entry(document), *data = latest ? get_record(latest, "data") : NULL;
	if (!data || data->type != RECORD_OBJECT || g_strcmp0(get_string(data, "S"), "movesel")) {
		data = create_object(RECORD_PLAIN);
		set_string(data, "S", "movesel");
		set_record(data, "j6", create_extent(extent.x, extent.y, extent.width, extent.height));
		Record *entry =
			create_history_entry(create_history_name(7, 7), SELECTION_OPERATION, FALSE, g_get_real_time() / 1000);
		set_record(entry, "data", data);
		append_history_entry(document, entry, &selection_operation, extended_history);
	}
	set_record(data, "ka", create_extent(x, y, extent.width, extent.height));
	redo_selection(data, document);
}

RasterExtent constrain_rectangle_drag(double start_x, double start_y, double end_x, double end_y, double ratio,
									  gboolean centered) {
	double left = start_x, top = start_y, right = end_x, bottom = end_y;
	if (ratio != 0) {
		left = left < right ? floor(left) : ceil(left);
		top = top < bottom ? floor(top) : ceil(top);
		double width = fabs(right - left), height = fabs(bottom - top), side = width;
		if (width > 0 && height / width < ratio)
			side *= height / width / ratio;
		right = right > left ? left + side : left - side;
		bottom = bottom > top ? top + side * ratio : top - side * ratio;
	}
	if (centered) {
		left -= right - left;
		top -= bottom - top;
	}
	double x0 = floor(MIN(left, right)), y0 = floor(MIN(top, bottom));
	double x1 = ceil(MAX(left, right)), y1 = ceil(MAX(top, bottom));
	return (RasterExtent){x0, y0, x1 - x0, y1 - y0};
}

RasterExtent constrain_selection_drag(double start_x, double start_y, double end_x, double end_y, gboolean square,
									  gboolean centered) {
	return constrain_rectangle_drag(start_x, start_y, end_x, end_y, square ? 1 : 0, centered);
}

gboolean select_similar_area(Document *document, double x, double y, WandSettings settings, gboolean all_layers,
							 SelectionMode mode, gboolean extended_history, GError **error) {
	RasterExtent bounds;
	GError *sample_error = NULL;
	Record *mask = create_similar_area(document, floor(x), floor(y), settings, all_layers, &bounds, &sample_error);
	if (sample_error) {
		g_propagate_error(error, sample_error);
		return FALSE;
	}
	Record *selection = mask ? create_selection(bounds, mask) : NULL;
	Record *previous = get_document_selection(document);
	if (selection && mode != SELECTION_REPLACE && previous) {
		Record *combined = combine_selections(document, selection, previous, mode);
		free_record(selection);
		selection = combined;
	}
	return record_selection(document, selection, 10, 12, extended_history);
}

static Record *create_alpha_selection(const Document *document, const Record *layer) {
	RasterExtent extent = read_extent(get_record(layer, "V"));
	Record *pixels = get_record(layer, "buffer");
	if (empty_extent(extent) || !pixels || pixels->type != RECORD_BUFFER)
		return NULL;
	Record *mask = create_selection_mask(document, extent);
	double scale = get_full_coverage(document) / channel_maximum(pixels->value.buffer.depth);
	for (gsize index = 0; index < (gsize)extent.width * extent.height; index++)
		write_channel(mask, index, read_channel(pixels, index * 4 + 3) * scale);
	return create_selection(extent, mask);
}

gboolean select_layer_pixels(Document *document, int layer_index, gboolean extended_history) {
	Record *layers = get_record(document->record, "Z");
	if (layer_index < 0 || layer_index >= (int)count_records(layers))
		return FALSE;
	LayerTree *node = g_ptr_array_index(document->lookup, layer_index);
	Record *selection = NULL;
	if (!node->children)
		selection = create_alpha_selection(document, get_item(layers, layer_index));
	else
		for (guint child = 0; child < node->children->len; child++) {
			LayerTree *item = g_ptr_array_index(node->children, child);
			Record *part = item->children ? NULL : create_alpha_selection(document, item->layer);
			if (!part)
				continue;
			if (!selection)
				selection = part;
			else {
				Record *joined = combine_selections(document, part, selection, SELECTION_ADD);
				free_record(part);
				free_record(selection);
				selection = joined;
			}
		}
	return record_selection(document, trim_selection(document, selection), 14, 11, extended_history);
}
