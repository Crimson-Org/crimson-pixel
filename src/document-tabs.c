#include <adwaita.h>

#include "common/loading.h"
#include "editor.h"
#include "history.h"
#include "../lang/en.h"

void free_presented_document(PresentedDocument *presented) {
	if (!presented)
		return;
	forget_recovery_file(presented);
	free_document(presented->document);
	if (presented->surface)
		cairo_surface_destroy(presented->surface);
	free_tiled_image(presented->tiles);
	g_clear_pointer(&presented->selection_edges, g_array_unref);
	for (int part = 0; part < 3; part++)
		free_tiled_image(presented->move_preview[part]);
	g_free(presented->transform);
	g_clear_object(&presented->file);
	g_free(presented);
}

PresentedDocument *get_page_document(GtkWidget *page) {
	return page ? g_object_get_data(G_OBJECT(page), "presented-document") : NULL;
}

GtkWidget *get_current_page(Editor *editor) {
	GtkNotebook *notebook = GTK_NOTEBOOK(editor->notebook);
	int index = gtk_notebook_get_current_page(notebook);
	return index < 0 ? NULL : gtk_notebook_get_nth_page(notebook, index);
}

DocumentView *get_page_view(GtkWidget *page, const char *name) {
	return g_object_get_data(G_OBJECT(g_object_get_data(G_OBJECT(page), name)), "document-view");
}

void refresh_history_actions(Editor *editor, GtkWidget *page) {
	PresentedDocument *presented = get_page_document(page);
	Record *record = presented ? presented->document->record : NULL;
	g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(editor->window), "undo")),
								record && get_number(record, "NP") > 0);
	g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(editor->window), "redo")),
								record && get_number(record, "NP") + 1 < count_records(get_record(record, "history")));
}

void refresh_document_page(Editor *editor, GtkWidget *page, gboolean render) {
	PresentedDocument *presented = get_page_document(page);
	Document *document = presented->document;
	if (render) {
		g_autoptr(GError) error = NULL;
		cairo_surface_t *surface = render_surface(document, &error);
		if (presented->surface)
			cairo_surface_destroy(presented->surface);
		presented->surface = surface;
		free_tiled_image(presented->tiles);
		presented->tiles = create_tiled_image(surface);
		if (error)
			gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		else if (page == get_current_page(editor))
			refresh_document_status(editor, document);
		gtk_widget_queue_draw(g_object_get_data(G_OBJECT(page), "primary"));
		gtk_widget_queue_draw(g_object_get_data(G_OBJECT(page), "secondary"));
	}
	g_autofree char *name = g_strdup_printf("%s%s", get_string(document->record, "name"),
											is_document_modified(document, g_get_real_time() / 1000) ? " *" : "");
	gtk_label_set_text(g_object_get_data(G_OBJECT(page), "tab-label"), name);
	if (page != get_current_page(editor))
		return;
	refresh_history_actions(editor, page);
	refresh_editor_actions(editor);
	refresh_editor_dock(editor);
}

void update_document_region(Editor *editor, PresentedDocument *presented, RasterExtent extent) {
	if (empty_extent(extent))
		return;
	g_autoptr(GError) error = NULL;
	if (!update_surface_region(presented->document, presented->surface, extent, &error)) {
		if (error)
			gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	invalidate_tiled_image(presented->tiles, extent);
	GtkWidget *page = get_current_page(editor);
	gtk_widget_queue_draw(g_object_get_data(G_OBJECT(page), "primary"));
	gtk_widget_queue_draw(g_object_get_data(G_OBJECT(page), "secondary"));
	gtk_widget_queue_draw(editor->navigator);
}

void edit_document_history(Editor *editor, gboolean redo) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented)
		return;
	/* Undo while transforming drops the unapplied box first */
	if (presented->transform) {
		finish_transform(editor, FALSE);
		return;
	}
	Document *document = presented->document;
	document->changed = (RasterExtent){0};
	document->selection_changed = document->selection_only = FALSE;
	gboolean changed = redo ? redo_document_command(document) : undo_document_command(document);
	if (!changed)
		return;
	RasterExtent region = document->changed;
	gboolean selection = document->selection_changed, selection_only = document->selection_only;
	document->changed = (RasterExtent){0};
	document->selection_changed = document->selection_only = FALSE;
	if (selection) {
		refresh_selection_display(get_page_view(page, "primary"));
		refresh_selection_display(get_page_view(page, "secondary"));
	}
	/* Known regions redraw only their pixels, selection steps redraw nothing, and anything else redraws all */
	if (!empty_extent(region))
		update_document_region(editor, presented, region);
	refresh_document_page(editor, page, empty_extent(region) && !selection_only);
}

