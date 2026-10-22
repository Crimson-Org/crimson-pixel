#pragma once

#include <gtk/gtk.h>

#include "kernels.h"

/* GPU tiles over the premultiplied display surface with a halving pyramid for zoomed out views */
typedef struct TiledImage TiledImage;

TiledImage *create_tiled_image(cairo_surface_t *surface);
void free_tiled_image(TiledImage *image);
void invalidate_tiled_image(TiledImage *image, RasterExtent extent);
void snapshot_tiled_image(TiledImage *image, GtkSnapshot *snapshot, const graphene_rect_t *bounds,
						  const graphene_rect_t *visible, double zoom, gboolean smooth, gboolean transformed);
void snapshot_patched_image(TiledImage *base, TiledImage *patch, RasterExtent patch_extent, int width, int height,
							GtkSnapshot *snapshot, const graphene_rect_t *bounds, const graphene_rect_t *visible,
							double zoom, gboolean smooth);
