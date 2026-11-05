#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stroke.h"

/* Tips under this diameter are drawn this much larger and scaled down, which keeps them round */
#define SMALL_TIP_DIAMETER 100
#define SMALL_TIP_SCALE 1.4
/* Pen pressure is kept between these */
#define LIGHTEST_PRESSURE .05
#define HEAVIEST_PRESSURE 5
/* Each dab dithers its alpha from a hash of its position, seeded once per dab from this range */
#define DITHER_SEED_RANGE 16777215
/* Hard tips this small or smaller are plain squares */
#define LARGEST_HARD_SQUARE 3
/* Dabs flattened past this scale are left out */
#define SMALLEST_DAB_SCALE 1e-4
/* Roundness never goes below this percent */
#define SMALLEST_ROUNDNESS 5
/* Half hard tips and harder keep their scallops small, and no step is shorter than half a pixel */
#define SMOOTH_HARDNESS 50
#define SHORTEST_STEP .5
/* Pixel lines only move once the pointer has moved almost a pixel */
#define PIXEL_LINE_DISTANCE .99
/* Curves bend toward their neighbours by a share between these, growing with the angle between them, and are drawn
 * in this many steps */
#define SMALLEST_HANDLE_SHARE .1
#define LARGEST_HANDLE_SHARE .35
#define CURVE_STEPS 10
/* Points are kept as x and y pairs */
#define POINT_VALUES 2

double get_default_random(gpointer data) {
	return g_random_double();
}

static void interleave_alpha(const Record *alpha, Record *rgba) {
	gsize count = MIN(rgba->value.buffer.length / RGBA_CHANNELS, alpha->value.buffer.length);
	guint8 *target = rgba->value.buffer.data;
	const guint8 *source = alpha->value.buffer.data;
	for (gsize index = 0; index < count; index++)
		target[index * RGBA_CHANNELS + ALPHA_CHANNEL] = source[index];
}

static void prepare_stroke_tip(Stroke *stroke) {
	stroke->tip_scale = stroke->shape.diameter < SMALL_TIP_DIAMETER ? SMALL_TIP_SCALE : 1;
	stroke->tip_ratio = 1;
	BrushLevel tip =
		create_computed_tip(stroke->shape.diameter, from_percent(stroke->shape.hardness), stroke->tip_scale);
	stroke->tip_levels = g_array_new(FALSE, FALSE, sizeof(BrushLevel));
	g_array_append_val(stroke->tip_levels, tip);
	gsize area = (gsize)tip.extent.width * tip.extent.height;
	stroke->tip_rgba = create_channels(area * RGBA_CHANNELS, BYTE_DEPTH);
	interleave_alpha(tip.values, stroke->tip_rgba);
}

Stroke *create_stroke(BrushShape shape, gboolean hard, double flow, double smoothing, guint32 color,
					  RasterExtent extent, RandomSource random, gpointer random_data) {
	Stroke *stroke = g_new0(Stroke, 1);
	stroke->shape = shape;
	stroke->hard = hard;
	stroke->flow = flow;
	stroke->smoothing = smoothing;
	stroke->color = color;
	stroke->extent = extent;
	stroke->random = random ? random : get_default_random;
	stroke->random_data = random_data;
	stroke->random(stroke->random_data);
	stroke->random(stroke->random_data);
	stroke->coverage = create_channels((gsize)extent.width * extent.height * RGBA_CHANNELS, BYTE_DEPTH);
	stroke->pending_low = g_new(int, MAX(1, extent.height));
	stroke->pending_high = g_new(int, MAX(1, extent.height));
	for (int row = 0; row < extent.height; row++)
		stroke->pending_low[row] = G_MAXINT;
	stroke->pending_top = G_MAXINT;
	stroke->pending_bottom = G_MININT;
	stroke->points = g_array_new(FALSE, FALSE, sizeof(double));
	stroke->pressures = g_array_new(FALSE, FALSE, sizeof(double));
	prepare_stroke_tip(stroke);
	return stroke;
}

void free_stroke(Stroke *stroke) {
	if (!stroke)
		return;
	free_record(stroke->coverage);
	free_record(stroke->tip_rgba);
	free_brush_levels(stroke->tip_levels);
	g_array_unref(stroke->points);
	g_array_unref(stroke->pressures);
	g_free(stroke->pending_low);
	g_free(stroke->pending_high);
	g_free(stroke);
}

static double point_value(double value) {
	return isnan(value) || value == 0 ? 0 : value;
}

static void record_stroke_extent(Stroke *stroke, RasterExtent extent) {
	stroke->dirty = extent;
	stroke->total = union_region(stroke->total, extent);
}