/* Native addition: closed documents stay in memory with their history, like closed browser tabs */
enum { CLOSED_DOCUMENTS_KEPT = 10 };

static void remove_editor_page(Editor *editor, GtkWidget *page) {
	int index = gtk_notebook_page_num(GTK_NOTEBOOK(editor->notebook), page);
	if (index < 0)
		return;
	PresentedDocument *presented = g_object_steal_data(G_OBJECT(page), "presented-document");
	/* A tab still loading stops its load and leaves nothing to reopen */
	if (!presented) {
		GCancellable *loading = g_object_get_data(G_OBJECT(page), "loading");
		if (loading)
			g_cancellable_cancel(loading);
		gtk_notebook_remove_page(GTK_NOTEBOOK(editor->notebook), index);
		return;
	}
	/* A closed document is no longer open work, so a crash must not bring it back, and an open transform is
	 * dropped the way cancelling it keeps the document */
	forget_recovery_file(presented);
	g_clear_pointer(&presented->transform, g_free);
	presented->closed_position = index;
	g_ptr_array_add(editor->closed_documents, presented);
	if (editor->closed_documents->len > CLOSED_DOCUMENTS_KEPT)
		g_ptr_array_remove_index(editor->closed_documents, 0);
	gtk_notebook_remove_page(GTK_NOTEBOOK(editor->notebook), index);
}

void reopen_closed_document(Editor *editor) {
	if (!editor->closed_documents->len)
		return;
	PresentedDocument *presented = g_ptr_array_steal_index(editor->closed_documents, editor->closed_documents->len - 1);
	install_document(editor, presented);
	gtk_notebook_reorder_child(GTK_NOTEBOOK(editor->notebook), get_current_page(editor), presented->closed_position);
}

/* The window reference keeps pending close responses safe during asynchronous loading */
typedef struct {
	GtkWidget *window, *page;
} CloseRequest;

static void finish_close(GObject *source, GAsyncResult *result, gpointer data) {
	CloseRequest *request = data;
	Editor *editor = g_object_get_data(G_OBJECT(request->window), "editor");
	g_autoptr(GError) error = NULL;
	int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
	editor->close_prompt = FALSE;
	if (choice == 1 && request->page)
		remove_editor_page(editor, request->page);
	else if (choice == 1) {
		editor->closing = TRUE;
		gtk_window_close(GTK_WINDOW(editor->window));
	}
	g_clear_object(&request->page);
	g_object_unref(request->window);
	g_free(request);
}

