#pragma once

#include "record.h"
#include "kernels.h"

typedef struct {
	double a, b, c, d, e, f;
} Affine;

/* One tip level pairs alpha values with their extent array entries */
typedef struct {
	Record *values;
	RasterExtent extent;
} BrushLevel;

/* The computed brush descriptor values that the stroke engine reads */
typedef struct {
	double diameter, hardness, angle, roundness, spacing;
} BrushShape;

typedef double (*RandomSource)(gpointer data);

/* Native pixel art improvement: one of the last two pixels of a pixel perfect line with the coverage it replaced */
typedef struct {
	int x, y;
	guint32 covered;
} PerfectPixel;

typedef struct {
	BrushShape shape;
	gboolean hard, smooth_edges, square;
	double flow, smoothing;
	guint32 color;
	RasterExtent extent;
	Record *coverage;
	GArray *tip_levels;
	double tip_scale, tip_ratio;
	Record *tip_rgba;
	GArray *points, *pressures;
	double last_x, last_y, brake_x, brake_y, pressure, carried, last_diameter, travelled;
	RasterExtent dirty, total;
	/* Native: the changed columns of each row since the pending regions were last taken, so large tips redraw only
	 * what they changed, and the first and last rows holding any */
	int *pending_low, *pending_high, pending_top, pending_bottom;
	/* Set when the last capsule ended solid at the current point, so the next one skips that circle's interior */
	gboolean swept_solid;
	/* Native pixel art improvement: one pixel lines drop the corner pixel of every L they make */
	gboolean pixel_perfect;
	PerfectPixel recent[2];
	int recent_count;
	RasterExtent touched;
	RandomSource random;
	gpointer random_data;
} Stroke;

Affine create_identity_affine(void);
void concatenate_affine(Affine *transform, Affine after);
void translate_affine(Affine *transform, double x, double y);
void scale_affine(Affine *transform, double x, double y);
void rotate_affine(Affine *transform, double angle);
gboolean is_translation_affine(Affine transform);
BrushLevel create_computed_tip(double diameter, double hardness, double scale);
void build_brush_levels(GArray *levels);
void free_brush_levels(GArray *levels);
BrushLevel resample_brush(GArray *levels, Affine transform);

Stroke *create_stroke(BrushShape shape, gboolean hard, double flow, double smoothing, guint32 color,
					  RasterExtent extent, RandomSource random, gpointer random_data);
void free_stroke(Stroke *stroke);
void move_stroke_to(Stroke *stroke, double x, double y, double pressure);
void draw_stroke_line_to(Stroke *stroke, double x, double y, double pressure);
void finish_stroke(Stroke *stroke);
RasterExtent get_stroke_dirty_extent(const Stroke *stroke);
void take_stroke_pending_regions(Stroke *stroke, GArray *regions);
RasterExtent get_stroke_total_extent(const Stroke *stroke);
double get_default_random(gpointer data);
