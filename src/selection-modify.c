#include <math.h>
#include <string.h>

#include "selection-modify.h"
#include "pixel-selection.h"

static const int MODIFY_NAMES[][3] = {{7, 9, -1}, {19, 3, 0}, {7, 4, -1}, {7, 5, -1}, {7, 6, -1}};

/* ============================== Source arithmetic ============================== */

/* Math.max and Math.min return NaN when either side is NaN */
static double source_max(double first, double second) {
	return isnan(first) || isnan(second) ? NAN : MAX(first, second);
}

static double source_min(double first, double second) {
	return isnan(first) || isnan(second) ? NAN : MIN(first, second);
}

static RasterExtent grow_extent(RasterExtent extent, int amount) {
	return (RasterExtent){extent.x - amount, extent.y - amount, extent.width + 2 * amount, extent.height + 2 * amount};
}

static gsize count_extent(RasterExtent extent) {
	return (gsize)MAX(0, extent.width) * MAX(0, extent.height);
}

static Record *convert_mask_depth(const Record *mask, unsigned depth) {
	gsize length = mask->value.buffer.length;
	unsigned from = mask->value.buffer.depth;
	Record *converted = create_channels(length, depth);
	if (from == depth) {
		memcpy(converted->value.buffer.data, mask->value.buffer.data, length * (depth / 8));
		return converted;
	}
	for (gsize index = 0; index < length; index++)
		write_channel(converted, index, convert_channel_depth(read_channel(mask, index), from, depth));
	return converted;
}

static void invert_mask(Record *mask) {
	unsigned depth = mask->value.buffer.depth;
	double full = channel_maximum(depth);
	for (gsize index = 0; index < mask->value.buffer.length; index++)
		write_channel(mask, index, full - read_channel(mask, index));
}

/* ============================== Distance coverage ============================== */

static float *measure_edge_distances(const guint8 *mask, int width, int height) {
	gsize count = (gsize)width * height;
	/* The column pass stores wrapped 16 bit distances and coverages */
	guint16 *nearest = g_new(guint16, count * 2);
	gint32 *vertices = g_new(gint32, width), *boundaries = g_new(gint32, width);
	gint32 *squares = g_new(gint32, width), *coverages = g_new(gint32, width);
	float *distances = g_new(float, count);
	for (gint64 column = 0; column < width; column++) {
		double source_row = -height - width, coverage = 0;
		gint64 index = column;
		for (gint64 row = 0; row < height; row++, index += width) {
			if (mask[index] > 128) {
				source_row = row;
				coverage = mask[index];
			}
			nearest[index * 2] = (guint16)(gint64)(row - source_row);
			nearest[index * 2 + 1] = coverage;
		}
		gint64 slot = 2 * (column + (gint64)(height - 1) * width);
		double distance = nearest[slot];
		coverage = nearest[slot + 1];
		for (gint64 remaining = height; remaining > 1; remaining--) {
			slot -= 2 * (gint64)width;
			if (nearest[slot] > distance) {
				nearest[slot] = (guint16)(gint64)++distance;
				nearest[slot + 1] = coverage;
				continue;
			}
			distance = nearest[slot];
			coverage = nearest[slot + 1];
		}
	}
	for (gint64 start = 0; start < (gint64)count; start += width) {
		gint64 end = start + width, vertex = start, boundary = start;
		int top = 0;
		vertices[0] = boundaries[0] = start;
		double vertex_square = (double)nearest[2 * start] * nearest[2 * start], boundary_value = vertex_square;
		for (gint64 point = start + 1; point < end; point++) {
			double square = (double)nearest[2 * point] * nearest[2 * point];
			while (top && boundary_value > (double)(boundary - point) * (boundary - point) + square) {
				boundary = boundaries[--top];
				vertex = vertices[top];
				vertex_square = (double)nearest[2 * vertex] * nearest[2 * vertex];
				boundary_value = (double)(boundary - vertex) * (boundary - vertex) + vertex_square;
			}
			if (top || boundary_value <= (double)(boundary - point) * (boundary - point) + square) {
				double span = point - vertex;
				double crossing = start +
								  floor((span * (point + vertex - start - start) + square - vertex_square) /
										(double)((gint64)span << 1)) +
								  1;
				if (crossing >= end)
					continue;
				boundary = boundaries[++top] = crossing;
				vertex = vertices[top] = point;
			} else
				boundary = start, vertex = vertices[0] = point;
			vertex_square = square;
			boundary_value = (double)(boundary - vertex) * (boundary - vertex) + vertex_square;
		}
		for (int index = 0; index < top; index++) {
			gint64 slot = (gint64)vertices[index] << 1;
			squares[index] = (gint32)(guint32)((double)nearest[slot] * nearest[slot]);
			coverages[index] = nearest[slot + 1];
		}
		double offset = .5 - nearest[((gint64)vertices[top] << 1) + 1] * (1.0 / 255);
		for (gint64 point = end - 1; point >= start; point--) {
			distances[point] =
				source_max(0, sqrt((double)(point - vertex) * (point - vertex) + vertex_square) + offset);
			if (!top || point != boundary)
				continue;
			boundary = boundaries[--top];
			vertex = vertices[top];
			vertex_square = squares[top];
			offset = .5 - coverages[top] * (1.0 / 255);
		}
	}
	g_free(nearest);
	g_free(vertices);
	g_free(boundaries);
	g_free(squares);
	g_free(coverages);
	return distances;
}

