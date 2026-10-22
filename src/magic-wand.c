#include <stdlib.h>
#include <string.h>

#include "magic-wand.h"
#include "pixel-selection.h"
#include "../lang/en.h"

static const guint8 *read_area_pixel(const guint8 *pixels, RasterExtent extent, int x, int y) {
	static const guint8 transparent[4] = {0};
	if (x < extent.x || y < extent.y || x >= extent.x + extent.width || y >= extent.y + extent.height)
		return transparent;
	return pixels + ((gsize)(y - extent.y) * extent.width + x - extent.x) * 4;
}

static int measure_color_distance(const guint8 *pixel, const guint8 *color) {
	if (color[3] == 0)
		return pixel[3] < 5 ? 0 : 255;
	if (pixel[3] == 0)
		return 255;
	return MAX(abs(pixel[0] - color[0]), MAX(abs(pixel[1] - color[1]), abs(pixel[2] - color[2])));
}

static guint8 get_edge_value(int distance, WandSettings settings) {
	if (!settings.smooth_edges || settings.tolerance <= 0)
		return 0;
	double beyond = CLAMP((distance - settings.tolerance) * (1.0 / settings.tolerance), 0, 1);
	return (guint8)(255 * (1 - beyond));
}

static void fill_contiguous(const guint8 *pixels, RasterExtent extent, RasterExtent area, int start_x, int start_y,
							const guint8 *color, WandSettings settings, guint8 *mask) {
	gsize count = (gsize)area.width * area.height;
	g_autofree guint8 *visited = g_malloc0(count);
	g_autofree guint32 *pending = g_new(guint32, count);
	gsize waiting = 0, start = (gsize)start_y * area.width + start_x;
	pending[waiting++] = start;
	visited[start] = 1;
	while (waiting) {
		guint32 index = pending[--waiting];
		int x = index % area.width, y = index / area.width;
		int distance = measure_color_distance(read_area_pixel(pixels, extent, area.x + x, area.y + y), color);
		if (distance > settings.tolerance) {
			mask[index] = get_edge_value(distance, settings);
			continue;
		}
		mask[index] = 255;
		const gboolean open[4] = {y != area.height - 1, y != 0, x != area.width - 1, x != 0};
		const gint64 offsets[4] = {area.width, -area.width, 1, -1};
		for (int side = 0; side < 4; side++) {
			if (!open[side] || visited[index + offsets[side]])
				continue;
			visited[index + offsets[side]] = 1;
			pending[waiting++] = index + offsets[side];
		}
	}
}

static gboolean touches_full_pixel(const guint8 *mask, RasterExtent area, int x, int y) {
	for (int row = MAX(0, y - 1); row <= MIN(area.height - 1, y + 1); row++)
		for (int column = MAX(0, x - 1); column <= MIN(area.width - 1, x + 1); column++)
			if ((row != y || column != x) && mask[(gsize)row * area.width + column] == 255)
				return TRUE;
	return FALSE;
}

static void fill_matching(const guint8 *pixels, RasterExtent extent, RasterExtent area, const guint8 *color,
						  WandSettings settings, guint8 *mask) {
	for (int y = 0; y < area.height; y++)
		for (int x = 0; x < area.width; x++)
			if (measure_color_distance(read_area_pixel(pixels, extent, area.x + x, area.y + y), color) <=
				settings.tolerance)
				mask[(gsize)y * area.width + x] = 255;
	for (int y = 0; y < area.height; y++)
		for (int x = 0; x < area.width; x++) {
			gsize index = (gsize)y * area.width + x;
			if (mask[index] || !touches_full_pixel(mask, area, x, y))
				continue;
			int distance = measure_color_distance(read_area_pixel(pixels, extent, area.x + x, area.y + y), color);
			mask[index] = get_edge_value(distance, settings);
		}
}

Record *create_wand_mask(const guint8 *pixels, RasterExtent pixels_extent, RasterExtent area, double x, double y,
						 WandSettings settings) {
	Record *channels = create_channels((gsize)area.width * area.height, 8);
	guint8 *mask = channels->value.buffer.data;
	int start_x = CLAMP((int)round_like_source(x - .5 - area.x), 0, area.width - 1);
	int start_y = CLAMP((int)round_like_source(y - .5 - area.y), 0, area.height - 1);
	guint8 color[4];
	memcpy(color, read_area_pixel(pixels, pixels_extent, area.x + start_x, area.y + start_y), 4);
	if (settings.contiguous)
		fill_contiguous(pixels, pixels_extent, area, start_x, start_y, color, settings, mask);
	else
		fill_matching(pixels, pixels_extent, area, color, settings, mask);
	return channels;
}

Record *create_similar_area(Document *document, double x, double y, WandSettings settings, gboolean all_layers,
							RasterExtent *bounds, GError **error) {
	RasterExtent area = {0, 0, document_width(document), document_height(document)};
	if (x < 0 || x > area.width || y < 0 || y > area.height)
		return NULL;
	Record *pixels = get_record(document->record, "buffer");
	RasterExtent pixels_extent = area;
	if (!all_layers) {
		Record *selected = get_record(document->record, "j");
		Record *layer = get_active_layer(document);
		Record *section = layer ? get_record(get_record(layer, "add"), "lsct") : NULL;
		const char *message = count_records(selected) > 1 ? TEXT_MORE_THAN_ONE_LAYER : TEXT_SELECT_LAYER_FIRST;
		if (!layer || (section && section->type == RECORD_NUMBER && section->value.number != 0)) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, message);
			return NULL;
		}
		pixels = get_record(layer, "buffer");
		pixels_extent = read_extent(get_record(layer, "V"));
	}
	if (!pixels || pixels->type != RECORD_BUFFER || pixels->value.buffer.depth != 8) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_WAND_DEPTH);
		return NULL;
	}
	g_autoptr(Record) mask = create_wand_mask(pixels->value.buffer.data, pixels_extent, area, x, y, settings);
	*bounds = find_mask_bounds(mask, area);
	Record *cropped = create_channels((gsize)bounds->width * bounds->height, 8);
	for (int row = 0; row < bounds->height; row++)
		memcpy((guint8 *)cropped->value.buffer.data + (gsize)row * bounds->width,
			   (guint8 *)mask->value.buffer.data + (gsize)(bounds->y + row) * area.width + bounds->x, bounds->width);
	return cropped;
}
