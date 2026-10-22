#include <math.h>
#include <string.h>

#include "selection-path.h"

/* Curves are split into lines that stay this close to the curve, in pixels */
#define CURVE_TOLERANCE 0.01
#define MAXIMUM_CURVE_LINES 4096
#define ELLIPSE_HANDLE 0.5522848

/* One straight piece of the outline in mask coordinates */
typedef struct {
	double x0, y0, x1, y1, top, bottom;
} PathLine;

SelectionPath *create_selection_path(void) {
	SelectionPath *path = g_new(SelectionPath, 1);
	path->commands = g_string_new(NULL);
	path->points = g_array_new(FALSE, FALSE, sizeof(double));
	return path;
}

void free_selection_path(SelectionPath *path) {
	if (!path)
		return;
	g_string_free(path->commands, TRUE);
	g_array_unref(path->points);
	g_free(path);
}

static void add_path_points(SelectionPath *path, char command, const double *points, guint count) {
	g_string_append_c(path->commands, command);
	g_array_append_vals(path->points, points, count);
}

SelectionPath *create_ellipse_path(RasterExtent box) {
	double left = box.x, top = box.y, right = box.x + box.width, bottom = box.y + box.height;
	double center_x = (left + right) / 2, center_y = (top + bottom) / 2;
	double across = (left - right) / 2, down = (top - bottom) / 2, handle = ELLIPSE_HANDLE;
	SelectionPath *path = create_selection_path();
	add_path_points(path, 'M', (double[]){center_x, center_y - down}, 2);
	add_path_points(path, 'C',
					(double[]){center_x + handle * across, center_y - down, center_x + across, center_y - handle * down,
							   center_x + across, center_y},
					6);
	add_path_points(path, 'C',
					(double[]){center_x + across, center_y + handle * down, center_x + handle * across, center_y + down,
							   center_x, center_y + down},
					6);
	add_path_points(path, 'C',
					(double[]){center_x - handle * across, center_y + down, center_x - across, center_y + handle * down,
							   center_x - across, center_y},
					6);
	add_path_points(path, 'C',
					(double[]){center_x - across, center_y - handle * down, center_x - handle * across, center_y - down,
							   center_x, center_y - down},
					6);
	return path;
}

SelectionPath *create_polygon_path(const double *points, guint count) {
	SelectionPath *path = create_selection_path();
	for (guint index = 0; index < count; index++)
		add_path_points(path, index ? 'L' : 'M', points + index * 2, 2);
	return path;
}

RasterExtent get_selection_path_bounds(const SelectionPath *path) {
	if (path->points->len < 2)
		return (RasterExtent){0};
	const double *points = (const double *)path->points->data;
	double left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
	for (guint index = 0; index + 1 < path->points->len; index += 2) {
		left = MIN(left, points[index]);
		right = MAX(right, points[index]);
		top = MIN(top, points[index + 1]);
		bottom = MAX(bottom, points[index + 1]);
	}
	int x = floor(left), y = floor(top);
	return (RasterExtent){x, y, (int)ceil(right) - x, (int)ceil(bottom) - y};
}

/* ============================== Coverage ============================== */

static void add_path_line(GArray *lines, double x0, double y0, double x1, double y1) {
	if (y0 == y1)
		return;
	PathLine line = {x0, y0, x1, y1, MIN(y0, y1), MAX(y0, y1)};
	g_array_append_val(lines, line);
}

/* Uniform steps keep a cubic within the tolerance when their count grows with the root of its bend */
static void add_curve_lines(GArray *lines, const double *from, const double *curve) {
	double bend_x = MAX(fabs(from[0] - 2 * curve[0] + curve[2]), fabs(curve[0] - 2 * curve[2] + curve[4]));
	double bend_y = MAX(fabs(from[1] - 2 * curve[1] + curve[3]), fabs(curve[1] - 2 * curve[3] + curve[5]));
	int steps = CLAMP((int)ceil(sqrt(hypot(bend_x, bend_y) * 0.75 / CURVE_TOLERANCE)), 1, MAXIMUM_CURVE_LINES);
	double previous_x = from[0], previous_y = from[1];
	for (int step = 1; step <= steps; step++) {
		double t = (double)step / steps, u = 1 - t;
		double x = u * u * u * from[0] + 3 * u * u * t * curve[0] + 3 * u * t * t * curve[2] + t * t * t * curve[4];
		double y = u * u * u * from[1] + 3 * u * u * t * curve[1] + 3 * u * t * t * curve[3] + t * t * t * curve[5];
		add_path_line(lines, previous_x, previous_y, x, y);
		previous_x = x;
		previous_y = y;
	}
}

