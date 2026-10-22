#include <string.h>

#include "common/key-store.h"
#include "common/window-geometry.h"
#include "editor.h"
#include "window-size.h"
#include "../lang/en.h"
#include "adjustments.h"
#include "layer-commands.h"
#include "layer-structure.h"
#include "pixel-selection.h"

#define WINDOW_STATE_FILE "window.ini"

static void remember_window_geometry(GtkWindow *window) {
	WindowGeometry geometry;
	read_window_geometry(window, &geometry);
	g_autofree char *path = get_state_file_path(WINDOW_STATE_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	store_window_geometry(file, &geometry);
	save_key_file(file, path);
}

static gboolean close_window(GtkWindow *window, Editor *editor) {
	if (!editor->closing && confirm_editor_close(editor, NULL))
		return TRUE;
	editor->closing = TRUE;
	remember_window_geometry(window);
	remember_workspace(editor);
	for (guint index = 0; index < editor->requests->len; index++)
		g_cancellable_cancel(g_ptr_array_index(editor->requests, index));
	return FALSE;
}

static void open_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	show_open_dialog(data);
}

static void open_recent_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	open_recent_uri(data, g_variant_get_string(parameter, NULL));
}

static void clear_recent_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	clear_recent_files();
}

static void export_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	show_export_dialog(data);
}

static void save_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	save_editor_document(data, FALSE);
}

static void save_as_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	save_editor_document(data, TRUE);
}

static void new_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	show_new_dialog(data);
}

static void quit_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Editor *editor = data;
	gtk_window_close(GTK_WINDOW(editor->window));
}

static void undo_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	edit_document_history(data, FALSE);
}

static void redo_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	edit_document_history(data, TRUE);
}

/* Layer menu commands take the document, the history policy and an error */
typedef gboolean (*LayerCommand)(Document *document, gboolean extended_history, GError **error);

static void run_layer_command(Editor *editor, LayerCommand command) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || presented->document->preview)
		return;
	g_autoptr(GError) error = NULL;
	gboolean changed = command(presented->document, FALSE, &error);
	if (error)
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
	if (!changed)
		return;
	refresh_selection_display(get_page_view(page, "primary"));
	refresh_selection_display(get_page_view(page, "secondary"));
	refresh_document_page(editor, page, TRUE);
}

static gboolean add_layer_above(Document *document, gboolean extended_history, GError **error) {
	return add_new_layer(document, FALSE, extended_history, error);
}

static gboolean copy_to_layer(Document *document, gboolean extended_history, GError **error) {
	return copy_selection_to_layer(document, FALSE, extended_history, error);
}

static gboolean cut_to_layer(Document *document, gboolean extended_history, GError **error) {
	return copy_selection_to_layer(document, TRUE, extended_history, error);
}

static gboolean merge_selected_layers(Document *document, gboolean extended_history, GError **error) {
	return merge_layers(document, MERGE_SELECTED, extended_history, error);
}

static gboolean merge_visible_layers(Document *document, gboolean extended_history, GError **error) {
	return merge_layers(document, MERGE_VISIBLE, extended_history, error);
}

static gboolean toggle_selected_clipping(Document *document, gboolean extended_history, GError **error) {
	return toggle_clipping_mask(document, extended_history, error);
}

static void clipping_mask_action(GSimpleAction *action, GVariant *state, gpointer data) {
	run_layer_command(data, toggle_selected_clipping);
	refresh_layer_menu_actions(data);
}

static void layer_color_action(GSimpleAction *action, GVariant *state, gpointer data) {
	Editor *editor = data;
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || presented->document->preview)
		return;
	g_autoptr(GError) error = NULL;
	if (!set_layer_color(presented->document, g_variant_get_int32(state), FALSE, &error))
		return;
	g_simple_action_set_state(action, state);
	refresh_document_page(editor, page, FALSE);
}

