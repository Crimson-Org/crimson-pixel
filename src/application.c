#include <libpanel.h>

#include "application.h"
#include "common/log.h"
#include "editor.h"
#include "../lang/en.h"

static void free_editor(gpointer data) {
	Editor *editor = data;
	g_ptr_array_unref(editor->requests);
	g_ptr_array_unref(editor->closed_documents);
	g_clear_pointer(&editor->clip_pixels, free_record);
	g_free(editor->layer_rows);
	g_clear_pointer(&editor->swatches, g_array_unref);
	g_free(editor);
}

static Editor *get_editor(GtkApplication *application) {
	GList *windows = gtk_application_get_windows(application);
	for (GList *item = windows; item; item = item->next) {
		Editor *editor = g_object_get_data(G_OBJECT(item->data), "editor");
		if (editor && !editor->closing)
			return editor;
	}
	Editor *editor = g_new0(Editor, 1);
	editor->application = application;
	editor->requests = g_ptr_array_new_with_free_func(g_object_unref);
	editor->closed_documents = g_ptr_array_new_with_free_func((GDestroyNotify)free_presented_document);
	build_editor_window(editor);
	note_startup_moment("Window created");
	note_first_frame(editor->window);
	g_object_set_data_full(G_OBJECT(editor->window), "editor", editor, free_editor);
	start_document_recovery(editor);
	return editor;
}

