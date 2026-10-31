#include <limits.h>
#include <math.h>
#include <string.h>

#include "document/history.h"
#include "document/layer-commands.h"
#include "move-tool.h"
#include "selection/pixel-selection.h"
#include "transform.h"
#include "lang/en.h"

static const char *TRANSFORM_OPERATION = "100";

/* Values closer than these to zero count as zero */
#define NEAR_ZERO 1e-9
#define FLAT_PROJECTIVE 1e-18
/* Snapping a uniform corner drag onto the diagonal stops after this many tries */
#define DIAGONAL_PASSES 16
/* A scale of zero would flatten the box for good, so it becomes this small instead */
#define SMALLEST_SCALE 1e-4

/* The bounds of a resampled image are found from points along its edges, every tenth of the way */
#define EDGE_POINTS_STEP .1
#define EDGE_POINTS_VALUES 96
/* Results larger than this on a side are refused */
#define LARGEST_RESULT 100000
/* Bilinear sampling is used up to this footprint, and larger ones average a grid of samples instead */
#define BILINEAR_FOOTPRINT 1.1
#define SAMPLES_PER_FOOTPRINT 2.3
/* Pixels this close to the source edge always get at least this many samples a side */
#define EDGE_MARGIN .6
#define EDGE_SAMPLES 5
/* Pixel centers move just under half a pixel, so a center on a pixel border rounds down */
#define SAMPLE_CENTER_SHIFT .499999
/* Halving stops at this many pixels, or once the scale is above this */
#define SMALLEST_LEVEL_PIXELS 16
#define LEVEL_SCALE_LIMIT .3
/* A flat box gets this much room on each side, so it can still be grabbed */
#define FLAT_BOX_MARGIN 10
/* The sharper resampling runs this three by three kernel after scaling */
#define KERNEL_SIZE 3
static const double SHARPEN_KERNEL[KERNEL_SIZE * KERNEL_SIZE] = {
	0, -1.0 / 12, 0, -1.0 / 12, 16.0 / 12, -1.0 / 12, 0, -1.0 / 12, 0};

/* A sample word holds a pixel's four bytes in memory order, so red is the lowest byte and alpha the highest */

static guint word_channel(guint32 word, int channel) {
	return word >> channel * CHAR_BIT & G_MAXUINT8;
}

static guint32 make_word(guint red, guint green, guint blue, guint alpha) {
	return (guint32)alpha << WORD_ALPHA_SHIFT | (guint32)blue << 2 * CHAR_BIT | (guint32)green << CHAR_BIT | red;
}

/* ============================== Projective math ============================== */

/* Solves the eight unknowns of a projective matrix from the system with its answers in the last column */
static gboolean solve_system(double system[PROJECTIVE_VALUES][PROJECTIVE_VALUES + 1], double result[PROJECTIVE_VALUES]) {
	for (int column = 0; column < PROJECTIVE_VALUES; column++) {
		int pivot = column;
		for (int row = column; row < PROJECTIVE_VALUES; row++)
			if (fabs(system[row][column]) > fabs(system[pivot][column]))
				pivot = row;
		if (pivot != column)
			for (int index = 0; index <= PROJECTIVE_VALUES; index++) {
				double swap = system[column][index];
				system[column][index] = system[pivot][index];
				system[pivot][index] = swap;
			}
		if (system[column][column] == 0)
			return FALSE;
		for (int row = column + 1; row < PROJECTIVE_VALUES; row++) {
			double factor = system[row][column] / system[column][column];
			for (int index = column; index <= PROJECTIVE_VALUES; index++)
				system[row][index] -= system[column][index] * factor;
		}
	}
	for (int row = PROJECTIVE_VALUES - 1; row >= 0; row--) {
		if (system[row][row] == 0)
			return FALSE;
		result[row] = system[row][PROJECTIVE_VALUES] / system[row][row];
		for (int above = row - 1; above >= 0; above--) {
			system[above][PROJECTIVE_VALUES] -= system[above][row] * result[row];
			system[above][row] = 0;
		}
	}
	return TRUE;
}