static void select_layer_pixels_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Editor *editor = data;
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	Record *selected = presented ? get_record(presented->document->record, "j") : NULL;
	if (!count_records(selected))
		return;
	if (!select_layer_pixels(presented->document, get_item(selected, 0)->value.number, FALSE))
		return;
	refresh_selection_display(get_page_view(page, "primary"));
	refresh_selection_display(get_page_view(page, "secondary"));
	refresh_document_page(editor, page, FALSE);
}

void refresh_layer_menu_actions(Editor *editor) {
	PresentedDocument *presented = get_page_document(get_current_page(editor));
	Document *document = presented ? presented->document : NULL;
	Record *selected = document ? get_record(document->record, "j") : NULL;
	int count = count_records(selected), first = count ? (int)get_item(selected, 0)->value.number : -1;
	Record *layer = count ? get_item(get_record(document->record, "Z"), first) : NULL;
	GActionMap *map = G_ACTION_MAP(editor->window);
	GSimpleAction *clipping = G_SIMPLE_ACTION(g_action_map_lookup_action(map, "clipping-mask"));
	g_simple_action_set_enabled(clipping, count == 1 && can_clip_layer(document, first));
	g_simple_action_set_state(clipping, g_variant_new_boolean(layer && get_boolean(layer, "jl")));
	gboolean mergeable =
		count && (count != 1 || first != 0) && (count != 1 || !is_layer_group(layer) || is_layer_visible(layer));
	g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(map, "merge-layers")), mergeable);
	g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(map, "select-layer-pixels")), count > 0);
	int color = layer ? (int)get_number(get_record(layer, "add"), "lclr") : 0;
	g_simple_action_set_state(G_SIMPLE_ACTION(g_action_map_lookup_action(map, "layer-color")),
							  g_variant_new_int32(CLAMP(color, 0, 7)));
}

static void group_layers_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_layer_command(data, group_selected_layers);
}

static void ungroup_layers_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_layer_command(data, ungroup_selected_layer);
}

static void merge_layers_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_layer_command(data, merge_selected_layers);
}

static void merge_visible_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_layer_command(data, merge_visible_layers);
}

static void flatten_image_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_layer_command(data, flatten_image);
}

static void layer_via_copy_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_layer_command(data, copy_to_layer);
}

static void layer_via_cut_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_layer_command(data, cut_to_layer);
}

/* Selection commands are available only while a selection exists conditions */
void refresh_editor_actions(Editor *editor) {
	PresentedDocument *presented = get_page_document(get_current_page(editor));
	gboolean selected = presented && get_document_selection(presented->document);
	const char *actions[] = {
		"deselect",		   "inverse",		"layer-via-cut", "transform-selection", "cut",			 "copy",
		"copy-merged",	   "clear",			"crop",			 "modify-border",		"modify-smooth", "modify-expand",
		"modify-contract", "modify-feather"};
	for (guint index = 0; index < G_N_ELEMENTS(actions); index++)
		g_simple_action_set_enabled(
			G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(editor->window), actions[index])), selected);
	refresh_layer_menu_actions(editor);
}

static void new_layer_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_layer_command(data, add_layer_above);
}

static void duplicate_layer_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_layer_command(data, duplicate_selected_layers);
}

static void delete_layer_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_layer_command(data, delete_selected_layers);
}

static void run_selection_command(Editor *editor, int command) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented)
		return;
	Document *document = presented->document;
	gboolean changed = command == 0	  ? select_all(document, FALSE)
					   : command == 1 ? deselect(document, FALSE)
									  : invert_selection(document, FALSE);
	if (!changed)
		return;
	refresh_selection_display(get_page_view(page, "primary"));
	refresh_selection_display(get_page_view(page, "secondary"));
	refresh_document_page(editor, page, FALSE);
}

static void snap_action(GSimpleAction *action, GVariant *state, gpointer data) {
	Editor *editor = data;
	const char *name = g_action_get_name(G_ACTION(action));
	gboolean value = g_variant_get_boolean(state);
	if (!strcmp(name, "snap"))
		editor->snap.enabled = editor->settings.snap = value;
	if (!strcmp(name, "snap-layers"))
		editor->snap.targets[SNAP_LAYERS] = editor->settings.snap_layers = value;
	if (!strcmp(name, "snap-bounds"))
		editor->snap.targets[SNAP_BOUNDS] = editor->settings.snap_bounds = value;
	g_simple_action_set_state(action, state);
	/* The View menu snap choices are kept between launches */
	g_autofree char *path = get_settings_path();
	save_settings(&editor->settings, path);
}

