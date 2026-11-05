#pragma once

#include <stdint.h>

#include "document/numbers.h"
#include "document/record.h"

typedef struct {
	int x, y, width, height;
} RasterExtent;

/* A drag with Shift held keeps to the axis it started along: moving up and down keeps x, and sideways keeps y */
typedef enum { AXIS_FREE, AXIS_VERTICAL, AXIS_HORIZONTAL } LockedAxis;

/* Called for each pixel of a one pixel line in drawing order */
typedef void (*PixelVisitor)(int x, int y, gpointer data);

/* The kernels work on a document's channels alone, without any tool's state */
void walk_pixel_line(double x, double y, double end_x, double end_y, PixelVisitor visit, gpointer data);
void draw_pixel_line(Record *channels, int width, int height, double x, double y, double end_x, double end_y,
					 guint32 color);
RasterExtent read_extent(const Record *record);
gboolean empty_extent(RasterExtent extent);
RasterExtent intersect_region(RasterExtent first, RasterExtent second);
RasterExtent union_region(RasterExtent first, RasterExtent second);
gboolean equal_region(RasterExtent first, RasterExtent second);
double step_zoom_level(double zoom, gboolean in);
/* Moves a point to the nearest eighth turn around a start, keeping its distance; a point closer to an axis than
 * half the other way snaps to the axis, and others to the diagonal */
void constrain_to_eighth_turns(double start_x, double start_y, double *x, double *y);
double clamp_zoom_level(double zoom);

/* Colours keep red in their high byte, while stored pixel words keep it in the low one, and this turns one order into
 * the other, without alpha */
static inline guint32 swap_red_and_blue(guint32 color) {
	return (color >> WORD_RED_SHIFT & G_MAXUINT8) | (color & G_MAXUINT8 << WORD_GREEN_SHIFT) |
		   (color & G_MAXUINT8) << WORD_RED_SHIFT;
}

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
