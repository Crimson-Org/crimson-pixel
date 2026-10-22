#include "editor.h"
#include "snapshot-area.h"
#include "swatches.h"
#include "../lang/en.h"

static const int SWATCH_SIZE = 22, SWATCH_GAP = 2;

static void store_swatch_library(Editor *editor) {
	g_autofree char *path = get_swatch_library_path();
	g_autofree char *directory = g_path_get_dirname(path);
	g_mkdir_with_parents(directory, 0700);
	g_autoptr(GBytes) bytes = write_swatches(editor->swatches);
	gsize length;
	const char *data = g_bytes_get_data(bytes, &length);
	g_autoptr(GError) error = NULL;
	if (!g_file_set_contents(path, data, length, &error))
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
}

static int count_swatch_columns(int width) {
	return MAX(1, (width + SWATCH_GAP) / (SWATCH_SIZE + SWATCH_GAP));
}

/* The swatch index under a point of the grid, or -1 between and after the swatches */
static int find_swatch(Editor *editor, double x, double y) {
	int step = SWATCH_SIZE + SWATCH_GAP, column = (int)x / step, row = (int)y / step;
	int columns = count_swatch_columns(gtk_widget_get_width(editor->swatch_grid));
	if (x < 0 || y < 0 || column >= columns || (int)x % step >= SWATCH_SIZE || (int)y % step >= SWATCH_SIZE)
		return -1;
	int index = row * columns + column;
	return index < (int)editor->swatches->len ? index : -1;
}

static int measure_swatch_grid(GtkWidget *widget, int width, gpointer data) {
	Editor *editor = data;
	int columns = count_swatch_columns(width), rows = (editor->swatches->len + columns - 1) / columns;
	return MAX(0, rows * (SWATCH_SIZE + SWATCH_GAP) - SWATCH_GAP);
}

static void append_outline(GtkSnapshot *snapshot, graphene_rect_t box, float width, GdkRGBA color) {
	GskRoundedRect outline;
	gsk_rounded_rect_init_from_rect(&outline, &box, 0);
	gtk_snapshot_append_border(snapshot, &outline, (const float[4]){width, width, width, width},
							   (const GdkRGBA[4]){color, color, color, color});
}

/* One render node per swatch, with an outline on the hovered swatch and a ring on the foreground color */
static void draw_swatch_grid(GtkWidget *widget, GtkSnapshot *snapshot, int width, int height, gpointer data) {
	Editor *editor = data;
	int columns = count_swatch_columns(width),
		hovered = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "hovered"));
	guint32 foreground = get_foreground_color(editor);
	const GdkRGBA border = {0, 0, 0, .25}, dark = {0, 0, 0, .9}, light = {1, 1, 1, .95};
	for (guint index = 0; index < editor->swatches->len; index++) {
		guint32 color = get_swatch_color(&g_array_index(editor->swatches, Swatch, index));
		graphene_rect_t box =
			GRAPHENE_RECT_INIT((index % columns) * (SWATCH_SIZE + SWATCH_GAP),
							   (index / columns) * (SWATCH_SIZE + SWATCH_GAP), SWATCH_SIZE, SWATCH_SIZE);
		gtk_snapshot_append_color(
			snapshot, &(GdkRGBA){(color >> 16 & 255) / 255.0, (color >> 8 & 255) / 255.0, (color & 255) / 255.0, 1},
			&box);
		append_outline(snapshot, box, 1, border);
		if ((int)index == hovered - 1 || color == foreground) {
			append_outline(snapshot, box, 2, dark);
			append_outline(snapshot,
						   GRAPHENE_RECT_INIT(box.origin.x + 2, box.origin.y + 2, SWATCH_SIZE - 4, SWATCH_SIZE - 4), 1,
						   light);
		}
	}
}

static void set_hovered_swatch(Editor *editor, int index) {
	if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(editor->swatch_grid), "hovered")) == index + 1)
		return;
	g_object_set_data(G_OBJECT(editor->swatch_grid), "hovered", GINT_TO_POINTER(index + 1));
	gtk_widget_queue_draw(editor->swatch_grid);
}

static void move_over_swatches(GtkEventControllerMotion *motion, double x, double y, Editor *editor) {
	set_hovered_swatch(editor, find_swatch(editor, x, y));
}

static void leave_swatches(GtkEventControllerMotion *motion, Editor *editor) {
	set_hovered_swatch(editor, -1);
}