static void arrange_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Editor *editor = data;
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented)
		return;
	const char *name = g_action_get_name(G_ACTION(action));
	LayerArrangement arrangement = !strcmp(name, "arrange-front")	   ? ARRANGE_FRONT
								   : !strcmp(name, "arrange-forward")  ? ARRANGE_FORWARD
								   : !strcmp(name, "arrange-backward") ? ARRANGE_BACKWARD
																	   : ARRANGE_BACK;
	g_autoptr(GError) error = NULL;
	if (arrange_layers(presented->document, arrangement, FALSE, &error))
		refresh_document_page(editor, page, TRUE);
	else if (error)
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
}

static void crop_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	crop_to_selection(data);
}

static void trim_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	show_trim_dialog(data);
}

static void trim_top_left_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	trim_editor_document(data, TRIM_TOP_LEFT, (const gboolean[4]){TRUE, TRUE, TRUE, TRUE});
}

static void reveal_all_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	reveal_editor_document(data);
}

static void image_size_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	show_image_size_dialog(data);
}

static void canvas_size_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	show_canvas_size_dialog(data);
}

static void cut_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	copy_to_clipboard(data, TRUE, FALSE);
}

static void copy_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	copy_to_clipboard(data, FALSE, FALSE);
}

static void copy_merged_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	copy_to_clipboard(data, FALSE, TRUE);
}

static void paste_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	paste_from_clipboard(data);
}

static void clear_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	clear_selected_area(data);
}

static void free_transform_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	start_transform(data, FALSE);
}

static void transform_selection_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	start_transform(data, TRUE);
}

static void select_all_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_selection_command(data, 0);
}

static void deselect_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_selection_command(data, 1);
}

static void inverse_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	run_selection_command(data, 2);
}

static void modify_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	const char *names[] = {"modify-border", "modify-smooth", "modify-expand", "modify-contract", "modify-feather"};
	const char *name = g_action_get_name(G_ACTION(action));
	for (guint index = 0; index < G_N_ELEMENTS(names); index++)
		if (!strcmp(name, names[index]))
			show_modify_dialog(data, index);
}

static void fill_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	show_fill_dialog(data);
}

static void stroke_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	show_stroke_dialog(data);
}

static void fill_foreground_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	fill_with_editor_color(data, FALSE);
}

static void fill_background_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	fill_with_editor_color(data, TRUE);
}

static void close_document_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	close_editor_page(data, get_current_page(data));
}

static void new_adjustment_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Editor *editor = data;
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || presented->document->preview)
		return;
	const char *key = g_variant_get_string(parameter, NULL);
	g_autoptr(GError) error = NULL;
	if (!add_adjustment_layer(presented->document, key, FALSE, &error)) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	refresh_selection_display(get_page_view(page, "primary"));
	refresh_selection_display(get_page_view(page, "secondary"));
	refresh_document_page(editor, page, TRUE);
	if (strcmp(key, "nvrt"))
		show_adjustment_panel(editor, get_item(get_record(presented->document->record, "j"), 0)->value.number);
}

static void reopen_closed_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	reopen_closed_document(data);
}

/* View commands act on whichever split view has focus, otherwise the primary view */
static DocumentView *get_target_view(Editor *editor) {
	GtkWidget *page = get_current_page(editor);
	if (!page)
		return NULL;
	DocumentView *secondary = get_page_view(page, "secondary");
	return gtk_widget_has_focus(secondary->canvas) ? secondary : get_page_view(page, "primary");
}

static void zoom_in_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	DocumentView *view = get_target_view(data);
	if (view)
		step_view_zoom(view, TRUE);
}

static void zoom_out_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	DocumentView *view = get_target_view(data);
	if (view)
		step_view_zoom(view, FALSE);
}

static void fit_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	DocumentView *view = get_target_view(data);
	if (view)
		fit_document_view(view);
}