/* Every coverage writer marks the columns it changed in a row, so each change reaches the layer exactly once */
static void mark_stroke_row(Stroke *stroke, int row, int low, int high) {
	RasterExtent area = stroke->extent;
	row -= area.y;
	low = MAX(low, area.x) - area.x;
	high = MIN(high, area.x + area.width - 1) - area.x;
	if (row < 0 || row >= area.height || low > high)
		return;
	if (stroke->pending_low[row] == G_MAXINT) {
		stroke->pending_low[row] = low;
		stroke->pending_high[row] = high;
	} else {
		stroke->pending_low[row] = MIN(stroke->pending_low[row], low);
		stroke->pending_high[row] = MAX(stroke->pending_high[row], high);
	}
	stroke->pending_top = MIN(stroke->pending_top, row);
	stroke->pending_bottom = MAX(stroke->pending_bottom, row);
}

static void mark_stroke_extent(Stroke *stroke, RasterExtent extent) {
	if (empty_extent(extent))
		return;
	for (int row = extent.y; row < extent.y + extent.height; row++)
		mark_stroke_row(stroke, row, extent.x, extent.x + extent.width - 1);
}

static double clamp_pressure(double pressure) {
	if (isnan(pressure))
		pressure = 1;
	return MAX(LIGHTEST_PRESSURE, MIN(HEAVIEST_PRESSURE, pressure));
}

static gboolean touches_region(RasterExtent first, RasterExtent second) {
	return !(second.y + second.height < first.y || second.x > first.x + first.width ||
			 second.y > first.y + first.height || second.x + second.width < first.x);
}

/* The colour bytes of two words mixed by their weights over their combined weight, each byte rounded */
static guint32 mix_colors(guint32 top, double top_weight, guint32 bottom, double bottom_weight, double reciprocal) {
	guint32 mixed = 0;
	for (int shift = WORD_RED_SHIFT; shift >= 0; shift -= CHAR_BIT) {
		double channel = floor(
			(((top >> shift) & G_MAXUINT8) * top_weight + ((bottom >> shift) & G_MAXUINT8) * bottom_weight) * reciprocal +
			.5);
		mixed |= ((guint32)channel & G_MAXUINT8) << shift;
	}
	return mixed;
}

static void blend_dab(Stroke *stroke, const Record *dab, RasterExtent dab_extent, double opacity) {
	RasterExtent clip = intersect_region(intersect_region(dab_extent, stroke->extent), dab_extent);
	int dab_x = MAX(0, clip.x - dab_extent.x), target_x = MAX(0, clip.x - stroke->extent.x);
	int dab_y = MAX(0, clip.y - dab_extent.y), target_y = MAX(0, clip.y - stroke->extent.y);
	const guint32 *source = dab->value.buffer.data;
	guint32 *target = stroke->coverage->value.buffer.data;
	gint32 seed = floor(stroke->random(stroke->random_data) * DITHER_SEED_RANGE);
	guint32 dither = 0;
	for (int row = 0; row < clip.height; row++) {
		gsize source_row = (gsize)(dab_y + row) * dab_extent.width + dab_x;
		gsize target_row = (gsize)(target_y + row) * stroke->extent.width + target_x;
		for (int column = 0; column < clip.width; column++) {
			guint32 pixel = source[source_row + column];
			if (pixel >> WORD_ALPHA_SHIFT == 0)
				continue;
			guint32 existing = target[target_row + column];
			double alpha = (pixel >> WORD_ALPHA_SHIFT) * opacity * BYTE_UNIT;
			double below = (existing >> WORD_ALPHA_SHIFT) * BYTE_UNIT;
			double remaining = below * (1 - alpha), combined = alpha + remaining;
			/* One hash gives the dither bytes of four pixels */
			dither >>= CHAR_BIT;
			if (column % sizeof(guint32) == 0)
				dither = hash_whole_number((gint64)(target_row + column) + seed);
			double reciprocal = combined == 0 ? 0 : 1 / combined;
			/* The alpha in 256ths of a level, with the dither deciding the rounding */
			guint32 result_alpha =
				((guint32)floor(combined * ((G_MAXUINT8 + 1) * G_MAXUINT8)) + (dither & G_MAXUINT8)) >> CHAR_BIT;
			target[target_row + column] =
				result_alpha << WORD_ALPHA_SHIFT | mix_colors(pixel, alpha, existing, remaining, reciprocal);
		}
	}
}