static gboolean query_swatch_tooltip(GtkWidget *widget, int x, int y, gboolean keyboard, GtkTooltip *tooltip,
									 Editor *editor) {
	int index = find_swatch(editor, x, y);
	if (index < 0)
		return FALSE;
	gtk_tooltip_set_text(tooltip, g_array_index(editor->swatches, Swatch, index).name);
	return TRUE;
}

static void refresh_swatch_grid(Editor *editor) {
	gtk_widget_set_visible(editor->swatch_empty, editor->swatches->len == 0);
	gtk_widget_queue_resize(editor->swatch_grid);
	gtk_widget_queue_draw(editor->swatch_grid);
}

static void delete_swatch(Editor *editor, guint index) {
	if (index >= editor->swatches->len)
		return;
	g_array_remove_index(editor->swatches, index);
	store_swatch_library(editor);
	set_hovered_swatch(editor, -1);
	refresh_swatch_grid(editor);
}

static void press_swatch(GtkGestureClick *gesture, int presses, double x, double y, Editor *editor) {
	int index = find_swatch(editor, x, y);
	if (index < 0)
		return;
	GdkModifierType modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
	if (modifiers & GDK_ALT_MASK)
		delete_swatch(editor, index);
	else
		set_editor_color(editor, get_swatch_color(&g_array_index(editor->swatches, Swatch, index)), FALSE);
}

static void add_foreground_swatch(GtkButton *button, Editor *editor) {
	guint32 color = get_foreground_color(editor);
	g_autofree char *name = g_strdup_printf(TEXT_SWATCH_NAME, color);
	add_swatch(editor->swatches, name, color);
	store_swatch_library(editor);
	refresh_swatch_grid(editor);
}

static void delete_swatch_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
	delete_swatch(data, g_variant_get_uint32(parameter));
}

/* The menu is parented to the grid, which has no child widgets of its own to rebuild */
static void press_swatch_menu(GtkGestureClick *gesture, int presses, double x, double y, Editor *editor) {
	int index = find_swatch(editor, x, y);
	if (index < 0)
		return;
	g_autoptr(GMenu) menu = g_menu_new();
	g_autoptr(GMenuItem) item = g_menu_item_new(TEXT_DELETE_SWATCH, NULL);
	g_menu_item_set_action_and_target_value(item, "win.delete-swatch", g_variant_new_uint32(index));
	g_menu_append_item(menu, item);
	show_context_menu(editor->swatch_grid, G_MENU_MODEL(menu), x, y);
}

static void finish_swatch_file(GObject *source, GAsyncResult *result, gpointer data) {
	Editor *editor = g_object_get_data(G_OBJECT(data), "editor");
	g_object_unref(data);
	if (!editor)
		return;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = g_file_load_bytes_finish(G_FILE(source), result, NULL, &error);
	gsize length = 0;
	const guint8 *contents = bytes ? g_bytes_get_data(bytes, &length) : NULL;
	g_autoptr(GArray) loaded = bytes ? read_swatches(contents, length, &error) : NULL;
	if (!loaded) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	for (guint index = 0; index < loaded->len; index++) {
		Swatch *swatch = &g_array_index(loaded, Swatch, index);
		Swatch copy = {g_strdup(swatch->name), swatch->red, swatch->green, swatch->blue};
		g_array_append_val(editor->swatches, copy);
	}
	store_swatch_library(editor);
	refresh_swatch_grid(editor);
	g_autofree char *name = g_file_get_basename(G_FILE(source));
	g_autofree char *message = g_strdup_printf(TEXT_SWATCHES_LOADED, loaded->len, name);
	gtk_label_set_text(GTK_LABEL(editor->status), message);
}

gboolean open_swatch_file(Editor *editor, GFile *file) {
	g_autofree char *name = g_file_get_basename(file);
	g_autofree char *lowercase = g_ascii_strdown(name, -1);
	if (!g_str_has_suffix(lowercase, ".aco") && !g_str_has_suffix(lowercase, ".ase"))
		return FALSE;
	g_file_load_bytes_async(file, NULL, finish_swatch_file, g_object_ref(editor->window));
	return TRUE;
}