static void actual_pixels_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	DocumentView *view = get_target_view(data);
	if (view)
		show_actual_pixels(view);
}

static void split_action(GSimpleAction *action, GVariant *state, gpointer data) {
	GtkWidget *page = get_current_page(data);
	if (!page)
		return;
	g_simple_action_set_state(action, state);
	gtk_widget_set_visible(g_object_get_data(G_OBJECT(page), "secondary"), g_variant_get_boolean(state));
}

/* Pixel Grid and Tile Preview are viewing aids that both views of the document share */
static void view_aid_action(GSimpleAction *action, GVariant *state, gpointer data) {
	GtkWidget *page = get_current_page(data);
	if (!page)
		return;
	g_simple_action_set_state(action, state);
	gboolean grid = !g_strcmp0(g_action_get_name(G_ACTION(action)), "grid");
	const char *names[] = {"primary", "secondary"};
	for (guint index = 0; index < G_N_ELEMENTS(names); index++) {
		DocumentView *view = get_page_view(page, names[index]);
		if (grid)
			view->grid = g_variant_get_boolean(state);
		else
			view->tiled = g_variant_get_boolean(state);
		gtk_widget_queue_draw(view->canvas);
	}
}

static void set_action_enabled(Editor *editor, const char *name, gboolean enabled) {
	g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(editor->window), name)),
								enabled);
}

static void set_action_state(Editor *editor, const char *name, gboolean active) {
	GAction *action = g_action_map_lookup_action(G_ACTION_MAP(editor->window), name);
	g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(active));
}

static void switch_document(GtkNotebook *notebook, GtkWidget *page, guint index, Editor *editor) {
	PresentedDocument *presented = get_page_document(page);
	gtk_stack_set_visible_child_name(GTK_STACK(editor->documents), page ? "documents" : "start");
	const char *document_actions[] = {"close",
									  "export",
									  "save",
									  "save-as",
									  "fit",
									  "actual-pixels",
									  "split",
									  "grid",
									  "tile-preview",
									  "zoom-in",
									  "zoom-out",
									  "trim",
									  "trim-top-left",
									  "reveal-all",
									  "new-layer",
									  "duplicate-layer",
									  "delete-layer",
									  "select-all",
									  "deselect",
									  "inverse",
									  "layer-via-copy",
									  "layer-via-cut",
									  "free-transform",
									  "arrange-front",
									  "arrange-forward",
									  "arrange-backward",
									  "arrange-back",
									  "fill",
									  "stroke",
									  "fill-foreground",
									  "fill-background",
									  "image-size",
									  "canvas-size",
									  "group-layers",
									  "ungroup-layers",
									  "merge-layers",
									  "merge-visible",
									  "flatten-image",
									  "select-layer-pixels",
									  "clipping-mask",
									  "layer-color",
									  "new-adjustment"};
	for (guint action = 0; action < G_N_ELEMENTS(document_actions); action++)
		set_action_enabled(editor, document_actions[action], presented != NULL);
	set_action_enabled(editor, "reopen-closed", editor->closed_documents->len > 0);
	refresh_history_actions(editor, page);
	refresh_editor_actions(editor);
	if (!presented) {
		gtk_label_set_text(GTK_LABEL(editor->status), TEXT_EMPTY);
		gtk_label_set_text(GTK_LABEL(editor->zoom_status), "");
		refresh_editor_dock(editor);
		return;
	}
	set_action_state(editor, "split", gtk_widget_get_visible(g_object_get_data(G_OBJECT(page), "secondary")));
	set_action_state(editor, "grid", get_page_view(page, "primary")->grid);
	set_action_state(editor, "tile-preview", get_page_view(page, "primary")->tiled);
	gtk_label_set_text(GTK_LABEL(editor->status), "");
	refresh_document_status(editor, presented->document);
	DocumentView *view = get_page_view(page, "primary");
	g_autofree char *zoom = g_strdup_printf("%.4g%%", view->zoom * 100);
	gtk_label_set_text(GTK_LABEL(editor->zoom_status), zoom);
	refresh_editor_dock(editor);
}

