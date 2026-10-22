#include <math.h>
#include <string.h>

#include "history.h"
#include "layer-commands.h"
#include "move-tool.h"
#include "pixel-selection.h"
#include "transform.h"
#include "../lang/en.h"

static const char *TRANSFORM_OPERATION = "100";

/* ============================== Projective math ============================== */

static gboolean solve_system(double system[8][9], double result[8]) {
	for (int column = 0; column < 8; column++) {
		int pivot = column;
		for (int row = column; row < 8; row++)
			if (fabs(system[row][column]) > fabs(system[pivot][column]))
				pivot = row;
		if (pivot != column)
			for (int index = 0; index < 9; index++) {
				double swap = system[column][index];
				system[column][index] = system[pivot][index];
				system[pivot][index] = swap;
			}
		if (system[column][column] == 0)
			return FALSE;
		for (int row = column + 1; row < 8; row++) {
			double factor = system[row][column] / system[column][column];
			for (int index = column; index < 9; index++)
				system[row][index] -= system[column][index] * factor;
		}
	}
	for (int row = 7; row >= 0; row--) {
		if (system[row][row] == 0)
			return FALSE;
		result[row] = system[row][8] / system[row][row];
		for (int above = row - 1; above >= 0; above--) {
			system[above][8] -= system[above][row] * result[row];
			system[above][row] = 0;
		}
	}
	return TRUE;
}

gboolean projective_from_rectangle(const double quad[8], double x, double y, double width, double height,
								   Projective *result) {
	const double corners[8] = {x, y, x + width, y, x + width, y + height, x, y + height};
	double system[8][9];
	for (int point = 0; point < 4; point++) {
		double from_x = corners[point * 2], from_y = corners[point * 2 + 1];
		double to_x = quad[point * 2], to_y = quad[point * 2 + 1];
		const double first[9] = {from_x, from_y, 1, 0, 0, 0, -from_x * to_x, -from_y * to_x, to_x};
		const double second[9] = {0, 0, 0, from_x, from_y, 1, -from_x * to_y, -from_y * to_y, to_y};
		memcpy(system[point * 2], first, sizeof first);
		memcpy(system[point * 2 + 1], second, sizeof second);
	}
	return solve_system(system, result->m);
}

Projective invert_projective(Projective matrix) {
	const double *m = matrix.m;
	double scale = 1 / (m[0] * m[4] - m[1] * m[3]);
	return (Projective){{(m[4] - m[5] * m[7]) * scale, (m[2] * m[7] - m[1]) * scale,
						 (m[1] * m[5] - m[2] * m[4]) * scale, (m[5] * m[6] - m[3]) * scale,
						 (m[0] - m[2] * m[6]) * scale, (m[3] * m[2] - m[0] * m[5]) * scale,
						 (m[3] * m[7] - m[4] * m[6]) * scale, (m[1] * m[6] - m[0] * m[7]) * scale}};
}

Projective multiply_projective(Projective first, Projective second) {
	const double *a = first.m, *b = second.m;
	double result[8] = {a[0] * b[0] + a[1] * b[3] + a[2] * b[6], a[0] * b[1] + a[1] * b[4] + a[2] * b[7],
						a[0] * b[2] + a[1] * b[5] + a[2],		 a[3] * b[0] + a[4] * b[3] + a[5] * b[6],
						a[3] * b[1] + a[4] * b[4] + a[5] * b[7], a[3] * b[2] + a[4] * b[5] + a[5],
						a[6] * b[0] + a[7] * b[3] + b[6],		 a[6] * b[1] + a[7] * b[4] + b[7]};
	double scale = 1 / (a[6] * b[2] + a[7] * b[5] + 1);
	Projective product;
	for (int index = 0; index < 8; index++)
		product.m[index] = result[index] * scale;
	return product;
}

void apply_projective(Projective matrix, double x, double y, double *result_x, double *result_y) {
	const double *m = matrix.m;
	double scale = 1 / (m[6] * x + m[7] * y + 1);
	*result_x = (m[0] * x + m[1] * y + m[2]) * scale;
	*result_y = (m[3] * x + m[4] * y + m[5]) * scale;
}

gboolean is_convex_quad(const double quad[8]) {
	int positive = 0, negative = 0;
	for (int point = 0; point < 4; point++) {
		const double *a = quad + point * 2, *b = quad + (point + 1) % 4 * 2, *c = quad + (point + 2) % 4 * 2;
		double turn = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]);
		if (turn > 0)
			positive++;
		if (turn < 0)
			negative++;
	}
	return positive == 4 || negative == 4;
}

