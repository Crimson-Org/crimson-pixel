#include <stdint.h>
#include <string.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "document.h"
#include "psd.h"
#include "../lang/en.h"

/* Decoder headers are bounded before allocating full resolution pixels */
static void check_dimensions(GdkPixbufLoader *loader, int width, int height, gpointer data) {
	gboolean *is_oversized = data;
	*is_oversized =
		width <= 0 || height <= 0 || width > 32768 || height > 32768 || (guint64)width * height > DOCUMENT_MAX_PIXELS;
	if (*is_oversized)
		gdk_pixbuf_loader_set_size(loader, 1, 1);
}

static Document *convert_pixels(GdkPixbuf *pixels, GCancellable *cancellable, GError **error) {
	int width = gdk_pixbuf_get_width(pixels), height = gdk_pixbuf_get_height(pixels);
	Document *document = create_document_record(TEXT_IMAGE_MODE, DOCUMENT_RASTER);
	set_number(document->record, "s", width);
	set_number(document->record, "T", height);
	g_autoptr(Record) layers = create_array();
	Record *layer = create_document_layer(document, FALSE);
	name_layer(layer, TEXT_IMAGE_MODE);
	set_record(layer, "V", create_extent(0, 0, width, height));
	Record *buffer = create_channels((gsize)width * height * 4, 8);
	if (!buffer) {
		free_record(layer);
		free_document(document);
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, TEXT_IMAGE_ALLOCATION_FAILED);
		return NULL;
	}
	set_record(layer, "buffer", buffer);
	append_record(layers, layer);
	int channels = gdk_pixbuf_get_n_channels(pixels);
	int stride = gdk_pixbuf_get_rowstride(pixels);
	const guint8 *source = gdk_pixbuf_read_pixels(pixels);
	guint8 *destination = buffer->value.buffer.data;
	for (int y = 0; y < height; y++) {
		if (g_cancellable_set_error_if_cancelled(cancellable, error)) {
			free_document(document);
			return NULL;
		}
		for (int x = 0; x < width; x++) {
			const guint8 *pixel = source + y * stride + x * channels;
			gsize offset = ((gsize)y * width + x) * 4;
			memcpy(destination + offset, pixel, 3);
			destination[offset + 3] = channels == 4 ? pixel[3] : 255;
		}
	}
	if (!replace_document_layers(document, layers, error)) {
		free_document(document);
		return NULL;
	}
	return document;
}

/* Pasting without an open document makes one from the clipboard image */
Document *create_document_from_pixbuf(GdkPixbuf *pixels, const char *name, GError **error) {
	Document *document = convert_pixels(pixels, NULL, error);
	if (document)
		set_string(document->record, "name", name);
	return document;
}

static gboolean accepts_loader_format(GdkPixbufLoader *loader, GError **error) {
	GdkPixbufFormat *format = gdk_pixbuf_loader_get_format(loader);
	if (!format)
		return TRUE;
	g_autofree char *name = gdk_pixbuf_format_get_name(format);
	const char *unsupported[] = {"psd", "psb", "ai", "pdf", "svg", "svgz", "ppx"};
	gboolean rejected = gdk_pixbuf_format_is_scalable(format);
	for (guint index = 0; index < G_N_ELEMENTS(unsupported); index++)
		if (g_str_equal(name, unsupported[index]))
			rejected = TRUE;
	if (!rejected)
		return TRUE;
	if (error)
		g_clear_error(error);
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_EDITABLE_IMPORT_UNAVAILABLE);
	return FALSE;
}

/* Photoshop files are decoded by the translated source reader instead of a raster decoder */
static Document *load_photoshop_document(GFile *file, const char *name, GCancellable *cancellable, GError **error) {
	char *contents;
	gsize length;
	if (!g_file_load_contents(file, cancellable, &contents, &length, NULL, error))
		return NULL;
	Document *document = read_psd_document((const guint8 *)contents, length, name, cancellable, error);
	g_free(contents);
	if (!document)
		return NULL;
	set_string(document->record, "name", name);
	Record *layers = reference_record(get_record(document->record, "Z"));
	set_record(document->record, "Z", create_array());
	gboolean replaced = replace_document_layers(document, layers, error);
	free_record(layers);
	if (!replaced) {
		free_document(document);
		return NULL;
	}
	return document;
}

Document *load_document(GFile *file, GCancellable *cancellable, GError **error) {
	g_autofree char *basename = g_file_get_basename(file);
	g_autofree char *lowercase = g_ascii_strdown(basename, -1);
	const char *editable_formats[] = {".ai", ".pdf", ".svg", ".svgz", ".ppx"};
	for (guint index = 0; index < G_N_ELEMENTS(editable_formats); index++) {
		if (g_str_has_suffix(lowercase, editable_formats[index])) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_EDITABLE_IMPORT_UNAVAILABLE);
			return NULL;
		}
	}
	g_autoptr(GFileInfo) info =
		g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
						  G_FILE_QUERY_INFO_NONE, cancellable, error);
	if (!info)
		return NULL;
	if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR || g_file_info_get_size(info) > 128 * 1024 * 1024) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_FILE_SIZE_LIMIT);
		return NULL;
	}
	g_autoptr(GFileInputStream) stream = g_file_read(file, cancellable, error);
	if (!stream)
		return NULL;
	guint8 signature[4] = {0};
	gsize signature_length = 0;
	if (!g_input_stream_read_all(G_INPUT_STREAM(stream), signature, sizeof signature, &signature_length, cancellable,
								 error))
		return NULL;
	if (signature_length == 4 && !memcmp(signature, "8BPS", 4))
		return load_photoshop_document(file, basename, cancellable, error);
	if (!g_seekable_seek(G_SEEKABLE(stream), 0, G_SEEK_SET, cancellable, error))
		return NULL;
	g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new();
	gboolean is_oversized = FALSE;
	g_signal_connect(loader, "size-prepared", G_CALLBACK(check_dimensions), &is_oversized);
	guchar buffer[65536];
	goffset total_bytes = 0;
	gssize bytes;
	while ((bytes = g_input_stream_read(G_INPUT_STREAM(stream), buffer, sizeof buffer, cancellable, error)) > 0) {
		total_bytes += bytes;
		if (total_bytes > 128 * 1024 * 1024 || is_oversized) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, TEXT_IMAGE_LIMIT);
			break;
		}
		gboolean written = gdk_pixbuf_loader_write(loader, buffer, bytes, error);
		if (!accepts_loader_format(loader, error)) {
			gdk_pixbuf_loader_close(loader, NULL);
			return NULL;
		}
		if (!written)
			break;
	}
	if ((error && *error) || is_oversized) {
		if (is_oversized && (!error || !*error))
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, TEXT_PIXEL_LIMIT);
		gdk_pixbuf_loader_close(loader, NULL);
		return NULL;
	}
	gboolean closed = gdk_pixbuf_loader_close(loader, error);
	if (!accepts_loader_format(loader, error) || !closed)
		return NULL;
	GdkPixbuf *pixels = gdk_pixbuf_loader_get_pixbuf(loader);
	if (!pixels) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, TEXT_NO_IMAGE);
		return NULL;
	}
	g_autoptr(GdkPixbuf) oriented = gdk_pixbuf_apply_embedded_orientation(pixels);
	Document *document = convert_pixels(oriented, cancellable, error);
	if (document) {
		set_string(document->record, "name", basename);
	}
	return document;
}
