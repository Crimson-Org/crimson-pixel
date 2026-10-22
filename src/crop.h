#pragma once

#include "document.h"
#include "transform.h"

typedef enum { CROP_FREE, CROP_RATIO, CROP_SIZE } CropMode;

typedef struct {
	CropMode mode;
	double width, height, resolution;
	gboolean delete_pixels;
} CropOptions;

typedef struct {
	RasterExtent rectangle, clip;
	Projective straighten;
	gboolean changes_layers, clips;
	double resolution;
	ResampleMode resample;
} CropPlan;

typedef enum { TRIM_TOP_LEFT, TRIM_BOTTOM_RIGHT, TRIM_TRANSPARENT } TrimColor;

enum { TRIM_TOP, TRIM_LEFT, TRIM_BOTTOM, TRIM_RIGHT };

RasterExtent get_crop_rectangle(const double quad[8], double *angle);
RasterExtent constrain_crop_drag(double start_x, double start_y, double end_x, double end_y, const CropOptions *options,
								 gboolean square, gboolean centered);
RasterExtent get_crop_start_box(int width, int height, const CropOptions *options);
void get_straighten_box(int width, int height, double start_x, double start_y, double end_x, double end_y,
						double quad[8]);
gboolean plan_crop(const double quad[8], int width, int height, const CropOptions *options, CropPlan *plan);
gboolean crop_document(Document *document, const double quad[8], const CropOptions *options, gboolean clear_selection,
					   gboolean extended_history);
gboolean resize_image(Document *document, int width, int height, double resolution, ResampleMode mode,
					  gboolean extended_history);
gboolean resize_canvas(Document *document, int width, int height, int anchor, gboolean extended_history);
RasterExtent find_trim_bounds(const Record *pixels, int width, int height, TrimColor color, const gboolean sides[4]);
gboolean trim_document(Document *document, TrimColor color, const gboolean sides[4], gboolean extended_history);
gboolean reveal_all(Document *document, gboolean extended_history);