void refresh_document_status(Editor *editor, Document *document) {
	g_autofree char *status =
		g_strdup_printf("%d × %d px · %u-bit", document_width(document), document_height(document),
						(unsigned)get_number(document->record, "depth"));
	if (document->preview) {
		GPtrArray *features = document->unsupported_features;
		g_autoptr(GString) names = g_string_new(NULL);
		for (guint index = 0; features && index < features->len; index++)
			g_string_append_printf(names, "%s%s", index ? ", " : "", (char *)g_ptr_array_index(features, index));
		g_autofree char *preview =
			names->len ? g_strdup_printf(TEXT_PREVIEW_FEATURES, names->str) : g_strdup(TEXT_PREVIEW_PROPERTIES);
		g_autofree char *combined = g_strdup_printf("%s · %s", status, preview);
		gtk_label_set_text(GTK_LABEL(editor->status), combined);
		return;
	}
	gtk_label_set_text(GTK_LABEL(editor->status), status);
}

static void remove_document(GtkNotebook *notebook, GtkWidget *page, guint index, Editor *editor) {
	switch_document(notebook, get_current_page(editor), 0, editor);
}

void refresh_loading_state(Editor *editor) {
	/* Recent files ignore further clicks while a file loads, so one click never opens it twice */
	if (editor->recent_list)
		gtk_widget_set_sensitive(editor->recent_list, editor->requests->len == 0);
}

static GtkWidget *create_status_bar(Editor *editor) {
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_margin_start(bar, 8);
	gtk_widget_set_margin_end(bar, 8);
	gtk_widget_set_margin_top(bar, 4);
	gtk_widget_set_margin_bottom(bar, 4);
	editor->zoom_status = gtk_label_new("");
	gtk_label_set_width_chars(GTK_LABEL(editor->zoom_status), 7);
	gtk_label_set_xalign(GTK_LABEL(editor->zoom_status), 0);
	gtk_box_append(GTK_BOX(bar), editor->zoom_status);
	editor->status = gtk_label_new(TEXT_EMPTY);
	gtk_label_set_xalign(GTK_LABEL(editor->status), 0);
	gtk_label_set_ellipsize(GTK_LABEL(editor->status), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(editor->status, TRUE);
	gtk_box_append(GTK_BOX(bar), editor->status);
	return bar;
}

/* Dropped files open as new documents like files passed on the command line */
static gboolean drop_files(GtkDropTarget *target, const GValue *value, double x, double y, Editor *editor) {
	if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
		return FALSE;
	for (GSList *item = g_value_get_boxed(value); item; item = item->next)
		open_editor_file(editor, item->data);
	return TRUE;
}

void queue_canvas_redraw(Editor *editor) {
	GtkWidget *page = get_current_page(editor);
	if (!page)
		return;
	/* The canvases themselves must redraw, since their containers reuse cached drawings */
	refresh_view_tool(get_page_view(page, "primary"));
	refresh_view_tool(get_page_view(page, "secondary"));
}

/* Space temporarily selects panning Hand tool */
static gboolean press_window_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType modifiers,
								 Editor *editor) {
	GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(editor->window));
	/* Keys meant for an open popover such as adjustment settings stay with it, so Escape closes it */
	GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
	if (event && gdk_event_get_surface(event) != gtk_native_get_surface(GTK_NATIVE(editor->window)))
		return FALSE;
	gboolean typing = focus && (GTK_IS_EDITABLE(focus) || gtk_widget_get_ancestor(focus, GTK_TYPE_SPIN_BUTTON));
	if (!typing && editor_transform(editor) &&
		(key == GDK_KEY_Return || key == GDK_KEY_KP_Enter || key == GDK_KEY_Escape)) {
		finish_transform(editor, key != GDK_KEY_Escape);
		return TRUE;
	}
	if (!typing && press_selection_path_key(editor, key, modifiers, TRUE))
		return TRUE;
	if (key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R) {
		editor->shift_held = TRUE;
		queue_canvas_redraw(editor);
	}
	if (key == GDK_KEY_space && !typing) {
		editor->space_held = TRUE;
		queue_canvas_redraw(editor);
		return TRUE;
	}

	/* Brush keys follow the physical keys right of P, which carry [ and ] on a US layout and Å and ¨ on a
	 * Swedish one, so layouts that need AltGr for brackets still get one-key brush sizing */
	enum { KEY_RIGHT_OF_P = 34, KEY_SECOND_RIGHT_OF_P = 35 };

	if (!typing && code == KEY_RIGHT_OF_P)
		key = GDK_KEY_bracketleft;
	if (!typing && code == KEY_SECOND_RIGHT_OF_P)
		key = GDK_KEY_bracketright;
	/* With Ctrl the same keys arrange layers, matching the bracket accelerators on any layout */
	if (!typing && (modifiers & GDK_CONTROL_MASK) && (code == KEY_RIGHT_OF_P || code == KEY_SECOND_RIGHT_OF_P)) {
		gboolean shift = modifiers & GDK_SHIFT_MASK, forward = code == KEY_SECOND_RIGHT_OF_P;
		g_action_group_activate_action(G_ACTION_GROUP(editor->window),
									   forward ? (shift ? "arrange-front" : "arrange-forward")
											   : (shift ? "arrange-back" : "arrange-backward"),
									   NULL);
		return TRUE;
	}
	/* Shift with the plus or minus key steps the blend mode, found by the unshifted key so every layout works */
	guint unshifted = 0;
	if (!typing &&
		(modifiers & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) == GDK_SHIFT_MASK &&
		gdk_display_translate_key(gtk_widget_get_display(editor->window), code, 0,
								  gtk_event_controller_key_get_group(GTK_EVENT_CONTROLLER_KEY(controller)), &unshifted,
								  NULL, NULL, NULL) &&
		(unshifted == GDK_KEY_plus || unshifted == GDK_KEY_equal || unshifted == GDK_KEY_KP_Add ||
		 unshifted == GDK_KEY_minus || unshifted == GDK_KEY_KP_Subtract)) {
		step_layer_blend_mode(editor, unshifted != GDK_KEY_minus && unshifted != GDK_KEY_KP_Subtract);
		return TRUE;
	}
	return press_tool_key(editor, key, modifiers);
}

