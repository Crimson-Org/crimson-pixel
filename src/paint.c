#include <math.h>
#include <string.h>

#include "paint.h"
#include "pixel-selection.h"
#include "magic-wand.h"
#include "blend.h"
#include "history.h"
#include "../lang/en.h"

void reset_paint_tool(PaintTool *tool, const char *identifier) {
	cancel_paint_tool(tool);
	tool->tool = identifier;
	tool->has_last = FALSE;
}

static void copy_pixels(const Record *source, RasterExtent source_extent, Record *target, RasterExtent target_extent) {
	if (equal_region(source_extent, target_extent)) {
		memcpy(target->value.buffer.data, source->value.buffer.data,
			   MIN(source->value.buffer.length, target->value.buffer.length));
		return;
	}
	RasterExtent overlap = intersect_region(source_extent, target_extent);
	if (empty_extent(overlap))
		return;
	for (int row = 0; row < overlap.height; row++) {
		gsize from = (gsize)(overlap.y - source_extent.y + row) * source_extent.width + overlap.x - source_extent.x;
		gsize to = (gsize)(overlap.y - target_extent.y + row) * target_extent.width + overlap.x - target_extent.x;
		memcpy((guint8 *)target->value.buffer.data + to * 4, (guint8 *)source->value.buffer.data + from * 4,
			   (gsize)overlap.width * 4);
	}
}

static Record *create_region_pixels(RasterExtent extent) {
	return create_channels((gsize)MAX(0, extent.width) * MAX(0, extent.height) * 4, 8);
}

static void extend_layer(Record *layer, RasterExtent extent) {
	RasterExtent current = read_extent(get_record(layer, "V"));
	if (empty_extent(extent) || equal_region(union_region(current, extent), current))
		return;
	RasterExtent grown = union_region(current, extent);
	Record *pixels = create_region_pixels(grown);
	copy_pixels(get_record(layer, "buffer"), current, pixels, grown);
	set_record(layer, "V", create_extent(grown.x, grown.y, grown.width, grown.height));
	set_record(layer, "buffer", pixels);
}

static RasterExtent find_opaque_bounds(const Record *pixels, RasterExtent extent) {
	const guint8 *bytes = pixels->value.buffer.data;
	int top = 0, left = 0, bottom = 0, right = 0;
#define OPAQUE(column, row) (bytes[((gsize)(row) * extent.width + (column)) * 4 + 3] != 0)
	for (gboolean found = FALSE; top < extent.height && !found; top += !found)
		for (int column = 0; column < extent.width && !found; column++)
			found = OPAQUE(column, top);
	for (gboolean found = FALSE; bottom < extent.height && !found; bottom += !found)
		for (int column = 0; column < extent.width && !found; column++)
			found = OPAQUE(column, extent.height - 1 - bottom);
	for (gboolean found = FALSE; left < extent.width && !found; left += !found)
		for (int row = 0; row < extent.height && !found; row++)
			found = OPAQUE(left, row);
	for (gboolean found = FALSE; right < extent.width && !found; right += !found)
		for (int row = 0; row < extent.height && !found; row++)
			found = OPAQUE(extent.width - 1 - right, row);
#undef OPAQUE
	RasterExtent bounds = {extent.x + left, extent.y + top, extent.width - left - right, extent.height - top - bottom};
	return empty_extent(bounds) ? (RasterExtent){0} : bounds;
}

void trim_layer(Record *layer) {
	if ((guint)get_number(layer, "NF") & 16)
		return;
	RasterExtent extent = read_extent(get_record(layer, "V"));
	RasterExtent bounds = find_opaque_bounds(get_record(layer, "buffer"), extent);
	if (equal_region(bounds, extent))
		return;
	Record *pixels = create_region_pixels(bounds);
	copy_pixels(get_record(layer, "buffer"), extent, pixels, bounds);
	set_record(layer, "V", create_extent(bounds.x, bounds.y, bounds.width, bounds.height));
	set_record(layer, "buffer", pixels);
}

static void union_dirty_record(Record *owner, const char *key, RasterExtent extent) {
	g_autoptr(Record) region = create_extent(extent.x, extent.y, extent.width, extent.height);
	Record *dirty = get_record(owner, key);
	gboolean empty = !dirty || dirty->type == RECORD_NULL;
	set_record(owner, key, empty ? reference_record(region) : union_extents(dirty, region));
}