/* Every subpath is closed like a canvas fill closes it, and points become mask coordinates */
static GArray *flatten_selection_path(const SelectionPath *path, RasterExtent bounds) {
	GArray *lines = g_array_new(FALSE, FALSE, sizeof(PathLine));
	g_autoptr(GArray) points = g_array_copy(path->points);
	double *values = (double *)points->data;
	for (guint index = 0; index + 1 < points->len; index += 2) {
		values[index] -= bounds.x;
		values[index + 1] -= bounds.y;
	}
	double start[2] = {0}, current[2] = {0};
	guint position = 0;
	for (gsize index = 0; index < path->commands->len; index++) {
		char command = path->commands->str[index];
		if (command == 'M') {
			add_path_line(lines, current[0], current[1], start[0], start[1]);
			start[0] = current[0] = values[position];
			start[1] = current[1] = values[position + 1];
			position += 2;
		} else if (command == 'L') {
			add_path_line(lines, current[0], current[1], values[position], values[position + 1]);
			current[0] = values[position];
			current[1] = values[position + 1];
			position += 2;
		} else {
			add_curve_lines(lines, current, values + position);
			current[0] = values[position + 4];
			current[1] = values[position + 5];
			position += 6;
		}
	}
	add_path_line(lines, current[0], current[1], start[0], start[1]);
	return lines;
}

static int compare_line_tops(gconstpointer first, gconstpointer second) {
	double a = ((const PathLine *)first)->top, b = ((const PathLine *)second)->top;
	return (a > b) - (a < b);
}

/* Adds the signed area a line covers within one row to the cells it crosses, so a running sum along the row
 * gives each pixel's winding weighted coverage */
static void accumulate_line_row(float *row, int width, const PathLine *line, int top) {
	double start = MAX(line->top, top), end = MIN(line->bottom, top + 1);
	if (end <= start)
		return;
	double slope = (line->x1 - line->x0) / (line->y1 - line->y0);
	/* Curve points can stray from the box by rounding error, which must not reach outside the row */
	double first = CLAMP(line->x0 + (start - line->y0) * slope, 0, width);
	double second = CLAMP(line->x0 + (end - line->y0) * slope, 0, width);
	double area = (end - start) * (line->y1 > line->y0 ? 1 : -1);
	double left = MIN(first, second), right = MAX(first, second);
	int cell = floor(left), last = ceil(right);
	if (cell + 1 >= last) {
		double middle = (first + second) / 2 - cell;
		row[cell] += area * (1 - middle);
		row[cell + 1] += area * middle;
		return;
	}
	double step = 1 / (right - left), left_part = left - cell, right_part = right - last + 1;
	double first_share = 0.5 * step * (1 - left_part) * (1 - left_part);
	double last_share = 0.5 * step * right_part * right_part;
	row[cell] += area * first_share;
	if (last == cell + 2)
		row[cell + 1] += area * (1 - first_share - last_share);
	else {
		double second_share = step * (1.5 - left_part);
		row[cell + 1] += area * (second_share - first_share);
		for (int x = cell + 2; x < last - 1; x++)
			row[x] += area * step;
		double before_last = second_share + (last - cell - 3) * step;
		row[last - 1] += area * (1 - before_last - last_share);
	}
	row[last] += area * last_share;
}

static double widen_alpha(int alpha, unsigned depth) {
	return depth == 16 ? alpha * 257 : depth == 32 ? alpha / 255.0 : alpha;
}