static void color_dab(Record *dab, guint32 color) {
	guint8 *bytes = dab->value.buffer.data;
	for (gsize index = 0; index + ALPHA_CHANNEL < dab->value.buffer.length; index += RGBA_CHANNELS) {
		bytes[index] = color >> WORD_RED_SHIFT & G_MAXUINT8;
		bytes[index + 1] = color >> WORD_GREEN_SHIFT & G_MAXUINT8;
		bytes[index + 2] = color & G_MAXUINT8;
	}
}

static void threshold_tip(Record *values) {
	guint8 *bytes = values->value.buffer.data;
	for (gsize index = 0; index < values->value.buffer.length; index++)
		bytes[index] = bytes[index] < G_MAXUINT8 / 2 ? 0 : G_MAXUINT8;
}

static BrushLevel shape_dab(Stroke *stroke, Affine transform, double x, double y, Record **rgba) {
	int size = round_half_up(stroke->shape.diameter);
	BrushLevel dab = {0};
	gboolean shared = FALSE;
	if (stroke->hard && size <= LARGEST_HARD_SQUARE) {
		dab.values = create_channels((gsize)size * size, BYTE_DEPTH);
		memset(dab.values->value.buffer.data, G_MAXUINT8, dab.values->value.buffer.length);
		dab.extent = (RasterExtent){round_half_up(x - size / 2.0), round_half_up(y - size / 2.0), size, size};
	} else if (is_translation_affine(transform)) {
		BrushLevel tip = g_array_index(stroke->tip_levels, BrushLevel, 0);
		dab.values = tip.values;
		dab.extent = (RasterExtent){round_half_up(transform.e), round_half_up(transform.f), tip.extent.width,
									tip.extent.height};
		shared = TRUE;
	} else if (sqrt(fabs(transform.a * transform.d - transform.b * transform.c)) > SMALLEST_DAB_SCALE)
		dab = resample_brush(stroke->tip_levels, transform);
	if (!dab.values)
		dab.values = create_channels(0, BYTE_DEPTH);
	if (shared && !stroke->hard) {
		*rgba = stroke->tip_rgba;
		dab.values = NULL;
		return dab;
	}
	if (stroke->hard)
		threshold_tip(dab.values);
	*rgba = create_channels((gsize)MAX(0, dab.extent.width) * MAX(0, dab.extent.height) * RGBA_CHANNELS, BYTE_DEPTH);
	interleave_alpha(dab.values, *rgba);
	if (shared)
		dab.values = NULL;
	return dab;
}

static RasterExtent draw_dab(Stroke *stroke, double x, double y, double diameter) {
	double opacity = MIN(1, stroke->flow);
	BrushLevel base = g_array_index(stroke->tip_levels, BrushLevel, 0);
	Affine transform = create_identity_affine();
	translate_affine(&transform, -base.extent.width / 2.0, -base.extent.height / 2.0);
	scale_affine(&transform, 1 / stroke->tip_scale, 1 / stroke->tip_scale);
	scale_affine(&transform, diameter / stroke->shape.diameter, diameter / stroke->shape.diameter);
	scale_affine(&transform, 1, from_percent(MAX(SMALLEST_ROUNDNESS, stroke->shape.roundness)));
	rotate_affine(&transform, 0 + stroke->shape.angle * (G_PI / (DEGREES_PER_TURN / 2)));
	translate_affine(&transform, x, y);
	Record *rgba = NULL;
	BrushLevel dab = shape_dab(stroke, transform, x, y, &rgba);
	RasterExtent extent = dab.extent;
	color_dab(rgba, stroke->color);
	if (empty_extent(stroke->total) && equal_region(stroke->extent, dab.extent) && opacity == 1) {
		memcpy(stroke->coverage->value.buffer.data, rgba->value.buffer.data,
			   MIN(rgba->value.buffer.length, stroke->coverage->value.buffer.length));
		mark_stroke_extent(stroke, dab.extent);
	} else if (touches_region(dab.extent, stroke->extent)) {
		extent = union_region(extent, dab.extent);
		blend_dab(stroke, rgba, dab.extent, opacity);
		mark_stroke_extent(stroke, dab.extent);
	}
	if (rgba != stroke->tip_rgba)
		free_record(rgba);
	free_record(dab.values);
	stroke->last_diameter = diameter;
	return extent;
}

/* A capsule is convex, so a row crosses it in one span: the hull of the row's crossings of both end circles and
 * of the band between them. Coordinates are relative to the segment start, with direction (dx, dy). */