static void invalidate_painted_region(Document *document, Record *layer, RasterExtent extent) {
	if (empty_extent(extent))
		return;
	union_dirty_record(get_record(layer, "CS"), "ey", extent);
	union_dirty_record(document->record, "ey", extent);
}

static void swap_painted_regions(Record *data, Document *document) {
	Record *layers = get_record(document->record, "Z");
	for (guint index = 0; index < count_records(data); index++) {
		Record *entry = get_item(data, index), *layer = get_item(layers, get_number(entry, "z"));
		RasterExtent extent = read_extent(get_record(entry, "jp"));
		document->changed = empty_extent(document->changed) ? extent : union_region(document->changed, extent);
		Record *current = create_region_pixels(extent);
		copy_pixels(get_record(layer, "buffer"), read_extent(get_record(layer, "V")), current, extent);
		extend_layer(layer, extent);
		copy_pixels(get_record(entry, "Di"), extent, get_record(layer, "buffer"), read_extent(get_record(layer, "V")));
		trim_layer(layer);
		set_record(entry, "Di", current);
		union_dirty_record(get_record(layer, "CS"), "ey", read_extent(get_record(layer, "V")));
	}
	set_boolean(document->record, "di", TRUE);
	RasterExtent everything = {0, 0, document_width(document), document_height(document)};
	union_dirty_record(document->record, "ey", everything);
}

static double get_painted_history_size(const Record *data) {
	double size = 0;
	for (guint index = 0; index < count_records(data); index++)
		size += get_record(get_item(data, index), "Di")->value.buffer.length;
	return size;
}

/* Each painting tool names its history step with its source label in list 10 */
typedef struct {
	int label;
	HistoryOperation operation;
} PaintHistory;

static const PaintHistory PAINT_HISTORIES[] = {
	{0, {PAINT_TOOL_BRUSH, swap_painted_regions, swap_painted_regions, get_painted_history_size}},
	{38, {PAINT_TOOL_PENCIL, swap_painted_regions, swap_painted_regions, get_painted_history_size}},
	{3, {PAINT_TOOL_ERASER, swap_painted_regions, swap_painted_regions, get_painted_history_size}},
	{6, {PAINT_TOOL_GRADIENT, swap_painted_regions, swap_painted_regions, get_painted_history_size}},
	{13, {PAINT_TOOL_BUCKET, swap_painted_regions, swap_painted_regions, get_painted_history_size}},
	{52, {PAINT_TOOL_MAGIC_ERASER, swap_painted_regions, swap_painted_regions, get_painted_history_size}}};

static const PaintHistory *find_paint_history(const char *tool) {
	for (guint index = 0; index < G_N_ELEMENTS(PAINT_HISTORIES); index++)
		if (!g_strcmp0(PAINT_HISTORIES[index].operation.id, tool))
			return &PAINT_HISTORIES[index];
	return &PAINT_HISTORIES[0];
}

static gboolean is_erasing_tool(const char *tool) {
	return !g_strcmp0(tool, PAINT_TOOL_ERASER) || !g_strcmp0(tool, PAINT_TOOL_MAGIC_ERASER);
}

gboolean check_paint_target(Document *document, GError **error) {
	Record *record = document->record, *selected = get_record(record, "j");
	const char *message = NULL;
	if (count_records(get_record(record, "In")))
		message = TEXT_PAINT_TARGET_UNAVAILABLE;
	else if (count_records(selected) != 1)
		message = count_records(selected) ? TEXT_MORE_THAN_ONE_LAYER : TEXT_SELECT_LAYER_FIRST;
	else if (document->preview)
		message = TEXT_PAINT_PREVIEW;
	else if (get_number(record, "depth") != 8 || document->mode != DOCUMENT_RASTER)
		message = TEXT_PAINT_DEPTH;
	if (!message) {
		guint index = get_item(selected, 0)->value.number;
		Record *layer = get_item(get_record(record, "Z"), index), *descriptors = get_record(layer, "add");
		Record *section = get_record(descriptors, "lsct");
		double target = get_number(layer, "A8");
		if (section && section->type == RECORD_NUMBER && section->value.number != 0 && target != 1)
			message = TEXT_LAYER_NOT_EDITABLE;
		else if (target > 0)
			message = TEXT_PAINT_TARGET_UNAVAILABLE;
		else if ((guint)get_number(layer, "NF") & 16)
			message = TEXT_LAYER_NOT_EDITABLE;
		else if ((guint)get_number(descriptors, "lspf") & 2)
			message = TEXT_LAYER_LOCKED;
		for (LayerTree *node = g_ptr_array_index(document->lookup, index); !message && node->parent;
			 node = node->parent)
			if (node->parent->layer && ((guint)get_number(get_record(node->parent->layer, "add"), "lspf") >> 31 & 1))
				message = TEXT_LAYER_LOCKED;
	}
	if (!message)
		return TRUE;
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, message);
	return FALSE;
}