/* ============================== Box handles ============================== */

static double meet_line(double x, double y, double direction_x, double direction_y, double line_x, double line_y,
						double line_direction_x, double line_direction_y) {
	double numerator = line_direction_x * (y - line_y) - line_direction_y * (x - line_x);
	double denominator = line_direction_y * direction_x - line_direction_x * direction_y;
	return denominator == 0 ? 0 : numerator / denominator;
}

static double measure_line_offset(double first_x, double first_y, double second_x, double second_y, double x,
								  double y) {
	double across = second_x - first_x, down = second_y - first_y;
	return meet_line(x, y, -down, across, first_x, first_y, across, down);
}

static void to_box_unit(const double quad[8], double x, double y, double *u, double *v) {
	Projective box = {{1, 0, 0, 0, 1, 0, 0, 0}};
	projective_from_rectangle(quad, 0, 0, 1, 1, &box);
	apply_projective(invert_projective(box), x, y, u, v);
}

static void transform_box(const double source[8], Projective unit_change, double target[8]) {
	Projective box = {{1, 0, 0, 0, 1, 0, 0, 0}};
	projective_from_rectangle(source, 0, 0, 1, 1, &box);
	const double corners[8] = {0, 0, 1, 0, 1, 1, 0, 1};
	for (int point = 0; point < 4; point++) {
		double u, v;
		apply_projective(unit_change, corners[point * 2], corners[point * 2 + 1], &u, &v);
		apply_projective(box, u, v, &target[point * 2], &target[point * 2 + 1]);
	}
}

static gboolean is_axis_aligned(const double quad[8]) {
	return (fabs(quad[1] - quad[3]) < 1e-6 && fabs(quad[2] - quad[4]) < 1e-6) ||
		   (fabs(quad[0] - quad[2]) < 1e-6 && fabs(quad[3] - quad[5]) < 1e-6);
}

static void scale_around_reference(const BoxDrag *drag, double x, double y, gboolean uniform, double next[8]) {
	double across = drag->handle_x - drag->reference_x, down = drag->handle_y - drag->reference_y;
	double scale_x = fabs(across) >= 1 ? (x - drag->reference_x) / across : 1;
	double scale_y = fabs(down) >= 1 ? (y - drag->reference_y) / down : 1;
	if (uniform)
		scale_x = scale_y = (scale_x + scale_y) / 2;
	for (int point = 0; point < 4; point++) {
		next[point * 2] = drag->reference_x + (drag->start[point * 2] - drag->reference_x) * scale_x;
		next[point * 2 + 1] = drag->reference_y + (drag->start[point * 2 + 1] - drag->reference_y) * scale_y;
	}
}

static void skew_box_edge(const BoxDrag *drag, double x, double y, gboolean shift, gboolean alt, double next[8]) {
	const int first_corners[9] = {0, 0, 0, 3, 0, 1, 0, 2, 0};
	const double *start = drag->start;
	int first = first_corners[drag->handle], second = (first + 1) & 3;
	double first_x = start[first * 2], first_y = start[first * 2 + 1];
	double second_x = start[second * 2], second_y = start[second * 2 + 1];
	if (shift) {
		double offset = measure_line_offset(first_x, first_y, second_x, second_y, x, y);
		x -= offset * (second_y - first_y);
		y += offset * (second_x - first_x);
	}
	double moved_x = x - drag->handle_x, moved_y = y - drag->handle_y;
	double base_x = drag->reference_x, base_y = drag->reference_y;
	double along_x = base_x + second_x - first_x, along_y = base_y + second_y - first_y;
	double handle_offset = measure_line_offset(base_x, base_y, along_x, along_y, drag->handle_x, drag->handle_y);
	for (int point = 0; point < 4; point++) {
		if (!alt && point != first && point != second)
			continue;
		double share = measure_line_offset(base_x, base_y, along_x, along_y, start[point * 2], start[point * 2 + 1]) /
					   handle_offset;
		next[point * 2] += share * moved_x;
		next[point * 2 + 1] += share * moved_y;
	}
}