gboolean projective_from_rectangle(const double quad[QUAD_VALUES], double x, double y, double width, double height,
								   Projective *result) {
	const double corners[QUAD_VALUES] = {x, y, x + width, y, x + width, y + height, x, y + height};
	double system[PROJECTIVE_VALUES][PROJECTIVE_VALUES + 1];
	for (int point = 0; point < QUAD_CORNERS; point++) {
		double from_x = corners[point * 2], from_y = corners[point * 2 + 1];
		double to_x = quad[point * 2], to_y = quad[point * 2 + 1];
		const double first[PROJECTIVE_VALUES + 1] = {from_x, from_y, 1, 0, 0, 0, -from_x * to_x, -from_y * to_x, to_x};
		const double second[PROJECTIVE_VALUES + 1] = {0, 0, 0, from_x, from_y, 1, -from_x * to_y, -from_y * to_y, to_y};
		memcpy(system[point * 2], first, sizeof first);
		memcpy(system[point * 2 + 1], second, sizeof second);
	}
	return solve_system(system, result->m);
}

void get_extent_quad(RasterExtent extent, double quad[QUAD_VALUES]) {
	double right = extent.x + extent.width, bottom = extent.y + extent.height;
	const double corners[QUAD_VALUES] = {extent.x, extent.y, right, extent.y, right, bottom, extent.x, bottom};
	memcpy(quad, corners, sizeof corners);
}

Projective get_quad_matrix(const double quad[QUAD_VALUES]) {
	Projective matrix = IDENTITY_PROJECTIVE;
	projective_from_rectangle(quad, 0, 0, 1, 1, &matrix);
	return matrix;
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
	double result[PROJECTIVE_VALUES] = {a[0] * b[0] + a[1] * b[3] + a[2] * b[6], a[0] * b[1] + a[1] * b[4] + a[2] * b[7],
						a[0] * b[2] + a[1] * b[5] + a[2],		 a[3] * b[0] + a[4] * b[3] + a[5] * b[6],
						a[3] * b[1] + a[4] * b[4] + a[5] * b[7], a[3] * b[2] + a[4] * b[5] + a[5],
						a[6] * b[0] + a[7] * b[3] + b[6],		 a[6] * b[1] + a[7] * b[4] + b[7]};
	double scale = 1 / (a[6] * b[2] + a[7] * b[5] + 1);
	Projective product;
	for (int index = 0; index < PROJECTIVE_VALUES; index++)
		product.m[index] = result[index] * scale;
	return product;
}

void apply_projective(Projective matrix, double x, double y, double *result_x, double *result_y) {
	const double *m = matrix.m;
	double scale = 1 / (m[6] * x + m[7] * y + 1);
	*result_x = (m[0] * x + m[1] * y + m[2]) * scale;
	*result_y = (m[3] * x + m[4] * y + m[5]) * scale;
}

gboolean is_convex_quad(const double quad[QUAD_VALUES]) {
	int positive = 0, negative = 0;
	for (int point = 0; point < QUAD_CORNERS; point++) {
		const double *a = quad + point * 2, *b = quad + (point + 1) % QUAD_CORNERS * 2;
		const double *c = quad + (point + 2) % QUAD_CORNERS * 2;
		double turn = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]);
		if (turn > 0)
			positive++;
		if (turn < 0)
			negative++;
	}
	return positive == QUAD_CORNERS || negative == QUAD_CORNERS;
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

gboolean is_corner_handle(int handle) {
	return handle == HANDLE_TOP_LEFT || handle == HANDLE_TOP_RIGHT || handle == HANDLE_BOTTOM_LEFT ||
		   handle == HANDLE_BOTTOM_RIGHT;
}

int get_handle_corner(int handle) {
	return handle == HANDLE_TOP_RIGHT ? 1 : handle == HANDLE_BOTTOM_RIGHT ? 2 : handle == HANDLE_BOTTOM_LEFT ? 3 : 0;
}

