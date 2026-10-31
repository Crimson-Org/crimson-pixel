#pragma once

#include <stdint.h>

#include "document/numbers.h"
#include "document/record.h"

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
double step_zoom_level(double zoom, gboolean in);
double clamp_zoom_level(double zoom);

/* Packed ARGB access to eight-bit RGBA channels, ignoring pixels outside the buffer */
static inline guint32 read_rgba_pixel(const Record *channels, gsize pixel) {
	if (!channels || channels->type != RECORD_BUFFER || channels->value.buffer.depth != BYTE_DEPTH ||
		pixel * RGBA_CHANNELS + ALPHA_CHANNEL >= channels->value.buffer.length)
		return 0;
	const guint8 *data = (const guint8 *)channels->value.buffer.data + pixel * RGBA_CHANNELS;
	return (guint32)data[ALPHA_CHANNEL] << WORD_ALPHA_SHIFT | (guint32)data[0] << WORD_RED_SHIFT |
		   (guint32)data[1] << WORD_GREEN_SHIFT | data[2];
}

static inline void write_rgba_pixel(Record *channels, gsize pixel, guint32 color) {
	if (!channels || channels->type != RECORD_BUFFER || channels->value.buffer.depth != BYTE_DEPTH ||
		pixel * RGBA_CHANNELS + ALPHA_CHANNEL >= channels->value.buffer.length)
		return;
	guint8 *data = (guint8 *)channels->value.buffer.data + pixel * RGBA_CHANNELS;
	data[0] = color >> WORD_RED_SHIFT;
	data[1] = color >> WORD_GREEN_SHIFT;
	data[2] = color;
	data[ALPHA_CHANNEL] = color >> WORD_ALPHA_SHIFT;
}