gboolean confirm_editor_close(Editor *editor, GtkWidget *page) {
	if (editor->close_prompt)
		return TRUE;
	gboolean modified = FALSE;
	GtkNotebook *notebook = GTK_NOTEBOOK(editor->notebook);
	for (int index = 0; index < gtk_notebook_get_n_pages(notebook); index++) {
		GtkWidget *candidate = gtk_notebook_get_nth_page(notebook, index);
		PresentedDocument *presented = get_page_document(candidate);
		if ((page && candidate != page) || !presented)
			continue;
		modified |= is_document_modified(presented->document, g_get_real_time() / 1000);
	}
	if (!modified)
		return FALSE;
	editor->close_prompt = TRUE;
	CloseRequest *request = g_new0(CloseRequest, 1);
	request->window = g_object_ref(editor->window);
	request->page = page ? g_object_ref(page) : NULL;
	g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("%s", TEXT_DISCARD_CHANGES);
	gtk_alert_dialog_set_detail(dialog, TEXT_UNSAVED_EDITS);
	gtk_alert_dialog_set_buttons(dialog, (const char *[]){TEXT_CANCEL, TEXT_DISCARD, NULL});
	gtk_alert_dialog_set_cancel_button(dialog, 0);
	gtk_alert_dialog_set_default_button(dialog, 0);
	gtk_alert_dialog_choose(dialog, GTK_WINDOW(editor->window), NULL, finish_close, request);
	return TRUE;
}

void close_editor_page(Editor *editor, GtkWidget *page) {
	if (!page || confirm_editor_close(editor, page))
		return;
	remove_editor_page(editor, page);
}

static void show_close_button(GtkWidget *button, gboolean shown) {
	gtk_widget_set_opacity(button, shown ? 1 : 0);
	gtk_widget_set_can_target(button, shown);
}

static void reveal_close_button(GtkWidget *button) {
	show_close_button(button, TRUE);
}

static void conceal_close_button(GtkWidget *button) {
	show_close_button(button, FALSE);
}

static void close_tab(GtkButton *button, Editor *editor) {
	close_editor_page(editor, g_object_get_data(G_OBJECT(button), "page"));
}

/* A middle click on a tab closes it, as in browsers */
static void click_tab_middle(GtkGestureClick *gesture, int presses, double x, double y, GtkWidget *page) {
	close_editor_page(g_object_get_data(G_OBJECT(page), "editor"), page);
}

static void change_view(DocumentView *view, gpointer data) {
	Editor *editor = data;
	GtkWidget *page = get_current_page(editor);
	if (!page || view != get_page_view(page, "primary"))
		return;
	g_autofree char *zoom = g_strdup_printf("%.4g%%", view->zoom * 100);
	gtk_label_set_text(GTK_LABEL(editor->zoom_status), zoom);
	gtk_widget_queue_draw(editor->navigator);
}

/* The tab names the document, keeps its close button space and closes on a middle click */
static GtkWidget *create_document_tab(GtkWidget *page, const char *name) {
	GtkWidget *tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	GtkWidget *label = gtk_label_new(name);
	g_object_set_data(G_OBJECT(page), "tab-label", label);
	gtk_widget_set_tooltip_text(label, name);
	const int maximum_name_characters = 30;
	if (name && g_utf8_strlen(name, -1) > maximum_name_characters) {
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_label_set_width_chars(GTK_LABEL(label), maximum_name_characters);
		gtk_label_set_max_width_chars(GTK_LABEL(label), maximum_name_characters);
	}
	gtk_box_append(GTK_BOX(tab), label);
	GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
	gtk_widget_set_tooltip_text(close, TEXT_CLOSE_DOCUMENT);
	g_object_set_data(G_OBJECT(close), "page", page);
	g_signal_connect(close, "clicked", G_CALLBACK(close_tab), g_object_get_data(G_OBJECT(page), "editor"));
	gtk_widget_add_css_class(close, "flat");
	gtk_box_append(GTK_BOX(tab), close);
	/* The close button keeps its space but appears only while the pointer is over the tab */
	show_close_button(close, FALSE);
	GtkEventController *hover = gtk_event_controller_motion_new();
	g_signal_connect_swapped(hover, "enter", G_CALLBACK(reveal_close_button), close);
	g_signal_connect_swapped(hover, "leave", G_CALLBACK(conceal_close_button), close);
	gtk_widget_add_controller(tab, hover);
	GtkGesture *middle = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(middle), GDK_BUTTON_MIDDLE);
	g_signal_connect(middle, "released", G_CALLBACK(click_tab_middle), page);
	gtk_widget_add_controller(tab, GTK_EVENT_CONTROLLER(middle));
	return tab;
}

