#pragma once

#include <stdint.h>

#include "record.h"

typedef struct {
	int x, y, width, height;
} RasterExtent;

/* Called for each pixel of a one pixel line in drawing order */
typedef void (*PixelVisitor)(int x, int y, gpointer data);

/* Translated kernels operate on source channels without supplying tool state */
void walk_pixel_line(double x, double y, double end_x, double end_y, PixelVisitor visit, gpointer data);
void draw_pixel_line(Record *channels, int width, int height, double x, double y, double end_x, double end_y,
					 guint32 color);
RasterExtent read_extent(const Record *record);
gboolean empty_extent(RasterExtent extent);
RasterExtent intersect_region(RasterExtent first, RasterExtent second);
RasterExtent union_region(RasterExtent first, RasterExtent second);
gboolean equal_region(RasterExtent first, RasterExtent second);
double round_like_source(double value);
double step_zoom_level(double zoom, gboolean in);
double clamp_zoom_level(double zoom);

/* Packed ARGB access to eight-bit RGBA channels, ignoring pixels outside the buffer */
static inline guint32 read_rgba_pixel(const Record *channels, gsize pixel) {
	if (!channels || channels->type != RECORD_BUFFER || channels->value.buffer.depth != 8 ||
		pixel * 4 + 3 >= channels->value.buffer.length)
		return 0;
	const guint8 *data = (const guint8 *)channels->value.buffer.data + pixel * 4;
	return (guint32)data[3] << 24 | (guint32)data[0] << 16 | (guint32)data[1] << 8 | data[2];
}

static inline void write_rgba_pixel(Record *channels, gsize pixel, guint32 color) {
	if (!channels || channels->type != RECORD_BUFFER || channels->value.buffer.depth != 8 ||
		pixel * 4 + 3 >= channels->value.buffer.length)
		return;
	guint8 *data = (guint8 *)channels->value.buffer.data + pixel * 4;
	data[0] = color >> 16;
	data[1] = color >> 8;
	data[2] = color;
	data[3] = color >> 24;
}