static void load_file(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	GFile *file = data;
	g_autoptr(GError) error = NULL;
	Document *document = load_document(file, cancellable, &error);
	if (!document) {
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	note_startup_moment("Document decoded");
	cairo_surface_t *surface = render_surface(document, &error);
	note_startup_moment("Document drawn");
	if (!surface) {
		free_document(document);
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	PresentedDocument *presented = g_new0(PresentedDocument, 1);
	presented->document = document;
	presented->surface = surface;
	presented->tiles = create_tiled_image(surface);
	/* Formats Save can write back stay attached to their file, others save as a new Photoshop file */
	presented->format = find_file_format(file, TRUE);
	if (presented->format)
		presented->file = g_object_ref(file);
	g_task_return_pointer(task, presented, (GDestroyNotify)free_presented_document);
}

/* Marks loading tasks whose data is the opened file, and those reopening a crashed session's document */
static char OPENED_FILE_TAG[] = "opened file", RECOVERED_FILE_TAG[] = "recovered file";

static void finish_open(GObject *source, GAsyncResult *result, gpointer data) {
	Editor *editor = g_object_get_data(source, "editor");
	g_autoptr(GError) error = NULL;
	PresentedDocument *presented = g_task_propagate_pointer(G_TASK(result), &error);
	g_ptr_array_remove(editor->requests, g_task_get_cancellable(G_TASK(result)));
	if (!editor->closing)
		refresh_loading_state(editor);
	gboolean recovered = g_task_get_source_tag(G_TASK(result)) == RECOVERED_FILE_TAG;
	if (editor->closing)
		g_clear_pointer(&presented, free_presented_document);
	if (recovered)
		adopt_recovered_document(editor, g_task_get_task_data(G_TASK(result)), presented);
	if (editor->closing)
		return;
	if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		g_message("Document open failed: %s", error->message);
	}
	/* New documents share this finish but carry their creation request instead of a file */
	if (presented && g_task_get_source_tag(G_TASK(result)) == OPENED_FILE_TAG)
		remember_recent_file(g_task_get_task_data(G_TASK(result)));
	finish_loading_page(editor, g_object_get_data(G_OBJECT(result), "loading-page"), presented);
}

/* The task keeps its tab until it finishes, even when the tab is closed meanwhile */
static void add_task_page(Editor *editor, GTask *task, const char *name) {
	GtkWidget *page = add_loading_page(editor, name, g_task_get_cancellable(task));
	g_object_set_data_full(G_OBJECT(task), "loading-page", g_object_ref(page), g_object_unref);
}

static void start_file_load(Editor *editor, GFile *file, gpointer tag) {
	GCancellable *cancellable = g_cancellable_new();
	g_ptr_array_add(editor->requests, cancellable);
	g_autoptr(GTask) task = g_task_new(editor->window, cancellable, finish_open, NULL);
	g_task_set_source_tag(task, tag);
	g_task_set_task_data(task, g_object_ref(file), g_object_unref);
	g_autofree char *name = g_file_get_basename(file);
	add_task_page(editor, task, name);
	refresh_loading_state(editor);
	g_task_run_in_thread(task, load_file);
}

void open_editor_file(Editor *editor, GFile *file) {
	if (!open_swatch_file(editor, file))
		start_file_load(editor, file, OPENED_FILE_TAG);
}

void open_recovered_file(Editor *editor, GFile *file) {
	start_file_load(editor, file, RECOVERED_FILE_TAG);
}

/* A new document request is immutable while channel allocation runs in a worker */
typedef struct {
	int width, height;
	unsigned depth;
	DocumentMode mode;
	char *name;
} NewDocumentRequest;

static void free_new_request(gpointer data) {
	NewDocumentRequest *request = data;
	g_free(request->name);
	g_free(request);
}

static void create_requested_model(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	NewDocumentRequest *request = data;
	g_autoptr(GError) error = NULL;
	if (g_task_return_error_if_cancelled(task))
		return;
	Document *document = create_transparent_document(request->width, request->height, request->depth, request->mode,
													 request->name, &error);
	if (!document) {
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	cairo_surface_t *surface = render_surface(document, &error);
	if (!surface) {
		free_document(document);
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	PresentedDocument *presented = g_new0(PresentedDocument, 1);
	presented->document = document;
	presented->surface = surface;
	presented->tiles = create_tiled_image(surface);
	g_task_return_pointer(task, presented, (GDestroyNotify)free_presented_document);
}

void create_editor_document(Editor *editor, int width, int height, unsigned depth, DocumentMode mode,
							const char *name) {
	NewDocumentRequest *request = g_new0(NewDocumentRequest, 1);
	*request = (NewDocumentRequest){width, height, depth, mode, g_strdup(name)};
	GCancellable *cancellable = g_cancellable_new();
	g_ptr_array_add(editor->requests, cancellable);
	g_autoptr(GTask) task = g_task_new(editor->window, cancellable, finish_open, NULL);
	g_task_set_task_data(task, request, free_new_request);
	add_task_page(editor, task, request->name);
	refresh_loading_state(editor);
	g_task_run_in_thread(task, create_requested_model);
}

static void activate(GApplication *application) {
	Editor *editor = get_editor(GTK_APPLICATION(application));
	gtk_window_present(GTK_WINDOW(editor->window));
}

static void open_files(GApplication *application, GFile **files, int count, const char *hint) {
	Editor *editor = get_editor(GTK_APPLICATION(application));
	for (int index = 0; index < count; index++)
		open_editor_file(editor, files[index]);
	gtk_window_present(GTK_WINDOW(editor->window));
}

/* libpanel supplies the docking panels and initializes libadwaita with them */
static void start_up(GApplication *application) {
	panel_init();
}

static int handle_options(GApplication *application, GVariantDict *options) {
	if (g_variant_dict_contains(options, "new-window"))
		g_application_set_flags(application, g_application_get_flags(application) | G_APPLICATION_NON_UNIQUE);
	return -1;
}

static const GOptionEntry OPTIONS[] = {
	{"new-window", 'n', 0, G_OPTION_ARG_NONE, NULL, TEXT_NEW_WINDOW_OPTION, NULL},
	G_OPTION_ENTRY_NULL,
};

const ApplicationSetup APPLICATION_SETUP = {
	.identity = {"io.github.CrimsonOrg.CrimsonPixel", TEXT_APPLICATION_NAME, "crimson-pixel", APPLICATION_VERSION},
	.start_up = start_up,
	.activate = activate,
	.open = open_files,
	.handle_options = handle_options,
	.options = OPTIONS,
	.parameter_summary = TEXT_FILES_ARGUMENT,
};
