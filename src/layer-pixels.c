#include <math.h>
#include <string.h>

#include "history.h"
#include "layer-commands.h"
#include "pixel-selection.h"
#include "../lang/en.h"

static Record *extract_selected_pixels(const Record *layer, const Record *selection, RasterExtent *result,
									   GError **error) {
	Record *buffer = get_record(layer, "buffer"), *mask = get_record(selection, "C");
	RasterExtent layer_extent = read_extent(get_record(layer, "V")),
				 mask_extent = read_extent(get_record(selection, "V"));
	RasterExtent overlap = intersect_region(layer_extent, mask_extent);
	int left = overlap.width, right = -1, top = overlap.height, bottom = -1;
	g_autofree double *alpha = g_new0(double, (gsize)MAX(0, overlap.width) * MAX(0, overlap.height));
	double full = channel_maximum(buffer->value.buffer.depth);
	for (int y = 0; y < overlap.height; y++)
		for (int x = 0; x < overlap.width; x++) {
			gsize from =
				((gsize)(overlap.y - layer_extent.y + y) * layer_extent.width + overlap.x - layer_extent.x + x) * 4;
			double covered = read_channel(mask, (gsize)(overlap.y - mask_extent.y + y) * mask_extent.width + overlap.x -
													mask_extent.x + x);
			double value = full == 1 ? covered * read_channel(buffer, from + 3)
									 : trunc(covered * read_channel(buffer, from + 3) / full);
			alpha[(gsize)y * overlap.width + x] = value;
			if (!value)
				continue;
			left = MIN(left, x);
			right = MAX(right, x);
			top = MIN(top, y);
			bottom = MAX(bottom, y);
		}
	if (right < 0) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, TEXT_COPIED_AREA_EMPTY);
		return NULL;
	}
	RasterExtent extent = {overlap.x + left, overlap.y + top, right - left + 1, bottom - top + 1};
	Record *pixels = create_channels((gsize)extent.width * extent.height * 4, buffer->value.buffer.depth);
	for (int y = 0; y < extent.height; y++)
		for (int x = 0; x < extent.width; x++) {
			gsize from =
				((gsize)(extent.y - layer_extent.y + y) * layer_extent.width + extent.x - layer_extent.x + x) * 4;
			gsize to = ((gsize)y * extent.width + x) * 4;
			for (int channel = 0; channel < 3; channel++)
				write_channel(pixels, to + channel, read_channel(buffer, from + channel));
			write_channel(pixels, to + 3,
						  alpha[(gsize)(extent.y - overlap.y + y) * overlap.width + extent.x - overlap.x + x]);
		}
	*result = extent;
	return pixels;
}

static Record *create_cleared_layer(const Record *layer, const Record *selection) {
	Record *source = copy_layer_record(layer), *cleared = get_record(source, "buffer"),
		   *mask = get_record(selection, "C");
	RasterExtent layer_extent = read_extent(get_record(layer, "V")),
				 mask_extent = read_extent(get_record(selection, "V"));
	RasterExtent overlap = intersect_region(layer_extent, mask_extent);
	double full = channel_maximum(cleared->value.buffer.depth);
	for (int y = 0; y < overlap.height; y++)
		for (int x = 0; x < overlap.width; x++) {
			gsize at =
				((gsize)(overlap.y - layer_extent.y + y) * layer_extent.width + overlap.x - layer_extent.x + x) * 4 + 3;
			double keep = full - read_channel(mask, (gsize)(overlap.y - mask_extent.y + y) * mask_extent.width +
														overlap.x - mask_extent.x + x);
			double value = read_channel(cleared, at) * keep;
			write_channel(cleared, at,
						  full == 255 ? (double)(((guint)value + 1 + ((guint)value >> 8)) >> 8) : value / full);
		}
	return source;
}

static void insert_pixel_layer(Document *document, Record *layer, Record *pixels, RasterExtent extent, int replaced,
							   Record *replacement, Record *history_name, gboolean extended_history) {
	Record *record = document->record, *layers = get_record(record, "Z"), *selected = get_record(record, "j");
	int index = count_records(selected) ? get_item(selected, count_records(selected) - 1)->value.number
										: (int)count_records(layers) - 1;
	set_record(layer, "V", create_extent(extent.x, extent.y, extent.width, extent.height));
	set_record(layer, "buffer", pixels);
	g_autofree char *name = g_strdup_printf("Layer %d", find_highest_suffix(layers, "Layer ") + 1);
	name_layer(layer, name);
	set_boolean(record, "di", TRUE);
	Record *next = copy_array_shallow(layers);
	if (replacement) {
		free_record(g_ptr_array_index(next->value.array, replaced));
		g_ptr_array_index(next->value.array, replaced) = replacement;
	}
	int position = index + 1;
	Record *anchor = get_item(layers, index);
	if (anchor && get_number(get_record(anchor, "add"), "lsct") == 1)
		position--;
	position = CLAMP(position, 0, (int)count_records(layers));
	g_ptr_array_insert(next->value.array, position, layer);
	Record *data = create_layer_list_data(document, next, copy_array_shallow(selected), create_index_list(position));
	Record *selection = get_document_selection(document);
	if (selection) {
		Record *selections = create_array();
		append_record(selections, reference_record(selection));
		append_record(selections, create_null());
		set_record(data, "Ra", selections);
	}
	commit_layer_list(document, data, history_name, extended_history);
}

