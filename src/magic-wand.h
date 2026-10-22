#pragma once

#include "document.h"

typedef struct {
	int tolerance;
	gboolean smooth_edges, contiguous;
} WandSettings;

Record *create_wand_mask(const guint8 *pixels, RasterExtent pixels_extent, RasterExtent area, double x, double y,
						 WandSettings settings);
Record *create_similar_area(Document *document, double x, double y, WandSettings settings, gboolean all_layers,
							RasterExtent *bounds, GError **error);
