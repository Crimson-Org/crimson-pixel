#include <math.h>
#include <stdlib.h>

#include "kernels.h"

void walk_pixel_line(double x, double y, double end_x, double end_y, PixelVisitor visit, gpointer data) {
	int first_x = floor(x), first_y = floor(y), last_x = floor(end_x), last_y = floor(end_y);
	int dx = abs(last_x - first_x), dy = -abs(last_y - first_y);
	int step_x = first_x < last_x ? 1 : -1, step_y = first_y < last_y ? 1 : -1;
	int distance = dx + dy;
	while (TRUE) {
		visit(first_x, first_y, data);
		if (first_x == last_x && first_y == last_y)
			break;
		int twice = distance * 2;
		/* aH.amQ leaves exact diagonal ties on the earlier pixel */
		if (twice > dy) {
			distance += dy;
			first_x += step_x;
		}
		if (twice < dx) {
			distance += dx;
			first_y += step_y;
		}
	}
}

typedef struct {
	Record *channels;
	int width, height;
	guint32 color;
} PixelLineTarget;

static void write_line_pixel(int x, int y, gpointer data) {
	PixelLineTarget *target = data;
	if (x >= 0 && y >= 0 && x < target->width && y < target->height)
		write_rgba_pixel(target->channels, (gsize)y * target->width + x, target->color);
}

void draw_pixel_line(Record *channels, int width, int height, double x, double y, double end_x, double end_y,
					 guint32 color) {
	PixelLineTarget target = {channels, width, height, color};
	walk_pixel_line(x, y, end_x, end_y, write_line_pixel, &target);
}

RasterExtent read_extent(const Record *record) {
	return (RasterExtent){get_number(record, "x"), get_number(record, "y"), get_number(record, "s"),
						  get_number(record, "T")};
}

gboolean empty_extent(RasterExtent extent) {
	return extent.width <= 0 || extent.height <= 0;
}

RasterExtent intersect_region(RasterExtent first, RasterExtent second) {
	int x = MAX(first.x, second.x), y = MAX(first.y, second.y);
	int right = MIN(first.x + first.width, second.x + second.width);
	int bottom = MIN(first.y + first.height, second.y + second.height);
	return right < x || bottom < y ? (RasterExtent){0} : (RasterExtent){x, y, right - x, bottom - y};
}

RasterExtent union_region(RasterExtent first, RasterExtent second) {
	if (empty_extent(first))
		return second;
	if (empty_extent(second))
		return first;
	int x = MIN(first.x, second.x), y = MIN(first.y, second.y);
	return (RasterExtent){x, y, MAX(first.x + first.width, second.x + second.width) - x,
						  MAX(first.y + first.height, second.y + second.height) - y};
}

gboolean equal_region(RasterExtent first, RasterExtent second) {
	return first.x == second.x && first.y == second.y && first.width == second.width && first.height == second.height;
}

/* Math.round rounds halves upward, unlike C round */
double round_like_source(double value) {
	double whole = floor(value);
	return value - whole >= .5 ? whole + 1 : whole;
}

static const double ZOOM_LEVELS[] = {128,
									 64,
									 32,
									 16,
									 12,
									 8,
									 6,
									 5,
									 4,
									 3,
									 2,
									 1,
									 2.0 / 3,
									 1.0 / 2,
									 1.0 / 2 * (2.0 / 3),
									 1.0 / 4,
									 1.0 / 4 * (2.0 / 3),
									 1.0 / 8,
									 1.0 / 8 * (2.0 / 3),
									 1.0 / 16,
									 1.0 / 16 * (2.0 / 3),
									 1.0 / 32,
									 1.0 / 32 * (2.0 / 3),
									 1.0 / 64};

double step_zoom_level(double zoom, gboolean in) {
	guint index = 0, last = G_N_ELEMENTS(ZOOM_LEVELS) - 1;
	while (index <= last && ZOOM_LEVELS[index] > zoom)
		index++;
	if (in)
		return index == 0 ? zoom : ZOOM_LEVELS[index - 1];
	if (index >= last)
		return zoom;
	return ZOOM_LEVELS[index + (ZOOM_LEVELS[index] == zoom)];
}

double clamp_zoom_level(double zoom) {
	return CLAMP(zoom, ZOOM_LEVELS[G_N_ELEMENTS(ZOOM_LEVELS) - 1], ZOOM_LEVELS[0]);
}