static Record *get_target_layer(PaintTool *tool) {
	Record *record = tool->document->record;
	return get_item(get_record(record, "Z"), get_item(get_record(record, "j"), 0)->value.number);
}

static guint32 erase_pixel(guint32 coverage, guint32 pixel, double opacity) {
	guint keep = ~(guint)round_like_source((coverage >> 24) * opacity) & 255;
	guint alpha = (pixel >> 24) * keep;
	alpha = (alpha + 1 + (alpha >> 8)) >> 8;
	return (pixel & 0x00ffffffu) | alpha << 24;
}

const GArray *get_paint_dirty_regions(PaintTool *tool) {
	if (!tool->dirty)
		tool->dirty = g_array_new(FALSE, FALSE, sizeof(RasterExtent));
	return tool->dirty;
}

void clear_paint_dirty_regions(PaintTool *tool) {
	if (tool->dirty)
		g_array_set_size(tool->dirty, 0);
}

static void add_paint_dirty_region(PaintTool *tool, RasterExtent region) {
	g_array_append_val((GArray *)get_paint_dirty_regions(tool), region);
}

static void composite_coverage(PaintTool *tool, const Record *coverage_pixels, RasterExtent coverage_extent,
							   RasterExtent dirty) {
	if (empty_extent(dirty))
		return;
	Record *layer = get_target_layer(tool), *region = create_region_pixels(dirty);
	copy_pixels(tool->backup, tool->backup_extent, region, dirty);
	gboolean alpha_locked = (guint)get_number(get_record(layer, "add"), "lspf") & 1;
	RasterExtent overlap = intersect_region(coverage_extent, dirty);
	Record *selection = get_document_selection(tool->document), *mask = NULL;
	RasterExtent mask_extent = {0};
	if (selection && !tool->coverage_masked) {
		mask = get_record(selection, "C");
		mask_extent = read_extent(get_record(selection, "V"));
		overlap = intersect_region(overlap, mask_extent);
	}
	const guint32 *coverage = coverage_pixels->value.buffer.data;
	guint32 *pixels = region->value.buffer.data;
	gboolean erasing = is_erasing_tool(tool->tool);
	for (int row = 0; row < overlap.height; row++)
		for (int column = 0; column < overlap.width; column++) {
			gsize from = (gsize)(overlap.y - coverage_extent.y + row) * coverage_extent.width + overlap.x -
						 coverage_extent.x + column;
			gsize to = (gsize)(overlap.y - dirty.y + row) * dirty.width + overlap.x - dirty.x + column;
			guint32 below = pixels[to], covered = coverage[from];
			if (mask) {
				guint value = (guint)read_channel(mask, (gsize)(overlap.y - mask_extent.y + row) * mask_extent.width +
															overlap.x - mask_extent.x + column) *
							  (covered >> 24);
				covered = (covered & 0x00ffffffu) | ((value + 1 + (value >> 8)) >> 8) << 24;
			}
			if (erasing) {
				pixels[to] = erase_pixel(covered, below, tool->opacity);
				continue;
			}
			guint32 result = compose_normal_pixel(covered, below, tool->opacity);
			pixels[to] = alpha_locked ? (result & 0x00ffffffu) | (below & 0xff000000u) : result;
		}
	copy_pixels(region, dirty, get_record(layer, "buffer"), read_extent(get_record(layer, "V")));
	free_record(region);
	invalidate_painted_region(tool->document, layer, dirty);
	add_paint_dirty_region(tool, dirty);
}

