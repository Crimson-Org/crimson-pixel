#include "editor.h"
#include "history.h"
#include "psd.h"
#include "paint.h"
#include "../lang/en.h"

/* One write holds its pixels or bytes and its target so the window may close meanwhile */
typedef struct {
	GBytes *bytes;
	GdkPixbuf *pixbuf;
	char *format;
	GFile *file;
	/* The saved tab, or nothing for an export */
	GtkWidget *page;
} WriteRequest;

static void free_write_request(WriteRequest *request) {
	g_clear_pointer(&request->bytes, g_bytes_unref);
	g_clear_object(&request->pixbuf);
	g_free(request->format);
	g_object_unref(request->file);
	g_clear_object(&request->page);
	g_free(request);
}

const char *find_file_format(GFile *file, gboolean saved_in_place) {
	g_autofree char *name = g_file_get_basename(file);
	g_autofree char *lowercase = g_ascii_strdown(name, -1);
	if (g_str_has_suffix(lowercase, ".psd") || g_str_has_suffix(lowercase, ".psb"))
		return "psd";
	if (g_str_has_suffix(lowercase, ".jpg") || g_str_has_suffix(lowercase, ".jpeg"))
		return "jpeg";
	if (g_str_has_suffix(lowercase, ".webp"))
		return "webp";
	if (g_str_has_suffix(lowercase, ".bmp"))
		return "bmp";
	if (g_str_has_suffix(lowercase, ".png"))
		return "png";
	if (!saved_in_place && (g_str_has_suffix(lowercase, ".tif") || g_str_has_suffix(lowercase, ".tiff")))
		return "tiff";
	return saved_in_place ? NULL : "png";
}

static void write_file(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	WriteRequest *request = data;
	GError *error = NULL;
	if (request->pixbuf) {
		char *encoded;
		gsize length;
		gboolean done =
			!strcmp(request->format, "jpeg")
				? gdk_pixbuf_save_to_buffer(request->pixbuf, &encoded, &length, "jpeg", &error, "quality", "92", NULL)
				: gdk_pixbuf_save_to_buffer(request->pixbuf, &encoded, &length, request->format, &error, NULL);
		if (!done) {
			g_task_return_error(task, error);
			return;
		}
		request->bytes = g_bytes_new_take(encoded, length);
	}
	gsize length;
	const char *bytes = g_bytes_get_data(request->bytes, &length);
	/* Replacing writes a temporary file and renames it, so a failed write keeps the old file intact */
	if (g_file_replace_contents(request->file, bytes, length, NULL, FALSE, G_FILE_CREATE_NONE, NULL, cancellable,
								&error))
		g_task_return_boolean(task, TRUE);
	else
		g_task_return_error(task, error);
}

static void finish_write(GObject *source, GAsyncResult *result, gpointer data) {
	Editor *editor = g_object_get_data(source, "editor");
	WriteRequest *request = g_task_get_task_data(G_TASK(result));
	g_autoptr(GError) error = NULL;
	if (!g_task_propagate_boolean(G_TASK(result), &error)) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	g_autofree char *name = g_file_get_basename(request->file);
	g_autofree char *message = g_strdup_printf(request->page ? TEXT_SAVED : TEXT_EXPORTED, name);
	gtk_label_set_text(GTK_LABEL(editor->status), message);
	if (!request->page)
		return;
	/* The document may have changed while writing, so only the written state counts as saved */
	remember_recent_file(request->file);
	if (gtk_widget_get_parent(request->page))
		refresh_document_page(editor, request->page, FALSE);
}

static void start_write(Editor *editor, WriteRequest *request, const char *status) {
	g_autoptr(GTask) task = g_task_new(editor->window, NULL, finish_write, NULL);
	g_task_set_task_data(task, request, (GDestroyNotify)free_write_request);
	gtk_label_set_text(GTK_LABEL(editor->status), status);
	g_task_run_in_thread(task, write_file);
}

/* Formats without transparency get the composite over white */
static GdkPixbuf *create_format_pixbuf(Document *document, const char *format, GError **error) {
	return create_document_pixbuf(document, !strcmp(format, "jpeg") || !strcmp(format, "bmp"), error);
}

void export_editor_document(Editor *editor, GFile *file) {
	PresentedDocument *presented = get_page_document(get_current_page(editor));
	if (!presented)
		return;
	const char *format = find_file_format(file, FALSE);
	g_autoptr(GError) error = NULL;
	GdkPixbuf *pixbuf = create_format_pixbuf(presented->document, format, &error);
	if (!pixbuf) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	WriteRequest *request = g_new0(WriteRequest, 1);
	*request = (WriteRequest){NULL, pixbuf, g_strdup(format), g_object_ref(file), NULL};
	start_write(editor, request, TEXT_EXPORTING);
}