void install_document(Editor *editor, PresentedDocument *presented) {
	/* Native improvement: History states in Preferences, 200 by default, instead of the browser's 20 */
	presented->document->history_limit = editor->settings.history_states;
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	g_object_set_data(G_OBJECT(page), "editor", editor);
	g_object_set_data_full(G_OBJECT(page), "presented-document", presented, (GDestroyNotify)free_presented_document);
	GtkWidget *views = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	GtkWidget *primary = create_document_view(presented), *secondary = create_document_view(presented);
	g_object_set_data(G_OBJECT(page), "primary", primary);
	g_object_set_data(G_OBJECT(page), "secondary", secondary);
	gtk_paned_set_start_child(GTK_PANED(views), primary);
	gtk_paned_set_end_child(GTK_PANED(views), secondary);
	gtk_widget_set_visible(secondary, FALSE);
	gtk_widget_set_vexpand(views, TRUE);
	gtk_box_append(GTK_BOX(page), views);
	DocumentView *views_to_watch[] = {get_page_view(page, "primary"), get_page_view(page, "secondary")};
	for (guint index = 0; index < G_N_ELEMENTS(views_to_watch); index++) {
		views_to_watch[index]->changed = change_view;
		views_to_watch[index]->changed_data = editor;
		views_to_watch[index]->editor = editor;
	}
	GtkWidget *tab = create_document_tab(page, get_string(presented->document->record, "name"));
	int index = gtk_notebook_append_page(GTK_NOTEBOOK(editor->notebook), page, tab);
	gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(editor->notebook), page, TRUE);
	gtk_notebook_set_current_page(GTK_NOTEBOOK(editor->notebook), index);
	refresh_document_page(editor, page, FALSE);
	change_view(get_page_view(page, "primary"), editor);
	gtk_widget_grab_focus(primary);
}

/* ============================== Loading tabs ==================================== */

/* A document's tab appears at once, and a spinner fades in only when loading takes a while */
GtkWidget *add_loading_page(Editor *editor, const char *name, GCancellable *loading) {
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	g_object_set_data(G_OBJECT(page), "editor", editor);
	g_object_set_data_full(G_OBJECT(page), "loading", g_object_ref(loading), g_object_unref);
	GtkWidget *spinner = create_loading_spinner();
	gtk_widget_set_vexpand(spinner, TRUE);
	gtk_box_append(GTK_BOX(page), spinner);
	int index = gtk_notebook_append_page(GTK_NOTEBOOK(editor->notebook), page, create_document_tab(page, name));
	gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(editor->notebook), page, TRUE);
	gtk_notebook_set_current_page(GTK_NOTEBOOK(editor->notebook), index);
	return page;
}

/* The loaded document takes the loading tab's place, or is dropped when that tab was closed meanwhile */
void finish_loading_page(Editor *editor, GtkWidget *page, PresentedDocument *presented) {
	GtkNotebook *notebook = GTK_NOTEBOOK(editor->notebook);
	int index = gtk_notebook_page_num(notebook, page);
	if (index < 0 || !presented) {
		if (index >= 0)
			gtk_notebook_remove_page(notebook, index);
		if (presented)
			free_presented_document(presented);
		return;
	}
	GtkWidget *shown = get_current_page(editor);
	install_document(editor, presented);
	GtkWidget *loaded = get_current_page(editor);
	fade_in_widget(loaded);
	gtk_notebook_reorder_child(notebook, loaded, index);
	gtk_notebook_remove_page(notebook, gtk_notebook_page_num(notebook, page));
	if (shown != page)
		gtk_notebook_set_current_page(notebook, gtk_notebook_page_num(notebook, shown));
}