static void draw_distance_coverage(const Record *source, Record *target, RasterExtent extent, double radius) {
	float *distances = measure_edge_distances(source->value.buffer.data, extent.width, extent.height);
	guint8 *values = target->value.buffer.data;
	for (gsize index = 0; index < count_extent(extent); index++) {
		double value = round_like_source(source_max(0, source_min(1, radius + .5 - distances[index])) * 255);
		values[index] = isnan(value) ? 0 : value;
	}
	g_free(distances);
}

/* ============================== Box blur ============================== */

static gsize pad_like_source(gsize count, unsigned depth) {
	if (depth == 8)
		return (count + 3) / 4 * 4;
	return depth == 16 ? count + (count & 1) : count;
}

static double read_source_array(const Record *array, gint64 index) {
	if (index < 0 || (guint64)index >= array->value.buffer.length)
		return NAN;
	return read_channel(array, index);
}

static void write_source_array(Record *array, gint64 index, double value) {
	if (index < 0 || (guint64)index >= array->value.buffer.length)
		return;
	write_channel(array, index, value);
}

static void get_box_sizes(double radius, int sizes[3]) {
	double ideal = sqrt(12 * radius * radius / 3 + 1);
	int lower = floor(ideal);
	if (lower % 2 == 0)
		lower--;
	double count = (12 * radius * radius - 3.0 * lower * lower - 4 * 3.0 * lower - 3 * 3.0) / (-4.0 * lower - 4);
	int smaller = round_like_source(count);
	for (int index = 0; index < 3; index++)
		sizes[index] = index < smaller ? lower : lower + 2;
}

static void blur_rows(const Record *source, Record *target, gint64 width, gint64 height, gint64 radius) {
	double scale = 1.0 / (radius + radius + 1);
	gint64 middle = width - radius - radius - 1;
	for (gint64 row = 0; row < height; row++) {
		gint64 out = row * width, back = out, ahead = out + radius;
		double first = read_source_array(source, out), last = read_source_array(source, out + width - 1);
		double sum = (radius + 1) * first;
		for (gint64 step = 0; step < radius; step++)
			sum += read_source_array(source, out + step);
		for (gint64 step = 0; step <= radius; step++) {
			sum += read_source_array(source, ahead + step) - first;
			write_source_array(target, out + step, sum * scale);
		}
		ahead += radius + 1;
		out += radius + 1;
		for (gint64 step = 0; step < middle; step++) {
			sum += read_source_array(source, ahead + step) - read_source_array(source, back + step);
			write_source_array(target, out + step, sum * scale);
		}
		ahead += middle;
		back += middle;
		out += middle;
		for (gint64 step = width - radius; step < width; step++) {
			sum += last - read_source_array(source, back++);
			write_source_array(target, out++, sum * scale);
		}
	}
}