static void finish_export_dialog(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GError) error = NULL;
	g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
	if (file)
		export_editor_document(data, file);
}

void show_export_dialog(Editor *editor) {
	PresentedDocument *presented = get_page_document(get_current_page(editor));
	if (!presented)
		return;
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, TEXT_EXPORT_AS);
	const char *name = get_string(presented->document->record, "name");
	g_autofree char *stem = g_strdup(name ? name : TEXT_UNTITLED);
	char *dot = strrchr(stem, '.');
	if (dot && dot != stem)
		*dot = 0;
	g_autofree char *suggestion = g_strconcat(stem, ".png", NULL);
	gtk_file_dialog_set_initial_name(dialog, suggestion);
	g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	const char *labels[] = {TEXT_EXPORT_PNG, TEXT_EXPORT_JPEG, TEXT_EXPORT_WEBP};
	const char *patterns[][2] = {{"*.png", NULL}, {"*.jpg", "*.jpeg"}, {"*.webp", NULL}};
	for (guint index = 0; index < G_N_ELEMENTS(labels); index++) {
		g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
		gtk_file_filter_set_name(filter, labels[index]);
		for (guint pattern = 0; pattern < 2 && patterns[index][pattern]; pattern++)
			gtk_file_filter_add_pattern(filter, patterns[index][pattern]);
		g_list_store_append(filters, filter);
	}
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_save(dialog, GTK_WINDOW(editor->window), NULL, finish_export_dialog, editor);
}

/* ============================== Saving =========================================== */

static void save_to_file(Editor *editor, GtkWidget *page, GFile *file, const char *format) {
	PresentedDocument *presented = get_page_document(page);
	if (!presented)
		return;
	Document *document = presented->document;
	gboolean photoshop = !strcmp(format, "psd");
	prepare_document_for_save(document);
	g_autoptr(GError) error = NULL;
	GByteArray *bytes = photoshop ? write_psd_document(document, FALSE, FALSE, &error) : NULL;
	GdkPixbuf *pixbuf = photoshop ? NULL : create_format_pixbuf(document, format, &error);
	if (!bytes && !pixbuf) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	mark_document_saved(document, photoshop, g_get_real_time() / 1000);
	if (presented->file != file) {
		g_clear_object(&presented->file);
		presented->file = g_object_ref(file);
		presented->format = format;
		g_autofree char *name = g_file_get_basename(file);
		set_string(document->record, "name", name);
	}
	WriteRequest *request = g_new0(WriteRequest, 1);
	*request = (WriteRequest){bytes ? g_byte_array_free_to_bytes(bytes) : NULL, pixbuf, g_strdup(format),
							  g_object_ref(file), g_object_ref(page)};
	start_write(editor, request, TEXT_SAVING);
}

typedef struct {
	Editor *editor;
	GtkWidget *page;
} SaveDialogRequest;

static void finish_save_dialog(GObject *source, GAsyncResult *result, gpointer data) {
	SaveDialogRequest *request = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GFile) file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
	if (file && gtk_widget_get_parent(request->page))
		save_to_file(request->editor, request->page, file, "psd");
	g_object_unref(request->page);
	g_free(request);
}

/* Save writes back to the opened file in its own format, and documents without one ask for a Photoshop file */
void save_editor_document(Editor *editor, gboolean choose_file) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented)
		return;
	if (presented->file && !choose_file) {
		save_to_file(editor, page, presented->file, presented->format);
		return;
	}
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, TEXT_SAVE_AS_PSD);
	const char *name = get_string(presented->document->record, "name");
	g_autofree char *stem = g_strdup(name ? name : TEXT_UNTITLED);
	char *dot = strrchr(stem, '.');
	if (dot && dot != stem)
		*dot = 0;
	g_autofree char *suggestion = g_strconcat(stem, ".psd", NULL);
	gtk_file_dialog_set_initial_name(dialog, suggestion);
	g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, TEXT_PHOTOSHOP_FILTER);
	gtk_file_filter_add_pattern(filter, "*.psd");
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	SaveDialogRequest *request = g_new0(SaveDialogRequest, 1);
	*request = (SaveDialogRequest){editor, g_object_ref(page)};
	gtk_file_dialog_save(dialog, GTK_WINDOW(editor->window), NULL, finish_save_dialog, request);
}