/* Each image part a stroke area extent reaches through its wrapped copies and mirror images */
static int collect_painted_parts(const PaintTool *tool, RasterExtent region, RasterExtent parts[36]) {
	int width = document_width(tool->document), height = document_height(tool->document);
	int copies = tool->wrap ? 3 : 1, count = 0;
	for (int row = 0; row < copies; row++)
		for (int column = 0; column < copies; column++) {
			RasterExtent part = intersect_region(
				(RasterExtent){region.x - column * width, region.y - row * height, region.width, region.height},
				(RasterExtent){0, 0, width, height});
			if (empty_extent(part))
				continue;
			for (int mirror = 0; mirror < 4; mirror++) {
				if ((mirror & tool->symmetry) != mirror)
					continue;
				RasterExtent mirrored = part;
				if (mirror & SYMMETRY_VERTICAL)
					mirrored.x = width - part.x - part.width;
				if (mirror & SYMMETRY_HORIZONTAL)
					mirrored.y = height - part.y - part.height;
				parts[count++] = mirrored;
			}
		}
	return count;
}

/* Each image pixel takes the strongest coverage of its copies and mirror images, so overlaps never add up */
static void merge_coverage(PaintTool *tool, RasterExtent part) {
	int width = document_width(tool->document), height = document_height(tool->document);
	int copies = tool->wrap ? 3 : 1;
	const guint32 *coverage = tool->stroke->coverage->value.buffer.data;
	guint32 *merged = tool->merged->value.buffer.data;
	gsize stride = (gsize)width * copies;
	for (int y = part.y; y < part.y + part.height; y++)
		for (int x = part.x; x < part.x + part.width; x++) {
			guint32 strongest = 0;
			for (int mirror = 0; mirror < 4; mirror++) {
				if ((mirror & tool->symmetry) != mirror)
					continue;
				int source_x = mirror & SYMMETRY_VERTICAL ? width - 1 - x : x;
				int source_y = mirror & SYMMETRY_HORIZONTAL ? height - 1 - y : y;
				for (int row = 0; row < copies; row++)
					for (int column = 0; column < copies; column++) {
						guint32 covered =
							coverage[(gsize)(source_y + row * height) * stride + source_x + column * width];
						if (covered >> 24 > strongest >> 24)
							strongest = covered;
					}
			}
			merged[(gsize)y * width + x] = strongest;
		}
}

static void composite_stroke(PaintTool *tool) {
	g_autoptr(GArray) pending = g_array_new(FALSE, FALSE, sizeof(RasterExtent));
	take_stroke_pending_regions(tool->stroke, pending);
	RasterExtent image = {0, 0, document_width(tool->document), document_height(tool->document)}, parts[36];
	for (guint region = 0; region < pending->len; region++) {
		RasterExtent changed = g_array_index(pending, RasterExtent, region);
		if (!tool->merged) {
			composite_coverage(tool, tool->stroke->coverage, tool->stroke->extent, changed);
			continue;
		}
		for (int index = 0, count = collect_painted_parts(tool, changed, parts); index < count; index++) {
			merge_coverage(tool, parts[index]);
			composite_coverage(tool, tool->merged, image, parts[index]);
		}
	}
}

/* The image area a complete stroke changed, mapped back from the stroke area */
static RasterExtent get_painted_extent(PaintTool *tool) {
	RasterExtent total = get_stroke_total_extent(tool->stroke), parts[36], painted = {0};
	if (!tool->merged)
		return total;
	int count = collect_painted_parts(tool, total, parts);
	for (int index = 0; index < count; index++)
		painted = index ? union_region(painted, parts[index]) : parts[index];
	return painted;
}

static void prepare_paint_target(PaintTool *tool, Document *document, RasterExtent region) {
	tool->document = document;
	clear_paint_dirty_regions(tool);
	Record *layer = get_target_layer(tool);
	tool->backup_extent = read_extent(get_record(layer, "V"));
	tool->backup = copy_record(get_record(layer, "buffer"));
	RasterExtent document_extent = {0, 0, document_width(document), document_height(document)};
	extend_layer(layer, union_region(union_region(document_extent, tool->backup_extent), region));
	if (!equal_region(read_extent(get_record(layer, "V")), tool->backup_extent) && document->grown_layer != layer) {
		g_clear_pointer(&document->grown_layer, free_record);
		document->grown_layer = reference_record(layer);
	}
}

