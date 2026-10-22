#include <math.h>
#include <string.h>

#include "parallel.h"
#include "tiled-image.h"

enum { TILE_SIZE = 512 };

typedef struct {
	int width, height, stride, columns, rows;
	GBytes *pixels;
	GdkTexture **tiles;
} TileLevel;

struct TiledImage {
	GPtrArray *levels;
};

static void release_surface(gpointer surface) {
	cairo_surface_destroy(surface);
}

static TileLevel *create_level(int width, int height, int stride, GBytes *pixels) {
	TileLevel *level = g_new0(TileLevel, 1);
	level->width = width;
	level->height = height;
	level->stride = stride;
	level->pixels = pixels;
	level->columns = (width + TILE_SIZE - 1) / TILE_SIZE;
	level->rows = (height + TILE_SIZE - 1) / TILE_SIZE;
	level->tiles = g_new0(GdkTexture *, (gsize)level->columns * level->rows);
	return level;
}

static void free_level(gpointer data) {
	TileLevel *level = data;
	for (int index = 0; index < level->columns * level->rows; index++)
		g_clear_object(&level->tiles[index]);
	g_free(level->tiles);
	g_bytes_unref(level->pixels);
	g_free(level);
}

/* One level reduction, shared by the rows each thread works on */
typedef struct {
	const TileLevel *source;
	TileLevel *target;
	RasterExtent extent;
} Reduction;

/* Each smaller level averages two by two premultiplied pixels of the level above it */
static void reduce_rows(int first, int last, gpointer data) {
	const Reduction *reduction = data;
	const TileLevel *source = reduction->source, *target = reduction->target;
	const guint8 *from = g_bytes_get_data(source->pixels, NULL);
	guint8 *to = (guint8 *)g_bytes_get_data(target->pixels, NULL);
	RasterExtent extent = reduction->extent;
	for (int y = first; y < last; y++) {
		int top = y * 2, bottom = MIN(top + 1, source->height - 1);
		for (int x = extent.x; x < extent.x + extent.width; x++) {
			int left = x * 2, right = MIN(left + 1, source->width - 1);
			const guint8 *a = from + (gsize)top * source->stride + left * 4;
			const guint8 *b = from + (gsize)top * source->stride + right * 4;
			const guint8 *c = from + (gsize)bottom * source->stride + left * 4;
			const guint8 *d = from + (gsize)bottom * source->stride + right * 4;
			guint8 *pixel = to + (gsize)y * target->stride + x * 4;
			for (int channel = 0; channel < 4; channel++)
				pixel[channel] = (a[channel] + b[channel] + c[channel] + d[channel] + 2) >> 2;
		}
	}
}

static void reduce_region(const TileLevel *source, TileLevel *target, RasterExtent extent) {
	Reduction reduction = {source, target, extent};
	run_rows_in_parallel(extent.y, extent.height, extent.width * 4, reduce_rows, &reduction);
}

TiledImage *create_tiled_image(cairo_surface_t *surface) {
	if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
		return NULL;
	cairo_surface_flush(surface);
	int width = cairo_image_surface_get_width(surface), height = cairo_image_surface_get_height(surface);
	int stride = cairo_image_surface_get_stride(surface);
	TiledImage *image = g_new0(TiledImage, 1);
	image->levels = g_ptr_array_new_with_free_func(free_level);
	GBytes *pixels = g_bytes_new_with_free_func(cairo_image_surface_get_data(surface), (gsize)stride * height,
												release_surface, cairo_surface_reference(surface));
	g_ptr_array_add(image->levels, create_level(width, height, stride, pixels));
	while (width > TILE_SIZE || height > TILE_SIZE) {
		TileLevel *source = g_ptr_array_index(image->levels, image->levels->len - 1);
		width = (width + 1) / 2;
		height = (height + 1) / 2;
		GBytes *reduced = g_bytes_new_take(g_malloc((gsize)width * height * 4), (gsize)width * height * 4);
		TileLevel *target = create_level(width, height, width * 4, reduced);
		reduce_region(source, target, (RasterExtent){0, 0, width, height});
		g_ptr_array_add(image->levels, target);
	}
	return image;
}

void free_tiled_image(TiledImage *image) {
	if (!image)
		return;
	g_ptr_array_unref(image->levels);
	g_free(image);
}

/* Changed pixels drop the tiles that show them and refresh the smaller levels below */
void invalidate_tiled_image(TiledImage *image, RasterExtent extent) {
	if (!image)
		return;
	for (guint index = 0; index < image->levels->len; index++) {
		TileLevel *level = g_ptr_array_index(image->levels, index);
		extent = intersect_region(extent, (RasterExtent){0, 0, level->width, level->height});
		if (empty_extent(extent))
			return;
		if (index > 0)
			reduce_region(g_ptr_array_index(image->levels, index - 1), level, extent);
		/* A changed first column or row is also the overlap of the tile before it */
		for (int row = MAX(0, extent.y - 1) / TILE_SIZE; row <= (extent.y + extent.height - 1) / TILE_SIZE; row++)
			for (int column = MAX(0, extent.x - 1) / TILE_SIZE; column <= (extent.x + extent.width - 1) / TILE_SIZE;
				 column++)
				g_clear_object(&level->tiles[row * level->columns + column]);
		int right = extent.x + extent.width, bottom = extent.y + extent.height;
		extent =
			(RasterExtent){extent.x / 2, extent.y / 2, (right + 1) / 2 - extent.x / 2, (bottom + 1) / 2 - extent.y / 2};
	}
}

