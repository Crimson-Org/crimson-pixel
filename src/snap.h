#pragma once

#include "document.h"

enum { SNAP_GUIDES, SNAP_GRID, SNAP_LAYERS, SNAP_SLICES, SNAP_BOUNDS, SNAP_TARGETS };

typedef struct {
	gboolean enabled, targets[SNAP_TARGETS];
} SnapSettings;

typedef struct {
	double offset_x, offset_y, line_x, line_y, source_x, source_y;
	gboolean has_source_x, has_source_y;
} SnapResult;

void initialize_snap_settings(SnapSettings *settings);
SnapResult snap_points(Document *document, const SnapSettings *settings, const double *xs, const double *ys, int count,
					   double zoom, const GArray *excluded, const RasterExtent *extra, int extra_count);
SnapResult snap_rectangle(Document *document, const SnapSettings *settings, double x, double y, double width,
						  double height, double zoom, const GArray *excluded, const RasterExtent *extra,
						  int extra_count);