static void finish_swatch_dialog(GObject *source, GAsyncResult *result, gpointer data) {
	Editor *editor = data;
	g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, NULL);
	for (guint index = 0; files && index < g_list_model_get_n_items(files); index++) {
		g_autoptr(GFile) file = g_list_model_get_item(files, index);
		open_swatch_file(editor, file);
	}
}

static void show_swatch_dialog(GtkButton *button, Editor *editor) {
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, TEXT_LOAD_SWATCHES);
	g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_autoptr(GtkFileFilter) filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, TEXT_SWATCH_FILTER);
	const char *patterns[] = {"*.aco", "*.ACO", "*.ase", "*.ASE"};
	for (guint index = 0; index < G_N_ELEMENTS(patterns); index++)
		gtk_file_filter_add_pattern(filter, patterns[index]);
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(editor->window), NULL, finish_swatch_dialog, editor);
}

static GtkWidget *create_footer_button(const char *icon, const char *tooltip, GCallback pressed, Editor *editor) {
	GtkWidget *button = gtk_button_new_from_icon_name(icon);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
	g_signal_connect(button, "clicked", pressed, editor);
	return button;
}

/* The library loads once at startup from its small ACO file, and a missing file is an empty library */
GtkWidget *create_swatches_panel(Editor *editor) {
	g_autofree char *path = get_swatch_library_path();
	g_autofree char *contents = NULL;
	gsize length = 0;
	if (g_file_get_contents(path, &contents, &length, NULL))
		editor->swatches = read_swatches((const guint8 *)contents, length, NULL);
	if (!editor->swatches)
		editor->swatches = create_swatch_list();
	const GActionEntry actions[] = {{.name = "delete-swatch", .activate = delete_swatch_action, .parameter_type = "u"}};
	g_action_map_add_action_entries(G_ACTION_MAP(editor->window), actions, G_N_ELEMENTS(actions), editor);
	GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	editor->swatch_grid = create_snapshot_area(draw_swatch_grid, NULL, editor);
	set_snapshot_area_measure(editor->swatch_grid, measure_swatch_grid);
	gtk_widget_set_valign(editor->swatch_grid, GTK_ALIGN_START);
	gtk_widget_set_has_tooltip(editor->swatch_grid, TRUE);
	g_signal_connect(editor->swatch_grid, "query-tooltip", G_CALLBACK(query_swatch_tooltip), editor);
	g_signal_connect_object(editor->foreground, "notify::rgba", G_CALLBACK(gtk_widget_queue_draw), editor->swatch_grid,
							G_CONNECT_SWAPPED);
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(move_over_swatches), editor);
	g_signal_connect(motion, "leave", G_CALLBACK(leave_swatches), editor);
	gtk_widget_add_controller(editor->swatch_grid, motion);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(press_swatch), editor);
	gtk_widget_add_controller(editor->swatch_grid, GTK_EVENT_CONTROLLER(click));
	GtkGesture *menu_click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menu_click), GDK_BUTTON_SECONDARY);
	g_signal_connect(menu_click, "pressed", G_CALLBACK(press_swatch_menu), editor);
	gtk_widget_add_controller(editor->swatch_grid, GTK_EVENT_CONTROLLER(menu_click));
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), editor->swatch_grid);
	/* An empty library says so quietly in the middle of the swatch area */
	editor->swatch_empty = gtk_label_new(TEXT_SWATCHES_EMPTY);
	gtk_widget_add_css_class(editor->swatch_empty, "dim-label");
	gtk_widget_set_halign(editor->swatch_empty, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(editor->swatch_empty, GTK_ALIGN_CENTER);
	gtk_widget_set_can_target(editor->swatch_empty, FALSE);
	GtkWidget *area = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(area), scroll);
	gtk_overlay_add_overlay(GTK_OVERLAY(area), editor->swatch_empty);
	gtk_box_append(GTK_BOX(panel), area);
	GtkWidget *actions_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_set_halign(actions_row, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(actions_row), create_footer_button("document-open-symbolic", TEXT_LOAD_SWATCHES,
															  G_CALLBACK(show_swatch_dialog), editor));
	gtk_box_append(GTK_BOX(actions_row), create_footer_button("list-add-symbolic", TEXT_NEW_SWATCH,
															  G_CALLBACK(add_foreground_swatch), editor));
	gtk_box_append(GTK_BOX(panel), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	gtk_box_append(GTK_BOX(panel), actions_row);
	refresh_swatch_grid(editor);
	return panel;
}
