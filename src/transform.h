#pragma once

#include "document.h"

typedef struct {
	double m[8];
} Projective;

typedef enum { RESAMPLE_NEAREST, RESAMPLE_BILINEAR, RESAMPLE_SHARPER } ResampleMode;

typedef enum { TRANSFORM_LAYERS, TRANSFORM_PIXELS, TRANSFORM_SELECTION } TransformMode;

typedef void (*SnapPoint)(gpointer data, double *x, double *y);

typedef struct {
	double start[8], handle_x, handle_y, reference_x, reference_y;
	int handle;
	gboolean keeps_ratio, clamps_scale, skews;
	SnapPoint snap;
	gpointer snap_data;
} BoxDrag;

gboolean projective_from_rectangle(const double quad[8], double x, double y, double width, double height,
								   Projective *result);
Projective invert_projective(Projective matrix);
Projective multiply_projective(Projective first, Projective second);
void apply_projective(Projective matrix, double x, double y, double *result_x, double *result_y);
gboolean is_convex_quad(const double quad[8]);
void scale_box(const BoxDrag *drag, const double current[8], double x, double y, gboolean shift, gboolean alt,
			   gboolean control, double next[8]);
Record *resample_projective(const Record *pixels, RasterExtent extent, Projective matrix, RasterExtent *result);
Record *resample_image(const Record *pixels, RasterExtent extent, Projective matrix, ResampleMode mode,
					   RasterExtent *result);

TransformMode get_transform_mode(Document *document, gboolean selection_only);
RasterExtent get_transform_bounds(Document *document, TransformMode mode);
gboolean apply_transform(Document *document, TransformMode mode, const double quad[8], gboolean extended_history,
						 GError **error);