static void project_on_diagonal(const BoxDrag *drag, double anchor_x, double anchor_y, double *x, double *y) {
	double direction_x = drag->handle_x - anchor_x, direction_y = drag->handle_y - anchor_y;
	for (int pass = 0; pass < 16; pass++) {
		double offset = meet_line(*x, *y, -direction_y, direction_x, anchor_x, anchor_y, direction_x, direction_y);
		double projected_x = *x - direction_y * offset, projected_y = *y + direction_x * offset;
		if (drag->snap)
			drag->snap(drag->snap_data, &projected_x, &projected_y);
		if (hypot(projected_x - *x, projected_y - *y) < 1e-6)
			return;
		*x = projected_x;
		*y = projected_y;
	}
}

static double get_unit_scale(double now, double was, double anchor, gboolean clamps) {
	double scale = fabs(was - anchor) > 1e-9 ? (now - anchor) / (was - anchor) : 1;
	if (scale == 0)
		scale = 1e-4;
	return clamps && !(scale > 0) ? 0 : scale;
}

static void scale_in_unit_square(const BoxDrag *drag, const double current[8], double x, double y, gboolean shift,
								 gboolean alt, gboolean control, double next[8]) {
	const double *start = drag->start;
	int handle = drag->handle;
	gboolean corner = handle == 0 || handle == 2 || handle == 6 || handle == 8;
	gboolean uniform = shift || drag->keeps_ratio;
	if (is_axis_aligned(current)) {
		x = round_like_source(x);
		y = round_like_source(y);
	}
	const int opposite[9] = {8, 7, 6, 5, 4, 3, 2, 1, 0}, corner_points[9] = {0, 0, 1, 0, 0, 0, 3, 0, 2};
	double anchor_u = (opposite[handle] % 3) / 2.0, anchor_v = (opposite[handle] / 3) / 2.0;
	int anchor_point = corner_points[opposite[handle]];
	double anchor_x = start[anchor_point * 2], anchor_y = start[anchor_point * 2 + 1];
	if (alt) {
		anchor_x = drag->reference_x;
		anchor_y = drag->reference_y;
		to_box_unit(start, anchor_x, anchor_y, &anchor_u, &anchor_v);
	}
	if (corner && uniform)
		project_on_diagonal(drag, anchor_x, anchor_y, &x, &y);
	double now_u, now_v, was_u, was_v;
	to_box_unit(start, x, y, &now_u, &now_v);
	to_box_unit(start, drag->handle_x, drag->handle_y, &was_u, &was_v);
	double scale_u = get_unit_scale(now_u, was_u, anchor_u, drag->clamps_scale);
	double scale_v = get_unit_scale(now_v, was_v, anchor_v, drag->clamps_scale);
	double scale_x = 1, scale_y = 1;
	/* Control with Shift on an edge keeps its own axis unscaled, as gu._X a7 does */
	gboolean fixed = control && shift, both = uniform && !fixed;
	if (corner) {
		scale_x = scale_u;
		scale_y = uniform ? scale_u : scale_v;
	} else if (handle == 1 || handle == 7) {
		scale_x = both ? scale_v : 1;
		scale_y = fixed ? 1 : scale_v;
	} else if (handle == 3 || handle == 5) {
		scale_x = fixed ? 1 : scale_u;
		scale_y = both ? scale_u : 1;
	}
	Projective change = {{scale_x, 0, anchor_u - scale_x * anchor_u, 0, scale_y, anchor_v - scale_y * anchor_v, 0, 0}};
	transform_box(start, change, next);
}

void scale_box(const BoxDrag *drag, const double current[8], double x, double y, gboolean shift, gboolean alt,
			   gboolean control, double next[8]) {
	gboolean corner = drag->handle == 0 || drag->handle == 2 || drag->handle == 6 || drag->handle == 8;
	memcpy(next, drag->start, sizeof drag->start);
	if (corner && alt)
		scale_around_reference(drag, x, y, shift || drag->keeps_ratio, next);
	else if (!corner && control && drag->skews)
		skew_box_edge(drag, x, y, shift, alt, next);
	else
		scale_in_unit_square(drag, current, x, y, shift, alt, control, next);
}

/* ============================== Resampling ============================== */

static double measure_footprint(Projective unit, int width, int height, double x, double y, double u, double v) {
	double across_x, across_y, down_x, down_y;
	apply_projective(unit, u + 1.0 / width, v, &across_x, &across_y);
	apply_projective(unit, u, v + 1.0 / height, &down_x, &down_y);
	double across = (across_x - x) * (across_x - x) + (across_y - y) * (across_y - y);
	double down = (down_x - x) * (down_x - x) + (down_y - y) * (down_y - y);
	return 1 / sqrt(MAX(across, down));
}