static gboolean capsule_row_span(double radius, double row_y, double dx, double dy, double *low, double *high) {
	*low = G_MAXDOUBLE;
	*high = -G_MAXDOUBLE;
	const double ends[2][2] = {{0, 0}, {dx, dy}};
	for (int end = 0; end < 2; end++) {
		double offset = row_y - ends[end][1], squared = radius * radius - offset * offset;
		if (squared < 0)
			continue;
		double half = sqrt(squared);
		*low = MIN(*low, ends[end][0] - half);
		*high = MAX(*high, ends[end][0] + half);
	}
	double length = sqrt(dx * dx + dy * dy);
	if (length > 0) {
		/* Inside the band: |x * dy - row_y * dx| <= radius * length and 0 <= x * dx + row_y * dy <= length^2 */
		double band_low = -G_MAXDOUBLE, band_high = G_MAXDOUBLE;
		if (dy != 0) {
			double first = (row_y * dx - radius * length) / dy, second = (row_y * dx + radius * length) / dy;
			band_low = MAX(band_low, MIN(first, second));
			band_high = MIN(band_high, MAX(first, second));
		} else if (fabs(row_y * dx) > radius * length)
			band_high = band_low;
		if (dx != 0) {
			double first = -row_y * dy / dx, second = (length * length - row_y * dy) / dx;
			band_low = MAX(band_low, MIN(first, second));
			band_high = MIN(band_high, MAX(first, second));
		} else if (row_y * dy < 0 || row_y * dy > length * length)
			band_high = band_low;
		if (band_low < band_high) {
			*low = MIN(*low, band_low);
			*high = MAX(*high, band_high);
		}
	}
	return *low <= *high;
}

/* A fully hard round brush sweeps a capsule from the last point in one pass, giving exact
 * anti-aliased edges without stamping overlapping dabs. At full flow a pixel keeps its strongest coverage, so the
 * joints between pointer events stay smooth and pixels the last capsule already covered stay unchanged and unmarked.
 * Below full flow rim pixels blend over the stroke like a dab does. */
static RasterExtent sweep_capsule(Stroke *stroke, double x, double y) {
	double radius = stroke->shape.diameter / 2, start_x = stroke->last_x, start_y = stroke->last_y;
	double delta_x = x - start_x, delta_y = y - start_y, length_squared = delta_x * delta_x + delta_y * delta_y;
	int left = floor(MIN(start_x, x) - radius - 1), top = floor(MIN(start_y, y) - radius - 1);
	RasterExtent bounds = {left, top, (int)ceil(MAX(start_x, x) + radius + 1) - left,
						   (int)ceil(MAX(start_y, y) + radius + 1) - top};
	RasterExtent clip = intersect_region(bounds, stroke->extent);
	if (empty_extent(clip))
		return (RasterExtent){0};
	double opacity = MIN(1, stroke->flow), inner = MAX(0, radius - .5), outer = radius + .5;
	double reciprocal_length = length_squared > 0 ? 1 / length_squared : 0;
	/* Coverage pixels hold red, green and blue bytes in memory order, like the colored dabs */
	guint32 color = swap_red_and_blue(stroke->color);
	guint32 *target = stroke->coverage->value.buffer.data;
	guint32 solid = (guint32)(unit_to_byte(opacity) + .5) << WORD_ALPHA_SHIFT | color;
	RasterExtent changed = {0};
	for (int row = clip.y; row < clip.y + clip.height; row++) {
		double point_y = row + .5 - start_y, outer_low, outer_high, inner_low, inner_high;
		if (!capsule_row_span(outer, point_y, delta_x, delta_y, &outer_low, &outer_high))
			continue;
		gboolean filled = opacity >= 1 && capsule_row_span(inner, point_y, delta_x, delta_y, &inner_low, &inner_high);
		/* Pixel centers sit at column + 0.5 */
		int first = MAX(clip.x, (int)ceil(outer_low + start_x - .5));
		int last = MIN(clip.x + clip.width - 1, (int)floor(outer_high + start_x - .5));
		int solid_first = filled ? (int)ceil(inner_low + start_x - .5) : last + 1;
		int solid_last = filled ? (int)floor(inner_high + start_x - .5) : last;
		/* The previous capsule's end circle already holds solid coverage here */
		int skip_first = G_MAXINT, skip_last = G_MININT;
		double inside = inner * inner - point_y * point_y;
		if (filled && stroke->swept_solid && inside > 0) {
			skip_first = MAX(solid_first, (int)ceil(start_x - sqrt(inside) - .5));
			skip_last = MIN(solid_last, (int)floor(start_x + sqrt(inside) - .5));
		}
		guint32 *line = target + (gsize)(row - stroke->extent.y) * stroke->extent.width - stroke->extent.x;
		int changed_low = G_MAXINT, changed_high = G_MININT;
		for (int column = first; column <= last; column++) {
			if (column >= skip_first && column <= skip_last) {
				column = skip_last;
				continue;
			}
			if (column >= solid_first && column <= solid_last) {
				if (line[column] == solid)
					continue;
				line[column] = solid;
				changed_low = MIN(changed_low, column);
				changed_high = column;
				continue;
			}
			double point_x = column + .5 - start_x;
			double along = CLAMP((point_x * delta_x + point_y * delta_y) * reciprocal_length, 0, 1);
			double offset_x = point_x - along * delta_x, offset_y = point_y - along * delta_y;
			double alpha = CLAMP(outer - sqrt(offset_x * offset_x + offset_y * offset_y), 0, 1) * opacity;
			if (alpha <= 0)
				continue;
			guint32 existing = line[column];
			if (opacity >= 1) {
				guint32 strongest = (guint32)(unit_to_byte(alpha) + .5);
				if (strongest <= existing >> WORD_ALPHA_SHIFT)
					continue;
				line[column] = strongest << WORD_ALPHA_SHIFT | color;
				changed_low = MIN(changed_low, column);
				changed_high = column;
				continue;
			}
			double below = (existing >> WORD_ALPHA_SHIFT) * BYTE_UNIT, remaining = below * (1 - alpha),
				   combined = alpha + remaining;
			line[column] = (guint32)(unit_to_byte(combined) + .5) << WORD_ALPHA_SHIFT |
						   mix_colors(color, alpha, existing, remaining, 1 / combined);
			changed_low = MIN(changed_low, column);
			changed_high = column;
		}
		if (changed_low > changed_high)
			continue;
		mark_stroke_row(stroke, row, changed_low, changed_high);
		RasterExtent span = {changed_low, row, changed_high - changed_low + 1, 1};
		changed = empty_extent(changed) ? span : union_region(changed, span);
	}
	stroke->swept_solid = opacity >= 1;
	return changed;
}