static void finish_paint_target(PaintTool *tool, RasterExtent extent, gboolean extended_history, int name_group,
								int name_item) {
	Record *layer = get_target_layer(tool);
	RasterExtent layer_extent = read_extent(get_record(layer, "V"));
	gboolean keeps_growth = !g_strcmp0(tool->tool, PAINT_TOOL_BRUSH) || !g_strcmp0(tool->tool, PAINT_TOOL_PENCIL);
	if (!equal_region(layer_extent, tool->backup_extent)) {
		/* Tools other than Brush and Pencil shrink the grown layer back around the painted region */
		if (!keeps_growth) {
			RasterExtent kept = union_region(extent, tool->backup_extent);
			Record *pixels = create_region_pixels(kept);
			copy_pixels(get_record(layer, "buffer"), layer_extent, pixels, kept);
			set_record(layer, "V", create_extent(kept.x, kept.y, kept.width, kept.height));
			set_record(layer, "buffer", pixels);
		}
		invalidate_painted_region(tool->document, layer, layer_extent);
	}
	const PaintHistory *history = find_paint_history(tool->tool);
	Record *previous = create_region_pixels(extent);
	copy_pixels(tool->backup, tool->backup_extent, previous, extent);
	Record *name = create_array(), *data = create_array(), *entry = create_object(RECORD_PLAIN);
	append_record(name, create_number(name_group));
	append_record(name, create_number(name_item));
	set_number(entry, "z", get_item(get_record(tool->document->record, "j"), 0)->value.number);
	set_number(entry, "nG", 0);
	set_record(entry, "jp", create_extent(extent.x, extent.y, extent.width, extent.height));
	set_record(entry, "Di", previous);
	append_record(data, entry);
	Record *history_entry = create_history_entry(name, tool->tool, FALSE, g_get_real_time() / 1000);
	set_record(history_entry, "data", data);
	append_history_entry(tool->document, history_entry, &history->operation, extended_history);
	cancel_paint_tool(tool);
}

gboolean begin_paint_stroke(PaintTool *tool, Document *document, const PaintOptions *options, guint32 color, double x,
							double y, gboolean line_from_last, GError **error) {
	if (tool->stroke)
		return TRUE;
	if (!check_paint_target(document, error))
		return FALSE;
	tool->options = *options;
	tool->opacity = options->opacity;
	prepare_paint_target(tool, document, (RasterExtent){0});
	RasterExtent document_extent = {0, 0, document_width(document), document_height(document)};
	gboolean hard = !g_strcmp0(tool->tool, PAINT_TOOL_PENCIL) ||
					(!g_strcmp0(tool->tool, PAINT_TOOL_ERASER) && options->eraser_mode == 1);
	RasterExtent stroke_extent = document_extent;
	tool->wrap_x = tool->wrap_y = 0;
	if (tool->wrap) {
		stroke_extent = (RasterExtent){0, 0, document_extent.width * 3, document_extent.height * 3};
		tool->wrap_x = document_extent.width * (1 - floor(x / document_extent.width));
		tool->wrap_y = document_extent.height * (1 - floor(y / document_extent.height));
	}
	if (tool->wrap || tool->symmetry)
		tool->merged = create_region_pixels(document_extent);
	tool->stroke = create_stroke(options->shape, hard, options->flow, options->smoothing, color, stroke_extent,
								 tool->random, tool->random_data);
	tool->stroke->smooth_edges = options->smooth_edges;
	tool->stroke->square = options->square;
	tool->stroke->pixel_perfect = hard && options->pixel_perfect;
	double last_x = tool->last_x + tool->wrap_x, last_y = tool->last_y + tool->wrap_y;
	double stroke_x = x + tool->wrap_x, stroke_y = y + tool->wrap_y;
	if (line_from_last && tool->has_last) {
		move_stroke_to(tool->stroke, last_x, last_y, NAN);
		draw_stroke_line_to(tool->stroke, .001 * last_x + .999 * stroke_x, .001 * last_y + .999 * stroke_y, NAN);
		draw_stroke_line_to(tool->stroke, stroke_x, stroke_y, NAN);
	} else
		move_stroke_to(tool->stroke, stroke_x, stroke_y, NAN);
	tool->last_x = tool->anchor_x = x;
	tool->last_y = tool->anchor_y = y;
	tool->has_last = TRUE;
	tool->axis = 0;
	composite_stroke(tool);
	return TRUE;
}

static void constrain_point(PaintTool *tool, double *x, double *y, gboolean constrain) {
	double offset_x = *x - tool->anchor_x, offset_y = *y - tool->anchor_y;
	if (tool->axis == 0 && !(*x == tool->anchor_x && *y == tool->anchor_y))
		tool->axis = fabs(offset_x) < fabs(offset_y) ? 1 : 2;
	if (constrain && tool->axis == 1)
		offset_x = 0;
	if (constrain && tool->axis == 2)
		offset_y = 0;
	*x = tool->anchor_x + offset_x;
	*y = tool->anchor_y + offset_y;
}