static Record *get_editable_layer(Document *document) {
	Record *selected = get_record(document->record, "j");
	if (count_records(selected) != 1)
		return NULL;
	Record *layer = get_item(get_record(document->record, "Z"), get_item(selected, 0)->value.number);
	return is_layer_group(layer) || get_number(layer, "A8") > 0 ? NULL : layer;
}

gboolean copy_selection_to_layer(Document *document, gboolean cut, gboolean extended_history, GError **error) {
	Record *selection = get_document_selection(document), *layer = get_editable_layer(document);
	if (!selection || !layer) {
		if (cut)
			return FALSE;
		return duplicate_selected_layers(document, extended_history, error);
	}
	RasterExtent extent;
	Record *pixels = extract_selected_pixels(layer, selection, &extent, error);
	if (!pixels)
		return FALSE;
	int index = get_item(get_record(document->record, "j"), 0)->value.number;
	Record *copy = copy_layer_record(layer);
	set_number(get_record(copy, "add"), "lyid", next_layer_id(document));
	set_number(get_record(copy, "add"), "lspf", 0);
	double flags = get_number(copy, "NF");
	if ((guint)flags & 2)
		flags -= 2;
	if ((guint)flags & 1)
		flags -= 1;
	set_number(copy, "NF", flags);
	set_boolean(copy, "jl", FALSE);
	Record *history_name = create_history_name(6, 49);
	append_record(history_name, create_number(cut ? 1 : 0));
	insert_pixel_layer(document, copy, pixels, extent, index, cut ? create_cleared_layer(layer, selection) : NULL,
					   history_name, extended_history);
	return TRUE;
}

Record *copy_selection_pixels(Document *document, gboolean merged, RasterExtent *extent, GError **error) {
	Record *selection = get_document_selection(document);
	if (!selection)
		return NULL;
	if (merged) {
		Record *composite = get_record(document->record, "buffer");
		if (!composite || composite->type != RECORD_BUFFER)
			return NULL;
		Record *view = create_object(RECORD_PLAIN);
		set_record(view, "buffer", reference_record(composite));
		set_record(view, "V", create_extent(0, 0, document_width(document), document_height(document)));
		Record *pixels = extract_selected_pixels(view, selection, extent, error);
		free_record(view);
		return pixels;
	}
	Record *layer = get_editable_layer(document);
	if (!layer) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, TEXT_SELECT_LAYER_FIRST);
		return NULL;
	}
	return extract_selected_pixels(layer, selection, extent, error);
}

gboolean clear_selection_pixels(Document *document, gboolean extended_history, GError **error) {
	Record *selection = get_document_selection(document), *layer = get_editable_layer(document);
	if (!selection || !layer)
		return FALSE;
	Record *record = document->record, *layers = get_record(record, "Z"), *selected = get_record(record, "j");
	int index = get_item(selected, 0)->value.number;
	Record *next = copy_array_shallow(layers);
	free_record(g_ptr_array_index(next->value.array, index));
	g_ptr_array_index(next->value.array, index) = create_cleared_layer(layer, selection);
	set_boolean(record, "di", TRUE);
	record_layer_list(document, create_history_name(2, 2), next, copy_array_shallow(selected),
					  copy_array_shallow(selected), extended_history);
	return TRUE;
}

gboolean paste_pixels(Document *document, Record *pixels, RasterExtent extent, gboolean same_document,
					  gboolean extended_history, GError **error) {
	int width = document_width(document), height = document_height(document);
	if (!same_document) {
		extent.x = (width - extent.width) / 2;
		extent.y = (height - extent.height) / 2;
		if (width - extent.width < 0)
			extent.x = -((extent.width - width + 1) / 2);
		if (height - extent.height < 0)
			extent.y = -((extent.height - height + 1) / 2);
	}
	insert_pixel_layer(document, create_document_layer(document, FALSE), pixels, extent, -1, NULL,
					   create_history_name(5, 2), extended_history);
	return TRUE;
}