static void release_window_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType modifiers,
							   Editor *editor) {
	press_selection_path_key(editor, key, modifiers, FALSE);
	if (key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R) {
		editor->shift_held = FALSE;
		queue_canvas_redraw(editor);
	}
	if (key != GDK_KEY_space)
		return;
	editor->space_held = FALSE;
	queue_canvas_redraw(editor);
}

/* A click on empty tab bar space opens files, like the empty start view */
static void click_tab_bar(GtkGestureClick *gesture, int presses, double x, double y, Editor *editor) {
	if (presses != 1)
		return;
	GtkWidget *picked = gtk_widget_pick(editor->notebook, x, y, GTK_PICK_DEFAULT);
	gboolean header = FALSE;
	for (GtkWidget *widget = picked; widget && widget != editor->notebook; widget = gtk_widget_get_parent(widget)) {
		const char *name = gtk_widget_get_css_name(widget);
		if (!g_strcmp0(name, "tab") || !g_strcmp0(name, "arrow") || GTK_IS_BUTTON(widget))
			return;
		if (!g_strcmp0(name, "header"))
			header = TRUE;
	}
	if (header)
		show_open_dialog(editor);
}

static void preferences_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	show_preferences(data);
}

void build_editor_window(Editor *editor) {
	initialize_snap_settings(&editor->snap);
	g_autofree char *settings_path = get_settings_path();
	load_settings(&editor->settings, settings_path);
	editor->snap.enabled = editor->settings.snap;
	editor->snap.targets[SNAP_LAYERS] = editor->settings.snap_layers;
	editor->snap.targets[SNAP_BOUNDS] = editor->settings.snap_bounds;
	editor->window = gtk_application_window_new(editor->application);
	gtk_window_set_title(GTK_WINDOW(editor->window), TEXT_APPLICATION_NAME);
	GListModel *monitors = gdk_display_get_monitors(gtk_widget_get_display(editor->window));
	g_autoptr(GdkMonitor) monitor = g_list_model_get_item(monitors, 0);
	GdkRectangle geometry = {0, 0, 1280, 720};
	if (monitor)
		gdk_monitor_get_geometry(monitor, &geometry);
	int default_width, default_height;
	choose_window_size(geometry.width, geometry.height, &default_width, &default_height);
	g_autofree char *state_path = get_state_file_path(WINDOW_STATE_FILE);
	g_autoptr(GKeyFile) state = load_key_file(state_path, G_KEY_FILE_NONE);
	WindowGeometry window_geometry;
	load_window_geometry(state, &window_geometry, default_width, default_height);
	apply_window_geometry(GTK_WINDOW(editor->window), &window_geometry);
	const GActionEntry actions[] = {
		{.name = "open", .activate = open_action},
		{.name = "open-recent", .activate = open_recent_action, .parameter_type = "s"},
		{.name = "clear-recent", .activate = clear_recent_action},
		{.name = "export", .activate = export_action},
		{.name = "save", .activate = save_action},
		{.name = "save-as", .activate = save_as_action},
		{.name = "new", .activate = new_action},
		{.name = "close", .activate = close_document_action},
		{.name = "reopen-closed", .activate = reopen_closed_action},
		{.name = "new-adjustment", .activate = new_adjustment_action, .parameter_type = "s"},
		{.name = "quit", .activate = quit_action},
		{.name = "undo", .activate = undo_action},
		{.name = "redo", .activate = redo_action},
		{.name = "fit", .activate = fit_action},
		{.name = "zoom-in", .activate = zoom_in_action},
		{.name = "zoom-out", .activate = zoom_out_action},
		{.name = "new-layer", .activate = new_layer_action},
		{.name = "select-all", .activate = select_all_action},
		{.name = "free-transform", .activate = free_transform_action},
		{.name = "snap", .state = "true", .change_state = snap_action},
		{.name = "snap-layers", .state = "false", .change_state = snap_action},
		{.name = "snap-bounds", .state = "true", .change_state = snap_action},
		{.name = "cut", .activate = cut_action},
		{.name = "crop", .activate = crop_action},
		{.name = "image-size", .activate = image_size_action},
		{.name = "canvas-size", .activate = canvas_size_action},
		{.name = "trim", .activate = trim_action},
		{.name = "trim-top-left", .activate = trim_top_left_action},
		{.name = "reveal-all", .activate = reveal_all_action},
		{.name = "arrange-front", .activate = arrange_action},
		{.name = "arrange-forward", .activate = arrange_action},
		{.name = "arrange-backward", .activate = arrange_action},
		{.name = "arrange-back", .activate = arrange_action},
		{.name = "copy", .activate = copy_action},
		{.name = "copy-merged", .activate = copy_merged_action},
		{.name = "paste", .activate = paste_action},
		{.name = "clear", .activate = clear_action},
		{.name = "transform-selection", .activate = transform_selection_action},
		{.name = "layer-via-copy", .activate = layer_via_copy_action},
		{.name = "layer-via-cut", .activate = layer_via_cut_action},
		{.name = "deselect", .activate = deselect_action},
		{.name = "inverse", .activate = inverse_action},
		{.name = "modify-border", .activate = modify_action},
		{.name = "modify-smooth", .activate = modify_action},
		{.name = "modify-expand", .activate = modify_action},
		{.name = "modify-contract", .activate = modify_action},
		{.name = "modify-feather", .activate = modify_action},
		{.name = "fill", .activate = fill_action},
		{.name = "stroke", .activate = stroke_action},
		{.name = "preferences", .activate = preferences_action},
		{.name = "fill-foreground", .activate = fill_foreground_action},
		{.name = "fill-background", .activate = fill_background_action},
		{.name = "duplicate-layer", .activate = duplicate_layer_action},
		{.name = "delete-layer", .activate = delete_layer_action},
		{.name = "group-layers", .activate = group_layers_action},
		{.name = "ungroup-layers", .activate = ungroup_layers_action},
		{.name = "merge-layers", .activate = merge_layers_action},
		{.name = "merge-visible", .activate = merge_visible_action},
		{.name = "flatten-image", .activate = flatten_image_action},
		{.name = "select-layer-pixels", .activate = select_layer_pixels_action},
		{.name = "clipping-mask", .state = "false", .change_state = clipping_mask_action},
		{.name = "layer-color", .parameter_type = "i", .state = "0", .change_state = layer_color_action},
		{.name = "actual-pixels", .activate = actual_pixels_action},
		{.name = "split", .state = "false", .change_state = split_action},
		{.name = "grid", .state = "true", .change_state = view_aid_action},
		{.name = "tile-preview", .state = "false", .change_state = view_aid_action}};
	g_action_map_add_action_entries(G_ACTION_MAP(editor->window), actions, G_N_ELEMENTS(actions), editor);

	const struct {
		const char *name;
		gboolean value;
	} snap_states[] = {{"snap", editor->snap.enabled},
					   {"snap-layers", editor->snap.targets[SNAP_LAYERS]},
					   {"snap-bounds", editor->snap.targets[SNAP_BOUNDS]}};

	for (guint index = 0; index < G_N_ELEMENTS(snap_states); index++)
		g_simple_action_set_state(
			G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(editor->window), snap_states[index].name)),
			g_variant_new_boolean(snap_states[index].value));
	set_menu_shortcuts(editor);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	g_autoptr(GMenuModel) menu = create_menu_model(editor);
	gtk_box_append(GTK_BOX(box), gtk_popover_menu_bar_new_from_model(menu));
	editor->notebook = gtk_notebook_new();
	GtkGesture *empty_click = gtk_gesture_click_new();
	g_signal_connect(empty_click, "released", G_CALLBACK(click_tab_bar), editor);
	gtk_widget_add_controller(editor->notebook, GTK_EVENT_CONTROLLER(empty_click));
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(editor->notebook), TRUE);
	gtk_widget_set_hexpand(editor->notebook, TRUE);
	/* The dock's Color panel follows the tool column's color swatches, so the column comes first */
	GtkWidget *tool_column = create_tool_column(editor);
	editor->documents = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(editor->documents), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(editor->documents), create_start_view(editor), "start");
	gtk_stack_add_named(GTK_STACK(editor->documents), editor->notebook, "documents");
	gtk_widget_set_vexpand(editor->documents, TRUE);
	GtkWidget *workspace = create_editor_dock(editor, editor->documents);
	add_workspace_actions(editor);
	watch_layer_rename(editor);
	gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	gtk_box_append(GTK_BOX(box), create_options_bar(editor));
	gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_box_append(GTK_BOX(body), tool_column);
	gtk_box_append(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
	gtk_box_append(GTK_BOX(body), workspace);
	g_object_unref(workspace);
	gtk_widget_set_vexpand(body, TRUE);
	gtk_box_append(GTK_BOX(box), body);
	gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	gtk_box_append(GTK_BOX(box), create_status_bar(editor));
	g_signal_connect(editor->notebook, "switch-page", G_CALLBACK(switch_document), editor);
	g_signal_connect(editor->notebook, "page-removed", G_CALLBACK(remove_document), editor);
	switch_document(GTK_NOTEBOOK(editor->notebook), NULL, 0, editor);
	gtk_window_set_child(GTK_WINDOW(editor->window), box);
	g_signal_connect(editor->window, "close-request", G_CALLBACK(close_window), editor);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(press_window_key), editor);
	g_signal_connect(keys, "key-released", G_CALLBACK(release_window_key), editor);
	gtk_widget_add_controller(editor->window, keys);
	GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
	g_signal_connect(drop, "drop", G_CALLBACK(drop_files), editor);
	gtk_widget_add_controller(editor->window, GTK_EVENT_CONTROLLER(drop));
	initialize_editor_tools(editor);
	focus_start_view(editor->window, editor);
	g_signal_connect_after(editor->window, "map", G_CALLBACK(focus_start_view), editor);
}
