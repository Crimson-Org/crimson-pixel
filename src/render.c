#include <math.h>

#include "document.h"
#include "../lang/en.h"
#include "kernels.h"
#include "blend.h"
#include "parallel.h"

guint32 premultiply_pixel(guint32 pixel) {
	guint alpha = pixel >> 24;
	return (alpha << 24) | (((((pixel >> 16) & 255) * alpha + 127) / 255) << 16) |
		   (((((pixel >> 8) & 255) * alpha + 127) / 255) << 8) | (((pixel & 255) * alpha + 127) / 255);
}

guint32 unpremultiply_pixel(guint32 pixel) {
	guint alpha = pixel >> 24;
	if (!alpha)
		return 0;
	return (alpha << 24) | (MIN(255, (((pixel >> 16) & 255) * 255 + alpha / 2) / alpha) << 16) |
		   (MIN(255, (((pixel >> 8) & 255) * 255 + alpha / 2) / alpha) << 8) |
		   MIN(255, ((pixel & 255) * 255 + alpha / 2) / alpha);
}

static guint32 read_display_pixel(const Record *channels, gsize pixel) {
	return (guint32)read_channel_byte(channels, pixel * 4 + 3) << 24 |
		   (guint32)read_channel_byte(channels, pixel * 4) << 16 |
		   (guint32)read_channel_byte(channels, pixel * 4 + 1) << 8 | read_channel_byte(channels, pixel * 4 + 2);
}

/* One display conversion, shared by the rows each thread converts */
typedef struct {
	const Record *buffer;
	guint32 *pixels;
	int stride, width, origin_x, origin_y;
	RasterExtent extent;
} DisplayWork;

static void convert_display_rows(int first, int last, gpointer data) {
	const DisplayWork *work = data;
	const Record *buffer = work->buffer;
	gboolean direct = buffer && buffer->type == RECORD_BUFFER && buffer->value.buffer.depth == 8;
	for (int y = first; y < last; y++) {
		guint32 *row = work->pixels + (gsize)(y - work->origin_y) * work->stride - work->origin_x;
		for (int x = work->extent.x; x < work->extent.x + work->extent.width; x++) {
			gsize index = (gsize)y * work->width + x;
			if (!direct || index * 4 + 3 >= buffer->value.buffer.length) {
				row[x] = premultiply_pixel(read_display_pixel(buffer, index));
				continue;
			}
			const guint8 *source = (const guint8 *)buffer->value.buffer.data + index * 4;
			guint32 pixel = (guint32)source[3] << 24 | (guint32)source[0] << 16 | (guint32)source[1] << 8 | source[2];
			row[x] = source[3] == 255 ? pixel : premultiply_pixel(pixel);
		}
	}
}

/* Eight-bit channels convert directly, skipping the multiply for opaque pixels; the target pixels start at the
 * origin, so a region can fill its own small surface or its place in a whole one */
static void convert_display_region(const Record *buffer, guint32 *pixels, int stride, int width, RasterExtent extent,
								   int origin_x, int origin_y) {
	DisplayWork work = {buffer, pixels, stride, width, origin_x, origin_y, extent};
	run_rows_in_parallel(extent.y, extent.height, extent.width, convert_display_rows, &work);
}

/* Imported files keep their saved composite, which is shown when live composition is not ported */
static gboolean has_saved_composite(const Document *document, int width, int height) {
	Record *buffer = get_record(document->record, "buffer");
	return document->unsupported_features && buffer && buffer->type == RECORD_BUFFER &&
		   buffer->value.buffer.length >= (gsize)width * height * 4;
}

cairo_surface_t *render_surface(Document *document, GError **error) {
	int width, height;
	if (!get_valid_document_dimensions(document, &width, &height)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, TEXT_INVALID_DIMENSIONS);
		return NULL;
	}
	gboolean empty_vector = document->mode == DOCUMENT_VECTOR && !count_records(get_record(document->record, "Z"));
	g_autoptr(Record) dirty = create_extent(0, 0, width, height);
	g_autoptr(GError) composition_error = NULL;
	document->preview = FALSE;
	if (!empty_vector && !compose_raster_document(document, dirty, NULL, &composition_error)) {
		if (!has_saved_composite(document, width, height)) {
			g_propagate_error(error, g_steal_pointer(&composition_error));
			return NULL;
		}
		document->preview = TRUE;
	}
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, TEXT_DISPLAY_ALLOCATION_FAILED);
		return NULL;
	}
	guint32 *pixels = (guint32 *)cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface) / 4;
	Record *buffer = get_record(document->record, "buffer");
	/* Presentation conversion never changes the authoritative straight channels */
	if (!empty_vector)
		convert_display_region(buffer, pixels, stride, width, (RasterExtent){0, 0, width, height}, 0, 0);
	cairo_surface_mark_dirty(surface);
	return surface;
}

