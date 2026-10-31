#pragma once

#include "document/document.h"

/* A quad is four corners as x and y pairs, clockwise from the top left */
#define QUAD_CORNERS 4
#define QUAD_VALUES (QUAD_CORNERS * 2)
/* A projective matrix is a three by three matrix with the last value fixed at one */
#define PROJECTIVE_VALUES 8
/* Corners closer than this count as the same */
#define SAME_POINT_DISTANCE 1e-6

typedef struct {
	double m[PROJECTIVE_VALUES];
} Projective;

#define IDENTITY_PROJECTIVE ((Projective){{1, 0, 0, 0, 1, 0, 0, 0}})

/* Box handles sit on a three by three grid, row by row from the top left */
#define HANDLE_GRID_SIZE 3
enum {
	HANDLE_TOP_LEFT,
	HANDLE_TOP,
	HANDLE_TOP_RIGHT,
	HANDLE_LEFT,
	HANDLE_CENTER,
	HANDLE_RIGHT,
	HANDLE_BOTTOM_LEFT,
	HANDLE_BOTTOM,
	HANDLE_BOTTOM_RIGHT,
	HANDLE_GRID_COUNT
};

typedef enum { RESAMPLE_NEAREST, RESAMPLE_BILINEAR, RESAMPLE_SHARPER } ResampleMode;

typedef enum { TRANSFORM_LAYERS, TRANSFORM_PIXELS, TRANSFORM_SELECTION } TransformMode;

typedef void (*SnapPoint)(gpointer data, double *x, double *y);

typedef struct {
	double start[QUAD_VALUES], handle_x, handle_y, reference_x, reference_y;
	int handle;
	gboolean keeps_ratio, clamps_scale, skews;
	SnapPoint snap;
	gpointer snap_data;
} BoxDrag;

gboolean projective_from_rectangle(const double quad[QUAD_VALUES], double x, double y, double width, double height,
								   Projective *result);
Projective invert_projective(Projective matrix);
Projective multiply_projective(Projective first, Projective second);
void apply_projective(Projective matrix, double x, double y, double *result_x, double *result_y);
gboolean is_convex_quad(const double quad[QUAD_VALUES]);
void get_extent_quad(RasterExtent extent, double quad[QUAD_VALUES]);
/* The matrix that maps the unit square onto the quad */
Projective get_quad_matrix(const double quad[QUAD_VALUES]);
gboolean is_axis_aligned_quad(const double quad[QUAD_VALUES]);
gboolean is_corner_handle(int handle);
/* The quad corner a corner handle sits on */
int get_handle_corner(int handle);
void scale_box(const BoxDrag *drag, const double current[QUAD_VALUES], double x, double y, gboolean shift, gboolean alt,
			   gboolean control, double next[QUAD_VALUES]);
Record *resample_projective(const Record *pixels, RasterExtent extent, Projective matrix, RasterExtent *result);
Record *resample_image(const Record *pixels, RasterExtent extent, Projective matrix, ResampleMode mode,
					   RasterExtent *result);

TransformMode get_transform_mode(Document *document, gboolean selection_only);
RasterExtent get_transform_bounds(Document *document, TransformMode mode);
gboolean apply_transform(Document *document, TransformMode mode, const double quad[QUAD_VALUES], gboolean extended_history,
						 GError **error);