static void to_box_unit(const double quad[QUAD_VALUES], double x, double y, double *u, double *v) {
	apply_projective(invert_projective(get_quad_matrix(quad)), x, y, u, v);
}

static void transform_box(const double source[QUAD_VALUES], Projective unit_change, double target[QUAD_VALUES]) {
	Projective box = get_quad_matrix(source);
	const double corners[QUAD_VALUES] = {0, 0, 1, 0, 1, 1, 0, 1};
	for (int point = 0; point < QUAD_CORNERS; point++) {
		double u, v;
		apply_projective(unit_change, corners[point * 2], corners[point * 2 + 1], &u, &v);
		apply_projective(box, u, v, &target[point * 2], &target[point * 2 + 1]);
	}
}

gboolean is_axis_aligned_quad(const double quad[QUAD_VALUES]) {
	return (fabs(quad[1] - quad[3]) < SAME_POINT_DISTANCE && fabs(quad[2] - quad[4]) < SAME_POINT_DISTANCE) ||
		   (fabs(quad[0] - quad[2]) < SAME_POINT_DISTANCE && fabs(quad[3] - quad[5]) < SAME_POINT_DISTANCE);
}

static void scale_around_reference(const BoxDrag *drag, double x, double y, gboolean uniform,
								   double next[QUAD_VALUES]) {
	double across = drag->handle_x - drag->reference_x, down = drag->handle_y - drag->reference_y;
	double scale_x = fabs(across) >= 1 ? (x - drag->reference_x) / across : 1;
	double scale_y = fabs(down) >= 1 ? (y - drag->reference_y) / down : 1;
	if (uniform)
		scale_x = scale_y = (scale_x + scale_y) / 2;
	for (int point = 0; point < QUAD_CORNERS; point++) {
		next[point * 2] = drag->reference_x + (drag->start[point * 2] - drag->reference_x) * scale_x;
		next[point * 2 + 1] = drag->reference_y + (drag->start[point * 2 + 1] - drag->reference_y) * scale_y;
	}
}