static gboolean sweeps_capsules(const Stroke *stroke) {
	return stroke->smooth_edges && !stroke->hard && stroke->shape.hardness >= PERCENT && stroke->shape.roundness >= PERCENT &&
		   stroke->shape.diameter > 1;
}

static gboolean uses_pixel_lines(const Stroke *stroke) {
	/* Any tip of one pixel or less paints whole pixels at size 1 */
	return (stroke->hard && stroke->shape.diameter == 1) || (stroke->smooth_edges && stroke->shape.diameter <= 1);
}

static gboolean paints_whole_pixels(const Stroke *stroke) {
	return stroke->smooth_edges && (uses_pixel_lines(stroke) || (stroke->square && stroke->hard));
}

static gboolean draws_perfect_pixels(const Stroke *stroke) {
	return stroke->pixel_perfect && uses_pixel_lines(stroke) && paints_whole_pixels(stroke);
}

static void touch_pixel(Stroke *stroke, int x, int y) {
	stroke->touched = union_region(stroke->touched, (RasterExtent){x, y, 1, 1});
	mark_stroke_row(stroke, y, x, x);
}

static RasterExtent take_touched_pixels(Stroke *stroke) {
	RasterExtent touched = stroke->touched;
	stroke->touched = (RasterExtent){0};
	return touched;
}

/* Two pixels a step apart on both axes with the one between them beside each, which doubles the line's corner */
static gboolean makes_corner(PerfectPixel first, PerfectPixel middle, PerfectPixel last) {
	return abs(last.x - first.x) == 1 && abs(last.y - first.y) == 1 && (middle.x == first.x || middle.y == first.y) &&
		   (middle.x == last.x || middle.y == last.y);
}

static gboolean is_inside_stroke(const Stroke *stroke, int x, int y) {
	return x >= 0 && y >= 0 && x < stroke->extent.width && y < stroke->extent.height;
}

/* Native pixel art improvement: each new pixel of a one pixel line takes back the corner pixel of an L it
 * completes, restoring what that pixel covered before so crossings of the stroke stay painted */
static void plot_perfect_pixel(int x, int y, gpointer data) {
	Stroke *stroke = data;
	PerfectPixel *recent = stroke->recent;
	if (stroke->recent_count && recent[stroke->recent_count - 1].x == x && recent[stroke->recent_count - 1].y == y)
		return;
	gboolean inside = is_inside_stroke(stroke, x, y);
	gsize index = inside ? (gsize)y * stroke->extent.width + x : 0;
	PerfectPixel next = {x, y, inside ? read_rgba_pixel(stroke->coverage, index) : 0};
	if (stroke->recent_count == 2) {
		PerfectPixel corner = recent[1];
		if (!makes_corner(recent[0], corner, next))
			recent[0] = corner;
		else if (is_inside_stroke(stroke, corner.x, corner.y)) {
			write_rgba_pixel(stroke->coverage, (gsize)corner.y * stroke->extent.width + corner.x, corner.covered);
			touch_pixel(stroke, corner.x, corner.y);
		}
		stroke->recent_count = 1;
	}
	recent[stroke->recent_count++] = next;
	if (inside)
		write_rgba_pixel(stroke->coverage, index, OPAQUE_WORD | stroke->color);
	touch_pixel(stroke, x, y);
}