static void blur_columns(const Record *source, Record *target, gint64 width, gint64 height, gint64 radius) {
	double scale = 1.0 / (radius + radius + 1);
	gint64 middle = height - radius - radius - 1;
	for (gint64 column = 0; column < width; column++) {
		gint64 out = column, back = out, ahead = out + radius * width;
		double first = read_source_array(source, out), last = read_source_array(source, out + width * (height - 1));
		double sum = (radius + 1) * first;
		for (gint64 step = 0; step < radius; step++)
			sum += read_source_array(source, out + step * width);
		for (gint64 step = 0; step <= radius; step++) {
			sum += read_source_array(source, ahead) - first;
			write_source_array(target, out, sum * scale);
			ahead += width;
			out += width;
		}
		for (gint64 step = 0; step < middle; step++) {
			gint64 offset = step * width;
			sum += read_source_array(source, ahead + offset) - read_source_array(source, back + offset);
			write_source_array(target, out + offset, sum * scale);
		}
		back += middle * width;
		ahead += middle * width;
		out += middle * width;
		for (gint64 step = height - radius; step < height; step++) {
			sum += last - read_source_array(source, back);
			write_source_array(target, out, sum * scale);
			back += width;
			out += width;
		}
	}
}

static void blur_mask(Record *values, RasterExtent extent, double radius) {
	int sizes[3];
	get_box_sizes(radius, sizes);
	Record *scratch = create_channels(values->value.buffer.length, values->value.buffer.depth);
	for (int pass = 0; pass < 3; pass++) {
		blur_rows(values, scratch, extent.width, extent.height, (sizes[pass] - 1) >> 1);
		blur_columns(scratch, values, extent.width, extent.height, (sizes[pass] - 1) >> 1);
	}
	free_record(scratch);
}

static Record *blur_selection(const Record *selection, double radius, gboolean smooth, RasterExtent canvas,
							  gboolean at_canvas_bounds) {
	if (smooth)
		radius = round_like_source(radius * .7);
	RasterExtent from = read_extent(get_record(selection, "V"));
	RasterExtent extent = grow_extent(from, ceil(2.6 * radius));
	if (!at_canvas_bounds)
		extent = intersect_region(extent, canvas);
	if (empty_extent(extent))
		return NULL;
	const Record *source = get_record(selection, "C");
	unsigned depth = source->value.buffer.depth;
	gsize count = count_extent(extent);
	Record *values = create_channels(pad_like_source(count, depth), depth);
	copy_selection_mask(source, from, values, extent);
	blur_mask(values, extent, radius);
	double full = channel_maximum(depth), half = full / 2;
	for (gsize index = 0; smooth && index < values->value.buffer.length; index++)
		write_channel(values, index,
					  source_max(0, source_min(full, half + (read_channel(values, index) - half) * radius * 2.5)));
	Record *mask = create_channels(count, depth);
	memcpy(mask->value.buffer.data, values->value.buffer.data, count * (depth / 8));
	free_record(values);
	return create_selection(extent, mask);
}

/* ============================== Modifications ============================== */

static Record *expand_selection(const Record *selection, int amount) {
	RasterExtent from = read_extent(get_record(selection, "V")), extent = grow_extent(from, amount);
	const Record *source = get_record(selection, "C");
	unsigned depth = source->value.buffer.depth;
	Record *copied = create_channels(count_extent(extent), depth);
	copy_selection_mask(source, from, copied, extent);
	Record *bytes = convert_mask_depth(copied, 8), *coverage = create_channels(count_extent(extent), 8);
	draw_distance_coverage(bytes, coverage, extent, amount);
	Record *mask = convert_mask_depth(coverage, depth);
	free_record(copied);
	free_record(bytes);
	free_record(coverage);
	return create_selection(extent, mask);
}