/* Recompose only the painted region and refresh those presentation pixels */
gboolean update_surface_region(Document *document, cairo_surface_t *surface, RasterExtent extent, GError **error) {
	int width, height;
	if (!surface || !get_valid_document_dimensions(document, &width, &height))
		return FALSE;
	extent = intersect_region(extent, (RasterExtent){0, 0, width, height});
	if (empty_extent(extent))
		return TRUE;
	g_autoptr(Record) dirty = create_extent(extent.x, extent.y, extent.width, extent.height);
	if (!compose_raster_document(document, dirty, NULL, error))
		return FALSE;
	cairo_surface_flush(surface);
	guint32 *pixels = (guint32 *)cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface) / 4;
	Record *buffer = get_record(document->record, "buffer");
	convert_display_region(buffer, pixels, stride, width, extent, 0, 0);
	cairo_surface_mark_dirty_rectangle(surface, extent.x, extent.y, extent.width, extent.height);
	return TRUE;
}

/* An eight bit straight RGBA copy of the current composite for encoders */
GdkPixbuf *create_document_pixbuf(Document *document, gboolean opaque, GError **error) {
	int width, height;
	if (!get_valid_document_dimensions(document, &width, &height)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, TEXT_INVALID_DIMENSIONS);
		return NULL;
	}
	cairo_surface_t *surface = render_surface(document, error);
	if (!surface)
		return NULL;
	cairo_surface_destroy(surface);
	Record *buffer = get_record(document->record, "buffer");
	GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, !opaque, 8, width, height);
	if (!pixbuf) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, TEXT_DISPLAY_ALLOCATION_FAILED);
		return NULL;
	}
	guint8 *pixels = gdk_pixbuf_get_pixels(pixbuf);
	int stride = gdk_pixbuf_get_rowstride(pixbuf), channels = gdk_pixbuf_get_n_channels(pixbuf);
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++) {
			guint32 pixel = read_display_pixel(buffer, (gsize)y * width + x);
			guint8 *target = pixels + y * stride + x * channels;
			guint alpha = pixel >> 24;
			for (int channel = 0; channel < 3; channel++) {
				guint value = pixel >> (16 - channel * 8) & 255;
				/* Opaque formats show transparent areas over white */
				target[channel] = opaque ? (value * alpha + 255 * (255 - alpha) + 127) / 255 : value;
			}
			if (!opaque)
				target[3] = alpha;
		}
	return pixbuf;
}

/* ============================== Move preview ============================== */

static cairo_surface_t *create_region_surface(const Record *buffer, int width, RasterExtent region) {
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, region.width, region.height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	convert_display_region(buffer, (guint32 *)cairo_image_surface_get_data(surface),
						   cairo_image_surface_get_stride(surface) / 4, width, region, region.x, region.y);
	cairo_surface_mark_dirty(surface);
	return surface;
}

static cairo_surface_t *create_display_surface(const Record *buffer, int width, int height) {
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	convert_display_region(buffer, (guint32 *)cairo_image_surface_get_data(surface),
						   cairo_image_surface_get_stride(surface) / 4, width, (RasterExtent){0, 0, width, height}, 0,
						   0);
	cairo_surface_mark_dirty(surface);
	return surface;
}

/* Composes the region with only the chosen pixel layers visible into a surface of its size; NULL when none are */
static cairo_surface_t *compose_layers(Document *document, const gboolean *shown, guint count, RasterExtent region) {
	Record *layers = get_record(document->record, "Z");
	gboolean any = FALSE;
	for (guint index = 0; index < count; index++)
		any |= shown[index];
	if (!any)
		return NULL;
	g_autofree double *flags = g_new(double, count);
	for (guint index = 0; index < count; index++) {
		Record *layer = get_item(layers, index);
		flags[index] = get_number(layer, "NF");
		if (!is_layer_group(layer) && !shown[index])
			set_number(layer, "NF", (guint)flags[index] | 2);
	}
	g_autoptr(Record) dirty = create_extent(region.x, region.y, region.width, region.height);
	cairo_surface_t *surface = NULL;
	if (compose_raster_document(document, dirty, NULL, NULL))
		surface = create_region_surface(get_record(document->record, "buffer"), document_width(document), region);
	for (guint index = 0; index < count; index++)
		set_number(get_item(layers, index), "NF", flags[index]);
	return surface;
}