/* Tiles share the level memory, so creating one copies nothing and uploads only that tile */
static GdkTexture *get_tile(TileLevel *level, int column, int row) {
	GdkTexture **tile = &level->tiles[row * level->columns + column];
	if (*tile)
		return *tile;
	int x = column * TILE_SIZE, y = row * TILE_SIZE;
	int width = MIN(TILE_SIZE + 1, level->width - x), height = MIN(TILE_SIZE + 1, level->height - y);
	gsize offset = (gsize)y * level->stride + x * 4, length = (gsize)(height - 1) * level->stride + width * 4;
	g_autoptr(GBytes) bytes = g_bytes_new_from_bytes(level->pixels, offset, length);
	g_autoptr(GdkMemoryTextureBuilder) builder = gdk_memory_texture_builder_new();
	gdk_memory_texture_builder_set_bytes(builder, bytes);
	gdk_memory_texture_builder_set_stride(builder, level->stride);
	gdk_memory_texture_builder_set_width(builder, width);
	gdk_memory_texture_builder_set_height(builder, height);
	gdk_memory_texture_builder_set_format(builder, GDK_MEMORY_DEFAULT);
	*tile = gdk_memory_texture_builder_build(builder);
	return *tile;
}

/* The level nearest above the zoom keeps zoomed out views sharp and their tile count small */
void snapshot_tiled_image(TiledImage *image, GtkSnapshot *snapshot, const graphene_rect_t *bounds,
						  const graphene_rect_t *visible, double zoom, gboolean smooth, gboolean transformed) {
	if (!image || zoom <= 0)
		return;
	guint chosen = 0;
	while (chosen + 1 < image->levels->len && zoom * (1 << (chosen + 1)) <= 1)
		chosen++;
	TileLevel *level = g_ptr_array_index(image->levels, chosen);
	TileLevel *base = g_ptr_array_index(image->levels, 0);
	double scale_x = bounds->size.width / base->width * (1 << chosen);
	double scale_y = bounds->size.height / base->height * (1 << chosen);
	int first_column = MAX(0, (int)floor((visible->origin.x - bounds->origin.x) / scale_x / TILE_SIZE));
	int first_row = MAX(0, (int)floor((visible->origin.y - bounds->origin.y) / scale_y / TILE_SIZE));
	int last_column = MIN(level->columns - 1, (int)floor((visible->origin.x + visible->size.width - bounds->origin.x) /
														 scale_x / TILE_SIZE));
	int last_row = MIN(level->rows - 1,
					   (int)floor((visible->origin.y + visible->size.height - bounds->origin.y) / scale_y / TILE_SIZE));
	GskScalingFilter filter = !smooth ? GSK_SCALING_FILTER_NEAREST : GSK_SCALING_FILTER_LINEAR;
	for (int row = first_row; row <= last_row; row++)
		for (int column = first_column; column <= last_column; column++) {
			/* Each tile also holds the first column and row of its neighbours, so tiles overlap by real pixels and
			 * leave no seam even when a transform keeps their edges off whole screen pixels */
			double left = bounds->origin.x + column * TILE_SIZE * scale_x;
			double top = bounds->origin.y + row * TILE_SIZE * scale_y;
			double right = bounds->origin.x + MIN((column + 1) * TILE_SIZE + 1, level->width) * scale_x;
			double bottom = bounds->origin.y + MIN((row + 1) * TILE_SIZE + 1, level->height) * scale_y;
			if (!transformed) {
				left = round(left);
				top = round(top);
				right = round(right);
				bottom = round(bottom);
			}
			if (right <= left || bottom <= top)
				continue;
			gtk_snapshot_append_scaled_texture(snapshot, get_tile(level, column, row), filter,
											   &GRAPHENE_RECT_INIT(left, top, right - left, bottom - top));
		}
}

/* The base image is drawn around the patch rectangle and the patch inside it, as one image of the given size */
void snapshot_patched_image(TiledImage *base, TiledImage *patch, RasterExtent patch_extent, int width, int height,
							GtkSnapshot *snapshot, const graphene_rect_t *bounds, const graphene_rect_t *visible,
							double zoom, gboolean smooth) {
	double scale_x = bounds->size.width / width, scale_y = bounds->size.height / height;
	graphene_rect_t place =
		GRAPHENE_RECT_INIT(bounds->origin.x + patch_extent.x * scale_x, bounds->origin.y + patch_extent.y * scale_y,
						   patch_extent.width * scale_x, patch_extent.height * scale_y);
	gboolean whole =
		patch_extent.x <= 0 && patch_extent.y <= 0 && patch_extent.width >= width && patch_extent.height >= height;
	if (!whole) {
		g_autoptr(GskPathBuilder) builder = gsk_path_builder_new();
		gsk_path_builder_add_rect(builder, visible);
		gsk_path_builder_add_rect(builder, &place);
		g_autoptr(GskPath) outside = gsk_path_builder_to_path(builder);
		gtk_snapshot_push_fill(snapshot, outside, GSK_FILL_RULE_EVEN_ODD);
		snapshot_tiled_image(base, snapshot, bounds, visible, zoom, smooth, FALSE);
		gtk_snapshot_pop(snapshot);
	}
	if (patch)
		snapshot_tiled_image(patch, snapshot, &place, visible, zoom, smooth, FALSE);
}