void continue_paint_stroke(PaintTool *tool, double x, double y, gboolean constrain) {
	if (!tool->stroke)
		return;
	constrain_point(tool, &x, &y, constrain);
	if (x == tool->last_x && y == tool->last_y)
		return;
	draw_stroke_line_to(tool->stroke, x + tool->wrap_x, y + tool->wrap_y, NAN);
	tool->last_x = x;
	tool->last_y = y;
	composite_stroke(tool);
}

void end_paint_stroke(PaintTool *tool, gboolean extended_history) {
	if (!tool->stroke)
		return;
	finish_stroke(tool->stroke);
	composite_stroke(tool);
	finish_paint_target(tool, get_painted_extent(tool), extended_history, 10, find_paint_history(tool->tool)->label);
}

gboolean fill_paint_area(PaintTool *tool, Document *document, const FillOptions *options, guint32 color, double x,
						 double y, GError **error) {
	cancel_paint_tool(tool);
	if (!check_paint_target(document, error))
		return FALSE;
	RasterExtent extent;
	GError *sample_error = NULL;
	g_autoptr(Record) mask =
		create_similar_area(document, x, y, options->wand, options->all_layers, &extent, &sample_error);
	if (sample_error) {
		g_propagate_error(error, sample_error);
		return FALSE;
	}
	if (!mask)
		return TRUE;
	Record *selection = get_document_selection(document);
	RasterExtent kept = selection ? intersect_region(extent, read_extent(get_record(selection, "V"))) : extent;
	/* An area entirely outside the selection changes nothing, so it records no empty history step */
	if (empty_extent(kept))
		return TRUE;
	g_autoptr(Record) coverage = create_region_pixels(kept);
	guint32 *pixels = coverage->value.buffer.data;
	const guint8 *values = mask->value.buffer.data;
	for (int row = 0; row < kept.height; row++)
		for (int column = 0; column < kept.width; column++) {
			guint value = values[(gsize)(kept.y - extent.y + row) * extent.width + kept.x - extent.x + column];
			pixels[(gsize)row * kept.width + column] =
				(color >> 16 & 255) | (color & 0xff00) | (color & 255) << 16 | value << 24;
		}
	tool->opacity = options->opacity;
	prepare_paint_target(tool, document, (RasterExtent){0});
	composite_coverage(tool, coverage, kept, kept);
	finish_paint_target(tool, kept, FALSE, 10, find_paint_history(tool->tool)->label);
	return TRUE;
}

gboolean fill_paint_region(PaintTool *tool, Document *document, const Record *selection, guint32 color, int alpha,
						   gboolean preserve_transparency, int name_group, int name_item, GError **error) {
	cancel_paint_tool(tool);
	if (!check_paint_target(document, error))
		return FALSE;
	RasterExtent region = {0, 0, document_width(document), document_height(document)};
	if (selection)
		region = read_extent(get_record(selection, "V"));
	if (empty_extent(region))
		return TRUE;
	tool->opacity = 1;
	prepare_paint_target(tool, document, region);
	Record *layer = get_target_layer(tool);
	/* Preserve Transparency sets the alpha lock bit of lspf for the fill */
	gboolean alpha_locked = preserve_transparency || ((guint)get_number(get_record(layer, "add"), "lspf") & 1);
	RasterExtent layer_extent = read_extent(get_record(layer, "V"));
	guint32 *pixels = get_record(layer, "buffer")->value.buffer.data;
	const Record *mask = selection ? get_record(selection, "C") : NULL;
	guint32 source = (color >> 16 & 255) | (color & 0xff00) | (color & 255) << 16 | (guint32)alpha << 24;
	for (int row = 0; row < region.height; row++)
		for (int column = 0; column < region.width; column++) {
			guint32 covered = source;
			if (mask) {
				guint value = (double)alpha * read_channel(mask, (gsize)row * region.width + column) * (1.0 / 255);
				covered = (source & 0x00ffffffu) | value << 24;
			}
			gsize at =
				(gsize)(region.y - layer_extent.y + row) * layer_extent.width + region.x - layer_extent.x + column;
			guint32 below = pixels[at], result = compose_normal_pixel(covered, below, 1);
			pixels[at] = alpha_locked ? (result & 0x00ffffffu) | (below & 0xff000000u) : result;
		}
	invalidate_painted_region(document, layer, region);
	clear_paint_dirty_regions(tool);
	add_paint_dirty_region(tool, region);
	finish_paint_target(tool, region, FALSE, name_group, name_item);
	return TRUE;
}