/* Pixels are packed as their RGBA bytes read little endian views */
static guint32 pack(const guint8 *pixel) {
	return pixel[0] | (guint32)pixel[1] << 8 | (guint32)pixel[2] << 16 | (guint32)pixel[3] << 24;
}

static guint32 sample_bilinear(double x, double y, const guint8 *source, int width, int height, gboolean clamp) {
	x -= .499999;
	y -= .499999;
	int left = floor(x), top = floor(y);
	guint32 corners[4] = {0};
	for (int corner = 0; corner < 4; corner++) {
		int column = left + (corner & 1), row = top + (corner >> 1);
		if (clamp) {
			column = CLAMP(column, 0, width - 1);
			row = CLAMP(row, 0, height - 1);
		} else if (column < 0 || column >= width || row < 0 || row >= height)
			continue;
		corners[corner] = pack(source + ((gsize)row * width + column) * 4);
	}
	double across = x - left, down = y - top;
	double weights[4] = {(1 - down) * (1 - across), (1 - down) * across, down * (1 - across), down * across};
	double alpha = 0, red = 0, green = 0, blue = 0;
	for (int corner = 0; corner < 4; corner++) {
		double weight = weights[corner] * (corners[corner] >> 24);
		alpha += weight;
		red += weight * (corners[corner] & 255);
		green += weight * (corners[corner] >> 8 & 255);
		blue += weight * (corners[corner] >> 16 & 255);
	}
	if (alpha == 0)
		return 0;
	double scale = 1 / alpha;
	return (guint32)(alpha + .5) << 24 | (guint32)(blue * scale + .5) << 16 | (guint32)(green * scale + .5) << 8 |
		   (guint32)(red * scale + .5);
}

static void draw_nearest(Projective inverse, const guint32 *source, int width, int height, RasterExtent area,
						 gboolean affine, guint32 *target) {
	const double *m = inverse.m;
	double a = m[0] * width, b = m[1] * width, c = m[2] * width, d = m[3] * height, e = m[4] * height;
	double f = m[5] * height;
	gsize at = 0;
	for (double y = area.y + .5; y < area.y + area.height; y++)
		for (double x = area.x + .5; x < area.x + area.width; x++, at++) {
			double scale = affine ? 1 : 1 / (m[6] * x + m[7] * y + 1);
			double u = (a * x + b * y + c) * scale, v = (d * x + e * y + f) * scale;
			target[at] =
				0 < u && u < width && 0 < v && v < height ? source[(gsize)floor(v) * width + (gsize)floor(u)] : 0;
		}
}