/* Native pixel art tip: a hard square of whole pixels, centered on the pixel under the point */
static RasterExtent stamp_square(Stroke *stroke, int x, int y) {
	int size = MAX(1, (int)lround(stroke->shape.diameter));
	int left = x - (size - 1) / 2, top = y - (size - 1) / 2;
	RasterExtent clip = intersect_region((RasterExtent){left, top, size, size}, stroke->extent);
	guint32 color = OPAQUE_WORD | swap_red_and_blue(stroke->color);
	guint32 *target = stroke->coverage->value.buffer.data;
	for (int row = clip.y; row < clip.y + clip.height; row++) {
		guint32 *line = target + (gsize)(row - stroke->extent.y) * stroke->extent.width - stroke->extent.x;
		for (int column = clip.x; column < clip.x + clip.width; column++)
			line[column] = color;
	}
	mark_stroke_extent(stroke, clip);
	return clip;
}

/* Squares follow a pixel line between the points, so a stroke is the same at any pointer speed */
static RasterExtent stamp_square_line(Stroke *stroke, double from_x, double from_y, double to_x, double to_y) {
	int x = floor(from_x), y = floor(from_y), end_x = floor(to_x), end_y = floor(to_y);
	int across = abs(end_x - x), down = -abs(end_y - y), step_x = x < end_x ? 1 : -1, step_y = y < end_y ? 1 : -1;
	int error = across + down;
	RasterExtent extent = {0};
	while (TRUE) {
		RasterExtent stamped = stamp_square(stroke, x, y);
		extent = empty_extent(extent) ? stamped : union_region(extent, stamped);
		if (x == end_x && y == end_y)
			break;
		int twice = 2 * error;
		if (twice >= down) {
			error += down;
			x += step_x;
		}
		if (twice <= across) {
			error += across;
			y += step_y;
		}
	}
	return extent;
}

static RasterExtent bound_segment(double x, double y, double end_x, double end_y) {
	int left = floor(MIN(x, end_x)), top = floor(MIN(y, end_y));
	return (RasterExtent){left, top, (int)ceil(MAX(x, end_x)) - left, (int)ceil(MAX(y, end_y)) - top};
}

static RasterExtent space_dabs(Stroke *stroke, double x, double y, double pressure) {
	RasterExtent extent = {0};
	x = point_value(x);
	y = point_value(y);
	double direction_x = point_value(x - stroke->last_x), direction_y = point_value(y - stroke->last_y);
	double length = sqrt(direction_x * direction_x + direction_y * direction_y);
	direction_x /= length;
	direction_y /= length;
	double distance = length, walked = -stroke->carried;
	if (stroke->square && stroke->hard && !uses_pixel_lines(stroke)) {
		extent = stamp_square_line(stroke, stroke->last_x, stroke->last_y, x, y);
		walked = distance;
	} else if (draws_perfect_pixels(stroke)) {
		walk_pixel_line(stroke->last_x, stroke->last_y, x, y, plot_perfect_pixel, stroke);
		extent = take_touched_pixels(stroke);
		walked = distance;
	} else if (uses_pixel_lines(stroke) && paints_whole_pixels(stroke)) {
		/* The pixel under the pointer is painted as soon as the pointer enters it */
		draw_pixel_line(stroke->coverage, stroke->extent.width, stroke->extent.height, stroke->last_x, stroke->last_y,
						x, y, OPAQUE_WORD | stroke->color);
		extent = union_region((RasterExtent){floor(stroke->last_x), floor(stroke->last_y), 1, 1},
							  (RasterExtent){floor(x), floor(y), 1, 1});
		mark_stroke_extent(stroke, extent);
		walked = distance;
	} else if (uses_pixel_lines(stroke)) {
		if (distance > PIXEL_LINE_DISTANCE) {
			draw_pixel_line(stroke->coverage, stroke->extent.width, stroke->extent.height, stroke->last_x,
							stroke->last_y, x, y, OPAQUE_WORD | stroke->color);
			extent = union_region(extent, bound_segment(stroke->last_x, stroke->last_y, x, y));
			mark_stroke_extent(stroke, extent);
			walked = distance;
		} else {
			x = stroke->last_x;
			y = stroke->last_y;
		}
	} else if (sweeps_capsules(stroke)) {
		extent = sweep_capsule(stroke, x, y);
		walked = distance;
	} else
		while (TRUE) {
			double diameter = stroke->shape.diameter;
			double step = from_percent(MAX(SMALLEST_ROUNDNESS, stroke->shape.roundness)) *
						  from_percent(stroke->shape.spacing) * (diameter + stroke->last_diameter) / 2 * stroke->tip_ratio;
			/* Hard edges keep dab scallops under a quarter pixel, since a step s on radius r
		 * leaves bumps about s * s / (8 * r) deep */
			if (stroke->smooth_edges && stroke->shape.hardness >= SMOOTH_HARDNESS)
				step = MIN(step, sqrt(diameter));
			step = MAX(step, SHORTEST_STEP);
			if (!(walked + step < distance))
				break;
			walked += step;
			stroke->travelled += step;
			extent = union_region(extent, draw_dab(stroke, stroke->last_x + direction_x * walked,
												   stroke->last_y + direction_y * walked, diameter));
		}
	stroke->carried = distance - walked;
	stroke->last_x = x;
	stroke->last_y = y;
	stroke->pressure = pressure;
	return extent;
}