static Record *contract_selection(const Record *selection, int amount, RasterExtent canvas, gboolean at_canvas_bounds) {
	RasterExtent from = read_extent(get_record(selection, "V")), extent = grow_extent(from, 1);
	if (!at_canvas_bounds)
		extent = intersect_region(extent, canvas);
	if (empty_extent(extent))
		return NULL;
	const Record *source = get_record(selection, "C");
	unsigned depth = source->value.buffer.depth;
	Record *copied = create_channels(count_extent(extent), depth);
	copy_selection_mask(source, from, copied, extent);
	invert_mask(copied);
	Record *bytes = convert_mask_depth(copied, 8), *coverage = create_channels(count_extent(extent), 8);
	draw_distance_coverage(bytes, coverage, extent, amount);
	invert_mask(coverage);
	Record *mask = convert_mask_depth(coverage, depth);
	free_record(copied);
	free_record(bytes);
	free_record(coverage);
	return create_selection(extent, mask);
}

Record *create_selection_border(const Record *selection, double inner, double outer) {
	RasterExtent from = read_extent(get_record(selection, "V"));
	RasterExtent extent = grow_extent(from, MAX(1, (int)ceil(outer)));
	const Record *source = get_record(selection, "C");
	gsize count = count_extent(extent);
	Record *bytes = convert_mask_depth(source, 8), *area = create_channels(count, 8);
	Record *outside = create_channels(count, 8), *inside = create_channels(count, 8);
	copy_selection_mask(bytes, from, area, extent);
	if (outer != 0)
		draw_distance_coverage(area, outside, extent, outer);
	else
		memcpy(outside->value.buffer.data, area->value.buffer.data, count);
	invert_mask(area);
	if (inner != 0)
		draw_distance_coverage(area, inside, extent, inner);
	else
		memcpy(inside->value.buffer.data, area->value.buffer.data, count);
	guint8 *band = outside->value.buffer.data;
	const guint8 *kept = inside->value.buffer.data;
	for (gsize index = 0; index < count; index++)
		band[index] = (double)band[index] * kept[index] * (1.0 / 255);
	Record *mask = convert_mask_depth(outside, source->value.buffer.depth);
	free_record(bytes);
	free_record(area);
	free_record(outside);
	free_record(inside);
	return create_selection(extent, mask);
}

Record *create_modified_selection(const Record *selection, RasterExtent canvas, SelectionModification modification,
								  int amount, gboolean at_canvas_bounds) {
	switch (modification) {
	case MODIFY_BORDER:
		return create_selection_border(selection, amount / 2.0, amount / 2.0);
	case MODIFY_SMOOTH:
		return blur_selection(selection, amount, TRUE, canvas, at_canvas_bounds);
	case MODIFY_EXPAND:
		return expand_selection(selection, amount);
	case MODIFY_CONTRACT:
		return contract_selection(selection, amount, canvas, at_canvas_bounds);
	case MODIFY_FEATHER:
		return blur_selection(selection, amount, FALSE, canvas, at_canvas_bounds);
	}
	return NULL;
}

static Record *clip_selection(const Document *document, Record *selection, RasterExtent canvas) {
	RasterExtent from = read_extent(get_record(selection, "V")), extent = intersect_region(from, canvas);
	if (empty_extent(extent)) {
		free_record(selection);
		return NULL;
	}
	Record *mask = create_selection_mask(document, extent);
	copy_selection_mask(get_record(selection, "C"), from, mask, extent);
	free_record(selection);
	return create_selection(extent, mask);
}

static Record *create_modify_name(SelectionModification modification) {
	Record *name = create_array();
	for (int index = 0; index < 3 && MODIFY_NAMES[modification][index] >= 0; index++)
		append_record(name, create_number(MODIFY_NAMES[modification][index]));
	return name;
}

gboolean modify_selection(Document *document, SelectionModification modification, int amount, gboolean at_canvas_bounds,
						  gboolean extended_history) {
	Record *selection = get_document_selection(document);
	if (!selection)
		return FALSE;
	RasterExtent canvas = {0, 0, document_width(document), document_height(document)};
	Record *modified = create_modified_selection(selection, canvas, modification, amount, at_canvas_bounds);
	gboolean outward = modification == MODIFY_EXPAND || modification == MODIFY_CONTRACT;
	if (modified && outward && !at_canvas_bounds)
		modified = clip_selection(document, modified, canvas);
	modified = trim_selection(document, modified);
	return record_named_selection(document, modified, create_modify_name(modification), extended_history);
}