static Record *draw_resampled(const Record *pixels, RasterExtent extent, Projective matrix, gboolean nearest,
							  RasterExtent *result) {
	Projective rectangle = {{extent.width, 0, extent.x, 0, extent.height, extent.y, 0, 0}};
	Projective unit = multiply_projective(matrix, rectangle), inverse = invert_projective(unit);
	double left = G_MAXDOUBLE, top = G_MAXDOUBLE, right = -G_MAXDOUBLE, bottom = -G_MAXDOUBLE;
	double outline[96] = {0, 0, 1, 0, 1, 1, 0, 1};
	int count = 8;
	for (double step = .1; step < .99 && count + 8 <= 96; step += .1) {
		const double points[8] = {0, step, step, 0, 1, step, step, 1};
		memcpy(outline + count, points, sizeof points);
		count += 8;
	}
	for (int index = 0; index < count; index += 2) {
		double x, y;
		apply_projective(unit, outline[index], outline[index + 1], &x, &y);
		left = MIN(left, x);
		top = MIN(top, y);
		right = MAX(right, x);
		bottom = MAX(bottom, y);
	}
	int x0 = (int)floor(left + 1e-9), y0 = (int)floor(top + 1e-9);
	int x1 = (int)ceil(right - 1e-9), y1 = (int)ceil(bottom - 1e-9);
	*result = (RasterExtent){x0, y0, x1 - x0, y1 - y0};
	if (result->width <= 0 || result->height <= 0 || result->width > 100000 || result->height > 100000)
		return NULL;
	Record *output = create_channels((gsize)result->width * result->height * 4, 8);
	const guint8 *source = pixels->value.buffer.data;
	guint8 *target = output->value.buffer.data;
	int width = extent.width, height = extent.height;
	gboolean affine = fabs(unit.m[6]) < 1e-9 && fabs(unit.m[7]) < 1e-9;
	gboolean whole = TRUE;
	double size = 0;
	for (int index = 0; index < 8; index++) {
		double rounded = round_like_source(unit.m[index]);
		if (index != 2 && index != 5)
			size += fabs(rounded);
		if (fabs(rounded - unit.m[index]) > 1e-9)
			whole = FALSE;
	}
	if (nearest || (whole && size == width + height && affine)) {
		draw_nearest(inverse, (const guint32 *)source, width, height, *result, affine, (guint32 *)target);
		return output;
	}
	gboolean aligned = affine && ((fabs(unit.m[0]) < 1e-9 && fabs(unit.m[4]) < 1e-9) ||
								  (fabs(unit.m[1]) < 1e-9 && fabs(unit.m[3]) < 1e-9));
	for (int row = 0; row < result->height; row++)
		for (int column = 0; column < result->width; column++) {
			double x = column + result->x + .5, y = row + result->y + .5, u, v;
			apply_projective(inverse, x, y, &u, &v);
			double source_x = u * width, source_y = v * height;
			if (source_x < -1 || source_x > width + 1 || source_y < -1 || source_y > height + 1)
				continue;
			guint32 pixel;
			double footprint = measure_footprint(unit, width, height, x, y, u, v);
			int samples = round(footprint * 2.3);
			if (footprint < 1.1)
				pixel = sample_bilinear(source_x, source_y, source, width, height, aligned);
			else {
				if (source_x < .6 || source_x > width - .6 || source_y < .6 || source_y > height - .6)
					samples = MAX(samples, 5);
				double step = 1.0 / samples, alpha = 0, red = 0, green = 0, blue = 0;
				for (int across = 0; across < samples; across++)
					for (int down = 0; down < samples; down++) {
						double sample_u, sample_v;
						apply_projective(inverse, column + result->x + (across + .5) * step,
										 row + result->y + (down + .5) * step, &sample_u, &sample_v);
						if (sample_u < 0 || sample_u >= 1 || sample_v < 0 || sample_v >= 1)
							continue;
						guint32 value = pack(
							source + ((gsize)floor(sample_v * height) * width + (gsize)floor(sample_u * width)) * 4);
						double weight = value >> 24;
						red += (value & 255) * weight;
						green += (value >> 8 & 255) * weight;
						blue += (value >> 16 & 255) * weight;
						alpha += weight;
					}
				if (alpha == 0)
					continue;
				double scale = 1 / alpha;
				pixel = (guint32)(alpha * step * step + .5) << 24 | (guint32)(blue * scale + .5) << 16 |
						(guint32)(green * scale + .5) << 8 | (guint32)(red * scale + .5);
			}
			guint8 *out = target + ((gsize)row * result->width + column) * 4;
			out[0] = pixel;
			out[1] = pixel >> 8;
			out[2] = pixel >> 16;
			out[3] = pixel >> 24;
		}
	return output;
}

static Record *halve_level(const Record *pixels, RasterExtent extent, RasterExtent *half) {
	int width = extent.width, columns = width >> 1, rows = extent.height >> 1;
	*half = (RasterExtent){(int)((guint32)extent.x >> 1), (int)((guint32)extent.y >> 1), columns, rows};
	Record *output = create_channels((gsize)columns * rows * 4, 8);
	const guint32 *source = pixels->value.buffer.data;
	guint32 *target = output->value.buffer.data;
	for (int row = 0; row < rows; row++)
		for (int column = 0; column < columns; column++) {
			const guint32 *top = source + (gsize)(row << 1) * width + (column << 1);
			const guint32 corners[4] = {top[0], top[1], top[width], top[width + 1]};
			guint total = 0, sums[3] = {0}, weighted[3] = {0};
			for (int corner = 0; corner < 4; corner++) {
				guint alpha = corners[corner] >> 24;
				total += alpha;
				for (int channel = 0; channel < 3; channel++) {
					guint value = corners[corner] >> (channel * 8) & 255;
					sums[channel] += value;
					weighted[channel] += value * alpha;
				}
			}
			guint32 *out = target + (gsize)row * columns + column;
			if (total == 1020)
				*out = 255u << 24 | (sums[2] + 2) >> 2 << 16 | (sums[1] + 2) >> 2 << 8 | (sums[0] + 2) >> 2;
			else if (total == 0)
				*out = 0;
			else {
				double scale = 1.0 / total;
				guint red = (guint)(weighted[0] * scale + .5), green = (guint)(weighted[1] * scale + .5);
				guint blue = (guint)(weighted[2] * scale + .5);
				*out = (total + 2) >> 2 << 24 | blue << 16 | green << 8 | red;
			}
		}
	return output;
}