static RasterExtent smooth_dabs(Stroke *stroke, double x, double y, double pressure, gboolean final) {
	if (stroke->smoothing == 0 || final)
		return space_dabs(stroke, x, y, pressure);
	double offset_x = x - stroke->brake_x, offset_y = y - stroke->brake_y;
	double distance = sqrt(offset_x * offset_x + offset_y * offset_y);
	offset_x /= distance;
	offset_y /= distance;
	if (!(distance > stroke->smoothing))
		return (RasterExtent){0};
	double target_x = stroke->brake_x + offset_x * (distance - stroke->smoothing);
	double target_y = stroke->brake_y + offset_y * (distance - stroke->smoothing);
	RasterExtent extent = space_dabs(stroke, target_x, target_y, pressure);
	stroke->brake_x = target_x;
	stroke->brake_y = target_y;
	return extent;
}

static void find_curve_handle(double x, double y, double base_x, double base_y, double next_x, double next_y,
							  double *handle_x, double *handle_y) {
	double first_x = x - base_x, first_y = y - base_y, second_x = next_x - base_x, second_y = next_y - base_y;
	double first = sqrt(first_x * first_x + first_y * first_y);
	double second = sqrt(second_x * second_x + second_y * second_y);
	double angle = acos((first_x * second_x + first_y * second_y) / (first * second)) / G_PI;
	angle = SMALLEST_HANDLE_SHARE + angle * (LARGEST_HANDLE_SHARE - SMALLEST_HANDLE_SHARE);
	double across_x = next_x - x, across_y = next_y - y;
	double across = sqrt(across_x * across_x + across_y * across_y), factor = angle * second / across;
	*handle_x = point_value(across_x * factor);
	*handle_y = point_value(across_y * factor);
}

static int count_points(const Stroke *stroke) {
	return stroke->points->len / POINT_VALUES;
}

static void read_point(const Stroke *stroke, int point, double *x, double *y) {
	*x = g_array_index(stroke->points, double, point * POINT_VALUES);
	*y = g_array_index(stroke->points, double, point * POINT_VALUES + 1);
}

/* The curve from the third last to the second last of the first points, bent toward the points around them */
static RasterExtent draw_curve(Stroke *stroke, int points) {
	int start = points - 3, end = points - 2;
	double start_x, start_y, end_x, end_y;
	read_point(stroke, start, &start_x, &start_y);
	read_point(stroke, end, &end_x, &end_y);
	double start_pressure = g_array_index(stroke->pressures, double, start);
	double end_pressure = g_array_index(stroke->pressures, double, end);
	double before_x = 0, before_y = 0, after_x = 0, after_y = 0, x, y;
	if (start > 0) {
		read_point(stroke, start - 1, &x, &y);
		find_curve_handle(x, y, start_x, start_y, end_x, end_y, &before_x, &before_y);
	}
	if (end + 1 < count_points(stroke)) {
		read_point(stroke, end + 1, &x, &y);
		find_curve_handle(x, y, end_x, end_y, start_x, start_y, &after_x, &after_y);
	}
	double first_x = start_x + before_x, first_y = start_y + before_y;
	double second_x = end_x + after_x, second_y = end_y + after_y;
	RasterExtent extent = {0};
	int steps = uses_pixel_lines(stroke) ? 1 : CURVE_STEPS;
	for (int step = 0; step < steps; step++) {
		double t = (step + 1.0) / steps, u = 1 - t;
		double x = u * u * u * start_x + 3 * t * u * u * first_x + 3 * t * t * u * second_x + t * t * t * end_x;
		double y = u * u * u * start_y + 3 * t * u * u * first_y + 3 * t * t * u * second_y + t * t * t * end_y;
		extent = union_region(extent,
							  smooth_dabs(stroke, x, y, start_pressure + t * (end_pressure - start_pressure), FALSE));
	}
	return extent;
}