/* While layers move, the GPU draws what is below them, the moving pixels and what is above them, so the
 * document is composed again only on release. Floating pixels are drawn alone over their base layer. */
gboolean render_move_preview(Document *document, const GArray *moving, Record *floating, cairo_surface_t **below,
							 cairo_surface_t **moving_surface, cairo_surface_t **above, RasterExtent *below_extent,
							 RasterExtent *moving_extent) {
	/* TRUE with no moving surface means the whole current display moves */
	*below = *moving_surface = *above = NULL;
	RasterExtent canvas = {0, 0, document_width(document), document_height(document)};
	*below_extent = *moving_extent = canvas;
	Record *layers = get_record(document->record, "Z");
	guint count = count_records(layers);
	if (!moving->len || !count)
		return FALSE;
	int lowest = G_MAXINT;
	for (guint index = 0; index < moving->len; index++)
		lowest = MIN(lowest, g_array_index(moving, int, index));
	g_autofree gboolean *under = g_new0(gboolean, count), *over = g_new0(gboolean, count);
	g_autofree gboolean *chosen = g_new0(gboolean, count);
	for (guint index = 0; index < moving->len; index++)
		chosen[g_array_index(moving, int, index)] = TRUE;
	gboolean others = FALSE, covered = FALSE;
	for (guint index = 0; index < count; index++) {
		under[index] = floating ? (int)index <= lowest : (int)index < lowest && !chosen[index];
		over[index] = (int)index > lowest && !chosen[index];
		Record *layer = get_item(layers, index);
		gboolean shown = !is_layer_group(layer) && is_layer_visible(layer);
		if ((under[index] || over[index]) && shown)
			others = TRUE;
		if (over[index] && shown)
			covered = TRUE;
	}
	/* When only the moving layers are visible, the current display already is the moving part */
	if (!floating && !others)
		return TRUE;
	Record *saved = reference_record(get_record(document->record, "buffer"));
	set_record(document->record, "buffer", create_null());
	Record *layer = get_item(layers, lowest), *pixels = NULL, *extent = NULL;
	if (floating) {
		/* The floating layer shows its remaining base under the lifted pixels */
		pixels = reference_record(get_record(layer, "buffer"));
		extent = reference_record(get_record(layer, "V"));
		set_record(layer, "buffer", reference_record(get_record(floating, "Q6")));
		set_record(layer, "V", reference_record(get_record(floating, "DM")));
	}
	int width = canvas.width, height = canvas.height;
	/* Floating pixels with nothing above change only their own place, so the current display shows the rest */
	if (floating && !covered) {
		Record *offset = get_record(floating, "kv");
		RasterExtent place = read_extent(get_record(floating, "xX"));
		place.x += get_number(offset, "x");
		place.y += get_number(offset, "y");
		*below_extent = intersect_region(place, canvas);
	}
	if (!empty_extent(*below_extent))
		*below = compose_layers(document, under, count, *below_extent);
	*above = compose_layers(document, over, count, canvas);
	if (floating) {
		set_record(layer, "buffer", pixels);
		set_record(layer, "V", extent);
		/* The moving part only covers the lifted pixels, so a small selection stays cheap in a large document */
		RasterExtent from = read_extent(get_record(floating, "xX"));
		Record *offset = get_record(floating, "kv");
		from.x += get_number(offset, "x");
		from.y += get_number(offset, "y");
		RasterExtent to = intersect_region(from, (RasterExtent){0, 0, width, height});
		*moving_extent = to;
		Record *lifted = create_channels((gsize)MAX(0, to.width) * MAX(0, to.height) * 4, 8);
		const guint32 *source = get_record(floating, "Sl")->value.buffer.data;
		guint32 *target = lifted->value.buffer.data;
		for (int y = to.y; y < to.y + to.height; y++)
			memcpy(target + (gsize)(y - to.y) * to.width, source + (gsize)(y - from.y) * from.width + to.x - from.x,
				   (gsize)to.width * 4);
		if (!empty_extent(to))
			*moving_surface = create_display_surface(lifted, to.width, to.height);
		free_record(lifted);
	} else
		*moving_surface = compose_layers(document, chosen, count, canvas);
	set_record(document->record, "buffer", saved);
	return *moving_surface != NULL;
}