static double round_alpha(double value, unsigned depth) {
	double full = depth == 16 ? 65535 : depth == 32 ? 1 : 255;
	double half = depth == 32 ? 0.5 : floor(full / 2);
	return value < half ? 0 : full;
}

static void write_mask_row(Record *mask, const float *row, int width, int y, unsigned depth, gboolean anti_alias) {
	double sum = 0;
	for (int x = 0; x < width; x++) {
		sum += row[x];
		int alpha = (int)(MIN(1, fabs(sum)) * 255 + 0.5);
		double value = widen_alpha(alpha, depth);
		write_channel(mask, (gsize)y * width + x, anti_alias ? value : round_alpha(value, depth));
	}
}

Record *rasterize_selection_path(const SelectionPath *path, unsigned depth, gboolean anti_alias, RasterExtent *bounds) {
	*bounds = get_selection_path_bounds(path);
	if (empty_extent(*bounds))
		return NULL;
	int width = bounds->width, height = bounds->height;
	Record *mask = create_channels((gsize)width * height, depth);
	g_autoptr(GArray) lines = flatten_selection_path(path, *bounds);
	g_array_sort(lines, compare_line_tops);
	g_autoptr(GArray) active = g_array_new(FALSE, FALSE, sizeof(PathLine));
	g_autofree float *row = g_new(float, width + 2);
	guint next = 0;
	for (int y = 0; y < height; y++) {
		for (guint index = 0; index < active->len;)
			if (g_array_index(active, PathLine, index).bottom <= y)
				g_array_remove_index_fast(active, index);
			else
				index++;
		for (; next < lines->len && g_array_index(lines, PathLine, next).top < y + 1; next++)
			g_array_append_val(active, g_array_index(lines, PathLine, next));
		memset(row, 0, sizeof(float) * (width + 2));
		for (guint index = 0; index < active->len; index++)
			accumulate_line_row(row, width, &g_array_index(active, PathLine, index), y);
		write_mask_row(mask, row, width, y, depth, anti_alias);
	}
	return mask;
}

void constrain_polygon_angle(double start_x, double start_y, double *x, double *y) {
	if (*x == start_x && *y == start_y)
		return;
	double across = *x - start_x, down = *y - start_y, width = fabs(across), height = fabs(down);
	if (width > height)
		height = width / 2 > height ? 0 : width;
	if (height > width)
		width = height / 2 > width ? 0 : height;
	width = across > 0 ? width : -width;
	height = down > 0 ? height : -height;
	double scale = sqrt(across * across + down * down) / sqrt(width * width + height * height);
	*x = start_x + width * scale;
	*y = start_y + height * scale;
}

/* ============================== Modifiers ============================== */

void begin_modifier_tracking(ModifierTracker *tracker, gboolean shift, gboolean alt) {
	*tracker = (ModifierTracker){TRUE, FALSE, shift, alt, shift, alt, shift ? 0 : 1, alt ? 0 : 1};
}

void update_modifier_tracking(ModifierTracker *tracker, gboolean shift, gboolean alt) {
	tracker->shift = shift;
	tracker->alt = alt;
	if (!tracker->dragged)
		return;
	if (tracker->shift_stage == 0 && !shift)
		tracker->shift_stage++;
	if (tracker->shift_stage == 1 && shift)
		tracker->shift_stage++;
	if (tracker->alt_stage == 0 && !alt)
		tracker->alt_stage++;
	if (tracker->alt_stage == 1 && alt)
		tracker->alt_stage++;
}

gboolean is_shift_held(const ModifierTracker *tracker) {
	return tracker->pressed ? tracker->start_shift : tracker->shift;
}

gboolean is_alt_held(const ModifierTracker *tracker) {
	return tracker->pressed ? tracker->start_alt : tracker->alt;
}

gboolean was_shift_pressed_again(const ModifierTracker *tracker) {
	return tracker->dragged && tracker->shift_stage == 2;
}

gboolean was_alt_pressed_again(const ModifierTracker *tracker) {
	return tracker->dragged && tracker->alt_stage == 2;
}