void move_stroke_to(Stroke *stroke, double x, double y, double pressure) {
	pressure = clamp_pressure(pressure);
	stroke->pressure = pressure;
	stroke->last_x = stroke->brake_x = x;
	stroke->last_y = stroke->brake_y = y;
	g_array_set_size(stroke->points, 0);
	g_array_set_size(stroke->pressures, 0);
	g_array_append_val(stroke->points, x);
	g_array_append_val(stroke->points, y);
	g_array_append_val(stroke->pressures, pressure);
	stroke->swept_solid = FALSE;
	if (stroke->square && stroke->hard && !uses_pixel_lines(stroke))
		record_stroke_extent(stroke, stamp_square(stroke, floor(x), floor(y)));
	else if (draws_perfect_pixels(stroke)) {
		stroke->recent_count = 0;
		plot_perfect_pixel(floor(x), floor(y), stroke);
		record_stroke_extent(stroke, take_touched_pixels(stroke));
	} else if (paints_whole_pixels(stroke)) {
		draw_pixel_line(stroke->coverage, stroke->extent.width, stroke->extent.height, x, y, x, y,
						OPAQUE_WORD | stroke->color);
		record_stroke_extent(stroke, (RasterExtent){floor(x), floor(y), 1, 1});
		mark_stroke_extent(stroke, (RasterExtent){floor(x), floor(y), 1, 1});
	} else
		record_stroke_extent(stroke, draw_dab(stroke, x, y, stroke->shape.diameter));
}

void draw_stroke_line_to(Stroke *stroke, double x, double y, double pressure) {
	pressure = clamp_pressure(pressure);
	double last_x, last_y;
	read_point(stroke, count_points(stroke) - 1, &last_x, &last_y);
	if (x == last_x && y == last_y)
		return;
	g_array_append_val(stroke->points, x);
	g_array_append_val(stroke->points, y);
	g_array_append_val(stroke->pressures, pressure);
	/* Whole pixel tips draw straight to the pointer without the curve's one segment of lag */
	if (paints_whole_pixels(stroke)) {
		record_stroke_extent(stroke, space_dabs(stroke, x, y, pressure));
		return;
	}
	if (count_points(stroke) >= 3)
		record_stroke_extent(stroke, draw_curve(stroke, count_points(stroke)));
}

void finish_stroke(Stroke *stroke) {
	if (paints_whole_pixels(stroke))
		return;
	/* Two points draw straight, and more finish the curve to the last point */
	int points = count_points(stroke);
	double x, y;
	if (points == 2) {
		read_point(stroke, 1, &x, &y);
		record_stroke_extent(stroke, smooth_dabs(stroke, x, y, g_array_index(stroke->pressures, double, 1), TRUE));
	}
	if (points > 2)
		record_stroke_extent(stroke, draw_curve(stroke, points + 1));
}

RasterExtent get_stroke_dirty_extent(const Stroke *stroke) {
	return intersect_region(stroke->dirty, stroke->extent);
}

/* Hands over the changes since the last call as one region per band of rows, which keeps a large tip's region to
 * the thin edge it moved instead of its whole square */
void take_stroke_pending_regions(Stroke *stroke, GArray *regions) {
	const int band_rows = 16;
	RasterExtent area = stroke->extent;
	for (int band = stroke->pending_top; band <= stroke->pending_bottom; band += band_rows) {
		RasterExtent region = {0};
		for (int row = band; row < MIN(band + band_rows, stroke->pending_bottom + 1); row++) {
			if (stroke->pending_low[row] == G_MAXINT)
				continue;
			RasterExtent span = {area.x + stroke->pending_low[row], area.y + row,
								 stroke->pending_high[row] - stroke->pending_low[row] + 1, 1};
			region = empty_extent(region) ? span : union_region(region, span);
			stroke->pending_low[row] = G_MAXINT;
		}
		if (!empty_extent(region))
			g_array_append_val(regions, region);
	}
	stroke->pending_top = G_MAXINT;
	stroke->pending_bottom = G_MININT;
}

RasterExtent get_stroke_total_extent(const Stroke *stroke) {
	return intersect_region(stroke->total, stroke->extent);
}
