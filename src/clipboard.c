#include "editor.h"
#include "layer-commands.h"
#include "pixel-selection.h"
#include "../lang/en.h"

/* Copies go to the desktop clipboard as an image so other applications can paste them, and Crimson Pixel keeps its
 * own copy with the original position for pasting back here */

static void refresh_after_change(Editor *editor, GtkWidget *page) {
	refresh_selection_display(get_page_view(page, "primary"));
	refresh_selection_display(get_page_view(page, "secondary"));
	refresh_document_page(editor, page, TRUE);
}

void copy_to_clipboard(Editor *editor, gboolean cut, gboolean merged) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented)
		return;
	Document *document = presented->document;
	g_autoptr(GError) error = NULL;
	RasterExtent extent;
	Record *pixels = copy_selection_pixels(document, merged, &extent, &error);
	if (!pixels) {
		if (error)
			gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	g_clear_pointer(&editor->clip_pixels, free_record);
	editor->clip_pixels = pixels;
	editor->clip_extent = extent;
	editor->clip_document = document;
	g_autoptr(GBytes) bytes = g_bytes_new(pixels->value.buffer.data, pixels->value.buffer.length);
	g_autoptr(GdkTexture) texture =
		gdk_memory_texture_new(extent.width, extent.height, GDK_MEMORY_R8G8B8A8, bytes, (gsize)extent.width * 4);
	gdk_clipboard_set_texture(gtk_widget_get_clipboard(editor->window), texture);
	if (cut && clear_selection_pixels(document, FALSE, &error))
		refresh_after_change(editor, page);
	else if (error)
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
}

void clear_selected_area(Editor *editor) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented)
		return;
	g_autoptr(GError) error = NULL;
	if (clear_selection_pixels(presented->document, FALSE, &error))
		refresh_after_change(editor, page);
	else if (error)
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
}

static Record *read_texture_pixels(GdkTexture *texture) {
	int width = gdk_texture_get_width(texture), height = gdk_texture_get_height(texture);
	Record *pixels = create_channels((gsize)width * height * 4, 8);
	g_autoptr(GdkTextureDownloader) downloader = gdk_texture_downloader_new(texture);
	gdk_texture_downloader_set_format(downloader, GDK_MEMORY_R8G8B8A8);
	gdk_texture_downloader_download_into(downloader, pixels->value.buffer.data, (gsize)width * 4);
	return pixels;
}

/* Without an open document the pasted image becomes a new one */
static void open_pasted_image(Editor *editor, GdkTexture *texture) {
	int width = gdk_texture_get_width(texture), height = gdk_texture_get_height(texture);
	Record *pixels = read_texture_pixels(texture);
	g_autoptr(GBytes) bytes = g_bytes_new(pixels->value.buffer.data, pixels->value.buffer.length);
	free_record(pixels);
	g_autoptr(GdkPixbuf) pixbuf =
		gdk_pixbuf_new_from_bytes(bytes, GDK_COLORSPACE_RGB, TRUE, 8, width, height, width * 4);
	g_autoptr(GError) error = NULL;
	Document *document = create_document_from_pixbuf(pixbuf, TEXT_PASTED_DOCUMENT, &error);
	cairo_surface_t *surface = document ? render_surface(document, &error) : NULL;
	if (!surface) {
		if (document)
			free_document(document);
		if (error)
			gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	PresentedDocument *presented = g_new0(PresentedDocument, 1);
	presented->document = document;
	presented->surface = surface;
	presented->tiles = create_tiled_image(surface);
	install_document(editor, presented);
}

static void paste_pixels_here(Editor *editor, Record *pixels, RasterExtent extent, gboolean same_document) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented)
		return;
	g_autoptr(GError) error = NULL;
	if (paste_pixels(presented->document, pixels, extent, same_document, FALSE, &error))
		refresh_after_change(editor, page);
	else if (error)
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
}

static void finish_paste(GObject *source, GAsyncResult *result, gpointer data) {
	Editor *editor = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GdkTexture) texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source), result, &error);
	if (!texture) {
		gtk_label_set_text(GTK_LABEL(editor->status), TEXT_CLIPBOARD_NO_IMAGE);
		return;
	}
	if (!get_page_document(get_current_page(editor))) {
		open_pasted_image(editor, texture);
		return;
	}
	RasterExtent extent = {0, 0, gdk_texture_get_width(texture), gdk_texture_get_height(texture)};
	paste_pixels_here(editor, read_texture_pixels(texture), extent, FALSE);
}

void paste_from_clipboard(Editor *editor) {
	GdkClipboard *clipboard = gtk_widget_get_clipboard(editor->window);
	PresentedDocument *presented = get_page_document(get_current_page(editor));
	gboolean image = gdk_content_formats_contain_gtype(gdk_clipboard_get_formats(clipboard), GDK_TYPE_TEXTURE);
	if (gdk_clipboard_is_local(clipboard) && image && editor->clip_pixels && presented) {
		paste_pixels_here(editor, copy_record(editor->clip_pixels), editor->clip_extent,
						  editor->clip_document == presented->document);
		return;
	}
	gdk_clipboard_read_texture_async(clipboard, NULL, finish_paste, editor);
}