static void skew_box_edge(const BoxDrag *drag, double x, double y, gboolean shift, gboolean alt,
						  double next[QUAD_VALUES]) {
	/* The corner each edge starts from, going clockwise */
	const int first_corners[HANDLE_GRID_COUNT] = {
		[HANDLE_TOP] = 0, [HANDLE_RIGHT] = 1, [HANDLE_BOTTOM] = 2, [HANDLE_LEFT] = 3};
	const double *start = drag->start;
	int first = first_corners[drag->handle], second = (first + 1) % QUAD_CORNERS;
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
	for (int point = 0; point < QUAD_CORNERS; point++) {
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
	for (int pass = 0; pass < DIAGONAL_PASSES; pass++) {
		double offset = meet_line(*x, *y, -direction_y, direction_x, anchor_x, anchor_y, direction_x, direction_y);
		double projected_x = *x - direction_y * offset, projected_y = *y + direction_x * offset;
		if (drag->snap)
			drag->snap(drag->snap_data, &projected_x, &projected_y);
		if (hypot(projected_x - *x, projected_y - *y) < SAME_POINT_DISTANCE)
			return;
		*x = projected_x;
		*y = projected_y;
	}
}

static double get_unit_scale(double now, double was, double anchor, gboolean clamps) {
	double scale = fabs(was - anchor) > NEAR_ZERO ? (now - anchor) / (was - anchor) : 1;
	if (scale == 0)
		scale = SMALLEST_SCALE;
	return clamps && !(scale > 0) ? 0 : scale;
}

static void scale_in_unit_square(const BoxDrag *drag, const double current[QUAD_VALUES], double x, double y,
								 gboolean shift, gboolean alt, gboolean control, double next[QUAD_VALUES]) {
	const double *start = drag->start;
	int handle = drag->handle;
	gboolean corner = is_corner_handle(handle);
	gboolean uniform = shift || drag->keeps_ratio;
	if (is_axis_aligned_quad(current)) {
		x = round_half_up(x);
		y = round_half_up(y);
	}
	/* The grid is symmetric, so the opposite handle counts back from the last one */
	int opposite = HANDLE_GRID_COUNT - 1 - handle;
	double anchor_u = (opposite % HANDLE_GRID_SIZE) / 2.0, anchor_v = (opposite / HANDLE_GRID_SIZE) / 2.0;
	int anchor_point = get_handle_corner(opposite);
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
	/* Control with Shift on an edge keeps its own axis unscaled */
	gboolean fixed = control && shift, both = uniform && !fixed;
	if (corner) {
		scale_x = scale_u;
		scale_y = uniform ? scale_u : scale_v;
	} else if (handle == HANDLE_TOP || handle == HANDLE_BOTTOM) {
		scale_x = both ? scale_v : 1;
		scale_y = fixed ? 1 : scale_v;
	} else if (handle == HANDLE_LEFT || handle == HANDLE_RIGHT) {
		scale_x = fixed ? 1 : scale_u;
		scale_y = both ? scale_u : 1;
	}
	Projective change = {{scale_x, 0, anchor_u - scale_x * anchor_u, 0, scale_y, anchor_v - scale_y * anchor_v, 0, 0}};
	transform_box(start, change, next);
}

void scale_box(const BoxDrag *drag, const double current[QUAD_VALUES], double x, double y, gboolean shift, gboolean alt,
			   gboolean control, double next[QUAD_VALUES]) {
	gboolean corner = is_corner_handle(drag->handle);
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

static guint32 pack(const guint8 *pixel) {
	guint32 word = 0;
	for (int channel = 0; channel < RGBA_CHANNELS; channel++)
		word |= (guint32)pixel[channel] << channel * CHAR_BIT;
	return word;
}

static guint32 sample_bilinear(double x, double y, const guint8 *source, int width, int height, gboolean clamp) {
	x -= SAMPLE_CENTER_SHIFT;
	y -= SAMPLE_CENTER_SHIFT;
	int left = floor(x), top = floor(y);
	guint32 corners[QUAD_CORNERS] = {0};
	for (int corner = 0; corner < QUAD_CORNERS; corner++) {
		int column = left + (corner & 1), row = top + (corner >> 1);
		if (clamp) {
			column = CLAMP(column, 0, width - 1);
			row = CLAMP(row, 0, height - 1);
		} else if (column < 0 || column >= width || row < 0 || row >= height)
			continue;
		corners[corner] = pack(source + ((gsize)row * width + column) * RGBA_CHANNELS);
	}
	double across = x - left, down = y - top;
	double weights[QUAD_CORNERS] = {(1 - down) * (1 - across), (1 - down) * across, down * (1 - across),
									down * across};
	double alpha = 0, red = 0, green = 0, blue = 0;
	for (int corner = 0; corner < QUAD_CORNERS; corner++) {
		double weight = weights[corner] * word_channel(corners[corner], ALPHA_CHANNEL);
		alpha += weight;
		red += weight * word_channel(corners[corner], 0);
		green += weight * word_channel(corners[corner], 1);
		blue += weight * word_channel(corners[corner], 2);
	}
	if (alpha == 0)
		return 0;
	double scale = 1 / alpha;
	return make_word(red * scale + .5, green * scale + .5, blue * scale + .5, alpha + .5);
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
	double outline[EDGE_POINTS_VALUES] = {0, 0, 1, 0, 1, 1, 0, 1};
	int count = QUAD_VALUES;
	/* The steps stop before the far corner, which the outline already holds */
	for (double step = EDGE_POINTS_STEP; step < 1 - EDGE_POINTS_STEP / 2 && count + QUAD_VALUES <= EDGE_POINTS_VALUES;
		 step += EDGE_POINTS_STEP) {
		const double points[QUAD_VALUES] = {0, step, step, 0, 1, step, step, 1};
		memcpy(outline + count, points, sizeof points);
		count += QUAD_VALUES;
	}
	for (int index = 0; index < count; index += 2) {
		double x, y;
		apply_projective(unit, outline[index], outline[index + 1], &x, &y);
		left = MIN(left, x);
		top = MIN(top, y);
		right = MAX(right, x);
		bottom = MAX(bottom, y);
	}
	int x0 = (int)floor(left + NEAR_ZERO), y0 = (int)floor(top + NEAR_ZERO);
	int x1 = (int)ceil(right - NEAR_ZERO), y1 = (int)ceil(bottom - NEAR_ZERO);
	*result = (RasterExtent){x0, y0, x1 - x0, y1 - y0};
	if (result->width <= 0 || result->height <= 0 || result->width > LARGEST_RESULT || result->height > LARGEST_RESULT)
		return NULL;
	Record *output = create_channels((gsize)result->width * result->height * RGBA_CHANNELS, BYTE_DEPTH);
	const guint8 *source = pixels->value.buffer.data;
	guint8 *target = output->value.buffer.data;
	int width = extent.width, height = extent.height;
	gboolean affine = fabs(unit.m[6]) < NEAR_ZERO && fabs(unit.m[7]) < NEAR_ZERO;
	gboolean whole = TRUE;
	double size = 0;
	for (int index = 0; index < PROJECTIVE_VALUES; index++) {
		double rounded = round_half_up(unit.m[index]);
		/* The third and sixth values move rather than scale */
		if (index != 2 && index != 5)
			size += fabs(rounded);
		if (fabs(rounded - unit.m[index]) > NEAR_ZERO)
			whole = FALSE;
	}
	if (nearest || (whole && size == width + height && affine)) {
		draw_nearest(inverse, (const guint32 *)source, width, height, *result, affine, (guint32 *)target);
		return output;
	}
	gboolean aligned = affine && ((fabs(unit.m[0]) < NEAR_ZERO && fabs(unit.m[4]) < NEAR_ZERO) ||
								  (fabs(unit.m[1]) < NEAR_ZERO && fabs(unit.m[3]) < NEAR_ZERO));
	for (int row = 0; row < result->height; row++)
		for (int column = 0; column < result->width; column++) {
			double x = column + result->x + .5, y = row + result->y + .5, u, v;
			apply_projective(inverse, x, y, &u, &v);
			double source_x = u * width, source_y = v * height;
			if (source_x < -1 || source_x > width + 1 || source_y < -1 || source_y > height + 1)
				continue;
			guint32 pixel;
			double footprint = measure_footprint(unit, width, height, x, y, u, v);
			int samples = round(footprint * SAMPLES_PER_FOOTPRINT);
			if (footprint < BILINEAR_FOOTPRINT)
				pixel = sample_bilinear(source_x, source_y, source, width, height, aligned);
			else {
				if (source_x < EDGE_MARGIN || source_x > width - EDGE_MARGIN || source_y < EDGE_MARGIN ||
					source_y > height - EDGE_MARGIN)
					samples = MAX(samples, EDGE_SAMPLES);
				double step = 1.0 / samples, alpha = 0, red = 0, green = 0, blue = 0;
				for (int across = 0; across < samples; across++)
					for (int down = 0; down < samples; down++) {
						double sample_u, sample_v;
						apply_projective(inverse, column + result->x + (across + .5) * step,
										 row + result->y + (down + .5) * step, &sample_u, &sample_v);
						if (sample_u < 0 || sample_u >= 1 || sample_v < 0 || sample_v >= 1)
							continue;
						gsize at = (gsize)floor(sample_v * height) * width + (gsize)floor(sample_u * width);
						guint32 value = pack(source + at * RGBA_CHANNELS);
						double weight = word_channel(value, ALPHA_CHANNEL);
						red += word_channel(value, 0) * weight;
						green += word_channel(value, 1) * weight;
						blue += word_channel(value, 2) * weight;
						alpha += weight;
					}
				if (alpha == 0)
					continue;
				double scale = 1 / alpha;
				pixel = make_word(red * scale + .5, green * scale + .5, blue * scale + .5, alpha * step * step + .5);
			}
			guint8 *out = target + ((gsize)row * result->width + column) * RGBA_CHANNELS;
			for (int channel = 0; channel < RGBA_CHANNELS; channel++)
				out[channel] = word_channel(pixel, channel);
		}
	return output;
}

static Record *halve_level(const Record *pixels, RasterExtent extent, RasterExtent *half) {
	int width = extent.width, columns = width >> 1, rows = extent.height >> 1;
	*half = (RasterExtent){(int)((guint32)extent.x >> 1), (int)((guint32)extent.y >> 1), columns, rows};
	Record *output = create_channels((gsize)columns * rows * RGBA_CHANNELS, BYTE_DEPTH);
	const guint32 *source = pixels->value.buffer.data;
	guint32 *target = output->value.buffer.data;
	for (int row = 0; row < rows; row++)
		for (int column = 0; column < columns; column++) {
			const guint32 *top = source + (gsize)(row << 1) * width + (column << 1);
			const guint32 corners[QUAD_CORNERS] = {top[0], top[1], top[width], top[width + 1]};
			guint total = 0, sums[RGB_CHANNELS] = {0}, weighted[RGB_CHANNELS] = {0};
			for (int corner = 0; corner < QUAD_CORNERS; corner++) {
				guint alpha = word_channel(corners[corner], ALPHA_CHANNEL);
				total += alpha;
				for (int channel = 0; channel < RGB_CHANNELS; channel++) {
					guint value = word_channel(corners[corner], channel);
					sums[channel] += value;
					weighted[channel] += value * alpha;
				}
			}
			/* Shifting by two divides by the four corners, and adding two first rounds the result */
			guint32 *out = target + (gsize)row * columns + column;
			if (total == QUAD_CORNERS * G_MAXUINT8)
				*out = make_word((sums[0] + 2) >> 2, (sums[1] + 2) >> 2, (sums[2] + 2) >> 2, G_MAXUINT8);
			else if (total == 0)
				*out = 0;
			else {
				double scale = 1.0 / total;
				guint red = (guint)(weighted[0] * scale + .5), green = (guint)(weighted[1] * scale + .5);
				guint blue = (guint)(weighted[2] * scale + .5);
				*out = make_word(red, green, blue, (total + 2) >> 2);
			}
		}
	return output;
}

static void sharpen_pixels(Record *pixels, RasterExtent extent) {
	g_autoptr(Record) copy = copy_record(pixels);
	const guint8 *source = copy->value.buffer.data;
	guint8 *target = pixels->value.buffer.data;
	int width = extent.width, height = extent.height;
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++) {
			double sums[RGBA_CHANNELS] = {0};
			for (int index = 0; index < KERNEL_SIZE * KERNEL_SIZE; index++) {
				if (SHARPEN_KERNEL[index] == 0)
					continue;
				int column = CLAMP(x - 1 + index % KERNEL_SIZE, 0, width - 1);
				int row = CLAMP(y - 1 + index / KERNEL_SIZE, 0, height - 1);
				for (int channel = 0; channel < RGBA_CHANNELS; channel++)
					sums[channel] +=
						source[((gsize)row * width + column) * RGBA_CHANNELS + channel] * SHARPEN_KERNEL[index];
			}
			for (int channel = 0; channel < RGBA_CHANNELS; channel++)
				target[((gsize)y * width + x) * RGBA_CHANNELS + channel] =
					CLAMP((int)(.5 + sums[channel]), 0, G_MAXUINT8);
		}
}

Record *resample_image(const Record *pixels, RasterExtent extent, Projective matrix, ResampleMode mode,
					   RasterExtent *result) {
	if (empty_extent(extent))
		return NULL;
	const double *g = matrix.m;
	g_autoptr(Record) level = reference_record((Record *)pixels);
	RasterExtent level_extent = extent;
	if (fabs(g[6]) < FLAT_PROJECTIVE && fabs(g[7]) < FLAT_PROJECTIVE) {
		double scale = sqrt(fabs(g[0] * g[4] - g[3] * g[1]));
		for (int step = 0; level_extent.width >= 2 && level_extent.height >= 2; step++) {
			RasterExtent half = {0, 0, level_extent.width >> 1, level_extent.height >> 1};
			if ((gsize)half.width * half.height < SMALLEST_LEVEL_PIXELS || scale * (1 << step) >= LEVEL_SCALE_LIMIT)
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
	return !is_layer_group(layer) && get_number(get_record(layer, "add"), "lsct") == SECTION_LAYER;
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
			if (!read_channel(buffer, ((gsize)y * extent.width + x) * RGBA_CHANNELS + ALPHA_CHANNEL))
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
		bounds.x -= FLAT_BOX_MARGIN;
		bounds.width += 2 * FLAT_BOX_MARGIN;
	}
	if (bounds.height == 0) {
		bounds.y -= FLAT_BOX_MARGIN;
		bounds.height += 2 * FLAT_BOX_MARGIN;
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
	Record *trimmed = create_channels((gsize)MAX(0, bounds.width) * MAX(0, bounds.height) * RGBA_CHANNELS, BYTE_DEPTH);
	for (int y = 0; y < bounds.height; y++)
		memcpy((guint8 *)trimmed->value.buffer.data + (gsize)y * bounds.width * RGBA_CHANNELS,
			   (guint8 *)(*pixels)->value.buffer.data +
				   ((gsize)(bounds.y - extent->y + y) * extent->width + bounds.x - extent->x) * RGBA_CHANNELS,
			   (gsize)bounds.width * RGBA_CHANNELS);
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
			pixels = create_channels(0, BYTE_DEPTH);
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
		create_history_entry(create_history_name(HISTORY_TRANSFORM), TRANSFORM_OPERATION, FALSE, g_get_real_time() / G_TIME_SPAN_MILLISECOND);
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
	Record *mask = create_channels((gsize)result.width * result.height, BYTE_DEPTH);
	for (gsize at = 0; at < (gsize)result.width * result.height; at++)
		((guint8 *)mask->value.buffer.data)[at] = ((guint8 *)pixels->value.buffer.data)[at * RGBA_CHANNELS + ALPHA_CHANNEL];
	set_record(data, "Sl", pixels);
	set_record(data, "xX", create_extent(result.x, result.y, result.width, result.height));
	Record *selection = create_object(RECORD_PLAIN);
	set_record(selection, "C", mask);
	set_record(selection, "V", create_extent(result.x, result.y, result.width, result.height));
	set_record(data, "xO", selection);
	set_number(data, "z", index);
	append_move(document, data, HISTORY_TRANSFORM, extended_history);
	compose_floating(document, data, NULL);
	return TRUE;
}

static gboolean apply_to_selection(Document *document, Projective matrix, gboolean extended_history) {
	Record *previous = get_document_selection(document);
	if (!previous)
		return FALSE;
	RasterExtent extent = read_extent(get_record(previous, "V")), result;
	Record *mask = get_record(previous, "C");
	Record *alpha = create_channels((gsize)extent.width * extent.height * RGBA_CHANNELS, BYTE_DEPTH);
	for (gsize at = 0; at < (gsize)extent.width * extent.height; at++)
		((guint8 *)alpha->value.buffer.data)[at * RGBA_CHANNELS + ALPHA_CHANNEL] = read_channel(mask, at);
	Record *pixels = resample_projective(alpha, extent, matrix, &result);
	free_record(alpha);
	if (!pixels)
		return FALSE;
	Record *values = create_channels((gsize)result.width * result.height, BYTE_DEPTH);
	for (gsize at = 0; at < (gsize)result.width * result.height; at++)
		((guint8 *)values->value.buffer.data)[at] =
			((guint8 *)pixels->value.buffer.data)[at * RGBA_CHANNELS + ALPHA_CHANNEL];
	free_record(pixels);
	Record *selection = create_object(RECORD_PLAIN);
	set_record(selection, "C", values);
	set_record(selection, "V", create_extent(result.x, result.y, result.width, result.height));
	return record_selection(document, selection, HISTORY_TRANSFORM_SELECTION, extended_history);
}

gboolean apply_transform(Document *document, TransformMode mode, const double quad[QUAD_VALUES], gboolean extended_history,
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