static void sharpen_pixels(Record *pixels, RasterExtent extent) {
	static const double kernel[9] = {0, -1.0 / 12, 0, -1.0 / 12, 16.0 / 12, -1.0 / 12, 0, -1.0 / 12, 0};
	g_autoptr(Record) copy = copy_record(pixels);
	const guint8 *source = copy->value.buffer.data;
	guint8 *target = pixels->value.buffer.data;
	int width = extent.width, height = extent.height;
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++) {
			double sums[4] = {0};
			for (int index = 0; index < 9; index++) {
				if (kernel[index] == 0)
					continue;
				int column = CLAMP(x - 1 + index % 3, 0, width - 1), row = CLAMP(y - 1 + index / 3, 0, height - 1);
				for (int channel = 0; channel < 4; channel++)
					sums[channel] += source[((gsize)row * width + column) * 4 + channel] * kernel[index];
			}
			for (int channel = 0; channel < 4; channel++)
				target[((gsize)y * width + x) * 4 + channel] = CLAMP((int)(.5 + sums[channel]), 0, 255);
		}
}

Record *resample_image(const Record *pixels, RasterExtent extent, Projective matrix, ResampleMode mode,
					   RasterExtent *result) {
	if (empty_extent(extent))
		return NULL;
	const double *g = matrix.m;
	g_autoptr(Record) level = reference_record((Record *)pixels);
	RasterExtent level_extent = extent;
	if (fabs(g[6]) < 1e-18 && fabs(g[7]) < 1e-18) {
		double scale = sqrt(fabs(g[0] * g[4] - g[3] * g[1]));
		for (int step = 0; level_extent.width >= 2 && level_extent.height >= 2; step++) {
			RasterExtent half = {0, 0, level_extent.width >> 1, level_extent.height >> 1};
			if ((gsize)half.width * half.height < 16 || scale * (1 << step) >= .3)
				break;
			Record *smaller = halve_level(level, level_extent, &half);
			free_record(level);
			level = smaller;
			level_extent = half;
		}
		double scale_x = (double)extent.width / level_extent.width,
			   scale_y = (double)extent.height / level_extent.height;
		double move_x = -level_extent.x * scale_x + extent.x, move_y = -level_extent.y * scale_y + extent.y;
		matrix = (Projective){{scale_x * g[0] + 0 * g[1], 0 * g[0] + scale_y * g[1],
							   move_x * g[0] + move_y * g[1] + g[2], scale_x * g[3] + 0 * g[4],
							   0 * g[3] + scale_y * g[4], move_x * g[3] + move_y * g[4] + g[5], 0, 0}};
	}
	Record *output = draw_resampled(level, level_extent, matrix, mode == RESAMPLE_NEAREST, result);
	if (output && mode == RESAMPLE_SHARPER)
		sharpen_pixels(output, *result);
	return output;
}

Record *resample_projective(const Record *pixels, RasterExtent extent, Projective matrix, RasterExtent *result) {
	return resample_image(pixels, extent, matrix, RESAMPLE_BILINEAR, result);
}

/* ============================== Targets ============================== */

static gboolean is_pixel_layer(Record *layer) {
	return !is_layer_group(layer) && get_number(get_record(layer, "add"), "lsct") == 0;
}

TransformMode get_transform_mode(Document *document, gboolean selection_only) {
	if (selection_only)
		return TRANSFORM_SELECTION;
	Record *selected = get_record(document->record, "j");
	if (get_document_selection(document) && count_records(selected) == 1) {
		Record *layer = get_item(get_record(document->record, "Z"), get_item(selected, 0)->value.number);
		if (is_pixel_layer(layer) && !empty_extent(read_extent(get_record(layer, "V"))))
			return TRANSFORM_PIXELS;
	}
	return TRANSFORM_LAYERS;
}