gboolean draw_paint_bitmap(PaintTool *tool, Document *document, Record *pixels, RasterExtent extent, GError **error) {
	cancel_paint_tool(tool);
	if (!check_paint_target(document, error))
		return FALSE;
	Record *selection = get_document_selection(document), *mask = selection ? get_record(selection, "C") : NULL;
	guint8 *values = pixels->value.buffer.data;
	for (gsize pixel = 0; mask && pixel < (gsize)extent.width * extent.height; pixel++)
		values[pixel * 4 + 3] = (guint8)(values[pixel * 4 + 3] * read_channel(mask, pixel) * (1.0 / 255));
	tool->opacity = 1;
	tool->coverage_masked = TRUE;
	prepare_paint_target(tool, document, (RasterExtent){0});
	composite_coverage(tool, pixels, extent, extent);
	tool->coverage_masked = FALSE;
	finish_paint_target(tool, extent, FALSE, 10, find_paint_history(tool->tool)->label);
	return TRUE;
}

void cancel_paint_tool(PaintTool *tool) {
	free_stroke(tool->stroke);
	tool->stroke = NULL;
	g_clear_pointer(&tool->backup, free_record);
	g_clear_pointer(&tool->merged, free_record);
}

guint32 sample_document_color(Document *document, double x, double y, int size, gboolean active_layer) {
	Record *record = document->record, *pixels = get_record(record, "buffer");
	RasterExtent extent = {0, 0, document_width(document), document_height(document)};
	if (active_layer && count_records(get_record(record, "j"))) {
		Record *layer = get_item(get_record(record, "Z"), get_item(get_record(record, "j"), 0)->value.number);
		pixels = get_record(layer, "buffer");
		extent = read_extent(get_record(layer, "V"));
	}
	if (!pixels || pixels->type != RECORD_BUFFER)
		return 0;
	int column = (int)floor(x) - extent.x, row = (int)floor(y) - extent.y, half = (size - 1) / 2;
	int left = MAX(0, column - half), top = MAX(0, row - half);
	int right = MIN(extent.width, column + half + 1), bottom = MIN(extent.height, row + half + 1);
	double count = (double)(right - left) * (bottom - top), sums[3] = {0};
	for (int sample_x = left; sample_x < right; sample_x++)
		for (int sample_y = top; sample_y < bottom; sample_y++)
			for (int channel = 0; channel < 3; channel++)
				sums[channel] += read_channel(pixels, 4 * ((gsize)sample_y * extent.width + sample_x) + channel);
	double scale = 255 / (channel_maximum(pixels->value.buffer.depth) * count);
	if (pixels->value.buffer.depth == 32)
		scale = 255 / count;
	guint32 color = 0;
	for (int channel = 0; channel < 3; channel++) {
		double value = round_like_source(sums[channel] * scale);
		color |= (guint32)(isfinite(value) ? (gint64)value & 255 : 0) << (16 - channel * 8);
	}
	return color;
}

void prepare_document_for_save(Document *document) {
	if (document->grown_layer) {
		trim_layer(document->grown_layer);
		g_clear_pointer(&document->grown_layer, free_record);
	}
	g_autoptr(Record) everything = create_extent(0, 0, document_width(document), document_height(document));
	compose_raster_document(document, everything, NULL, NULL);
}

/* A private copy without history, which a worker may prepare and write while editing goes on */
Document *copy_document_for_save(const Document *document) {
	Document *copy = g_new0(Document, 1);
	copy->mode = document->mode;
	copy->record = create_object(document->record->class);
	for (guint index = 0; index < document->record->order->len; index++) {
		const char *key = g_ptr_array_index(document->record->order, index);
		if (strcmp(key, "history"))
			set_record(copy->record, key, copy_record(get_record(document->record, key)));
	}
	Record *layers = get_record(document->record, "Z");
	for (guint index = 0; document->grown_layer && index < count_records(layers); index++)
		if (get_item(layers, index) == document->grown_layer)
			copy->grown_layer = reference_record(get_item(get_record(copy->record, "Z"), index));
	return copy;
}