static RasterExtent find_pixel_bounds(Record *layer) {
	RasterExtent extent = read_extent(get_record(layer, "V"));
	Record *buffer = get_record(layer, "buffer");
	int left = extent.width, right = -1, top = extent.height, bottom = -1;
	for (int y = 0; y < extent.height; y++)
		for (int x = 0; x < extent.width; x++) {
			if (!read_channel(buffer, ((gsize)y * extent.width + x) * 4 + 3))
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

RasterExtent get_transform_bounds(Document *document, TransformMode mode) {
	if (mode != TRANSFORM_LAYERS)
		return get_selection_extent(document);
	g_autoptr(GArray) indices = collect_selected_subtrees(document);
	RasterExtent bounds = {0};
	Record *layers = get_record(document->record, "Z");
	for (guint index = 0; index < indices->len; index++) {
		Record *layer = get_item(layers, g_array_index(indices, int, index));
		if (!is_pixel_layer(layer))
			continue;
		RasterExtent extent = find_pixel_bounds(layer);
		if (empty_extent(extent))
			continue;
		bounds = empty_extent(bounds) ? extent : union_region(bounds, extent);
	}
	if (bounds.width == 0 && bounds.height == 0)
		return bounds;
	if (bounds.width == 0) {
		bounds.x -= 10;
		bounds.width += 20;
	}
	if (bounds.height == 0) {
		bounds.y -= 10;
		bounds.height += 20;
	}
	return bounds;
}

/* ============================== History ============================== */

static void swap_layers(Record *data, Document *document) {
	Record *layers = get_record(document->record, "Z"), *stored = get_record(data, "layers");
	for (guint index = 0; index < count_records(stored); index++) {
		Record *entry = get_item(stored, index), *layer = get_item(layers, get_number(entry, "z"));
		Record *buffer = reference_record(get_record(layer, "buffer")),
			   *extent = reference_record(get_record(layer, "V"));
		RasterExtent before = read_extent(extent);
		set_record(layer, "buffer", reference_record(get_record(entry, "buffer")));
		set_record(layer, "V", reference_record(get_record(entry, "V")));
		set_record(entry, "buffer", buffer);
		set_record(entry, "V", extent);
		RasterExtent after = read_extent(get_record(layer, "V"));
		RasterExtent changed = empty_extent(before)	 ? after
							   : empty_extent(after) ? before
													 : union_region(before, after);
		if (empty_extent(changed))
			continue;
		g_autoptr(Record) region = create_extent(changed.x, changed.y, changed.width, changed.height);
		Record *owners[] = {get_record(layer, "CS"), document->record};
		for (guint owner = 0; owner < G_N_ELEMENTS(owners); owner++) {
			Record *dirty = get_record(owners[owner], "ey");
			gboolean empty = !dirty || dirty->type == RECORD_NULL;
			set_record(owners[owner], "ey", empty ? reference_record(region) : union_extents(dirty, region));
		}
		document->changed = empty_extent(document->changed) ? changed : union_region(document->changed, changed);
	}
	set_boolean(document->record, "di", TRUE);
}

static double get_transform_history_size(const Record *data) {
	Record *stored = get_record(data, "layers");
	double size = 0;
	for (guint index = 0; index < count_records(stored); index++)
		size += get_record(get_item(stored, index), "buffer")->value.buffer.length;
	return size;
}

static const HistoryOperation transform_operation = {"100", swap_layers, swap_layers, get_transform_history_size};

/* ============================== Apply ============================== */

static void trim_pixels(Record **pixels, RasterExtent *extent) {
	Record *holder = create_object(RECORD_PLAIN);
	set_record(holder, "V", create_extent(extent->x, extent->y, extent->width, extent->height));
	set_record(holder, "buffer", reference_record(*pixels));
	RasterExtent bounds = find_pixel_bounds(holder);
	free_record(holder);
	if (equal_region(bounds, *extent))
		return;
	Record *trimmed = create_channels((gsize)MAX(0, bounds.width) * MAX(0, bounds.height) * 4, 8);
	for (int y = 0; y < bounds.height; y++)
		memcpy((guint8 *)trimmed->value.buffer.data + (gsize)y * bounds.width * 4,
			   (guint8 *)(*pixels)->value.buffer.data +
				   ((gsize)(bounds.y - extent->y + y) * extent->width + bounds.x - extent->x) * 4,
			   (gsize)bounds.width * 4);
	free_record(*pixels);
	*pixels = trimmed;
	*extent = bounds;
}

static gboolean apply_to_layers(Document *document, Projective matrix, gboolean extended_history) {
	g_autoptr(GArray) indices = collect_selected_subtrees(document);
	Record *layers = get_record(document->record, "Z"), *stored = create_array();
	for (guint index = 0; index < indices->len; index++) {
		int position = g_array_index(indices, int, index);
		Record *layer = get_item(layers, position);
		if (!is_pixel_layer(layer))
			continue;
		RasterExtent extent = read_extent(get_record(layer, "V")), result;
		Record *pixels = resample_projective(get_record(layer, "buffer"), extent, matrix, &result);
		if (!pixels) {
			result = (RasterExtent){0};
			pixels = create_channels(0, 8);
		} else
			trim_pixels(&pixels, &result);
		Record *entry = create_object(RECORD_PLAIN);
		set_number(entry, "z", position);
		set_record(entry, "buffer", pixels);
		set_record(entry, "V", create_extent(result.x, result.y, result.width, result.height));
		append_record(stored, entry);
	}
	if (!count_records(stored)) {
		free_record(stored);
		return FALSE;
	}
	Record *data = create_object(RECORD_PLAIN);
	set_record(data, "layers", stored);
	Record *entry =
		create_history_entry(create_history_name(10, 16), TRANSFORM_OPERATION, FALSE, g_get_real_time() / 1000);
	set_record(entry, "data", data);
	append_history_entry(document, entry, &transform_operation, extended_history);
	swap_layers(data, document);
	return TRUE;
}

static gboolean apply_to_pixels(Document *document, Projective matrix, gboolean extended_history, GError **error) {
	Record *selected = get_record(document->record, "j");
	int index = get_item(selected, 0)->value.number;
	Record *layer = get_item(get_record(document->record, "Z"), index);
	Record *data = lift_pixels(document, layer, FALSE, error);
	if (!data)
		return FALSE;
	RasterExtent result;
	Record *pixels = resample_projective(get_record(data, "Sl"), read_extent(get_record(data, "xX")), matrix, &result);
	if (!pixels) {
		free_record(data);
		return FALSE;
	}
	trim_pixels(&pixels, &result);
	Record *mask = create_channels((gsize)result.width * result.height, 8);
	for (gsize at = 0; at < (gsize)result.width * result.height; at++)
		((guint8 *)mask->value.buffer.data)[at] = ((guint8 *)pixels->value.buffer.data)[at * 4 + 3];
	set_record(data, "Sl", pixels);
	set_record(data, "xX", create_extent(result.x, result.y, result.width, result.height));
	Record *selection = create_object(RECORD_PLAIN);
	set_record(selection, "C", mask);
	set_record(selection, "V", create_extent(result.x, result.y, result.width, result.height));
	set_record(data, "xO", selection);
	set_number(data, "z", index);
	append_move(document, data, 10, 16, extended_history);
	compose_floating(document, data, NULL);
	return TRUE;
}

static gboolean apply_to_selection(Document *document, Projective matrix, gboolean extended_history) {
	Record *previous = get_document_selection(document);
	if (!previous)
		return FALSE;
	RasterExtent extent = read_extent(get_record(previous, "V")), result;
	Record *mask = get_record(previous, "C"), *alpha = create_channels((gsize)extent.width * extent.height * 4, 8);
	for (gsize at = 0; at < (gsize)extent.width * extent.height; at++)
		((guint8 *)alpha->value.buffer.data)[at * 4 + 3] = read_channel(mask, at);
	Record *pixels = resample_projective(alpha, extent, matrix, &result);
	free_record(alpha);
	if (!pixels)
		return FALSE;
	Record *values = create_channels((gsize)result.width * result.height, 8);
	for (gsize at = 0; at < (gsize)result.width * result.height; at++)
		((guint8 *)values->value.buffer.data)[at] = ((guint8 *)pixels->value.buffer.data)[at * 4 + 3];
	free_record(pixels);
	Record *selection = create_object(RECORD_PLAIN);
	set_record(selection, "C", values);
	set_record(selection, "V", create_extent(result.x, result.y, result.width, result.height));
	return record_selection(document, selection, 7, 10, extended_history);
}

gboolean apply_transform(Document *document, TransformMode mode, const double quad[8], gboolean extended_history,
						 GError **error) {
	RasterExtent bounds = get_transform_bounds(document, mode);
	if (empty_extent(bounds) || !is_convex_quad(quad))
		return FALSE;
	Projective matrix;
	if (!projective_from_rectangle(quad, bounds.x, bounds.y, bounds.width, bounds.height, &matrix))
		return FALSE;
	set_boolean(document->record, "di", TRUE);
	if (mode == TRANSFORM_SELECTION)
		return apply_to_selection(document, matrix, extended_history);
	if (mode == TRANSFORM_PIXELS)
		return apply_to_pixels(document, matrix, extended_history, error);
	return apply_to_layers(document, matrix, extended_history);
}
