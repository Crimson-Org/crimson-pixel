#include "editor.h"
#include "../lang/en.h"

/* Opened and saved files go to the desktop recent files store, so the file manager sees them too, and the start
 * view and File Open Recent list this application's entries that still exist, newest first */

enum { RECENT_FILE_LIMIT = 10 };

void remember_recent_file(GFile *file) {
	g_autofree char *uri = g_file_get_uri(file);
	gtk_recent_manager_add_item(gtk_recent_manager_get_default(), uri);
}

static gint compare_recent_use(gconstpointer first, gconstpointer second) {
	GDateTime *first_time = gtk_recent_info_get_modified(*(GtkRecentInfo *const *)first);
	GDateTime *second_time = gtk_recent_info_get_modified(*(GtkRecentInfo *const *)second);
	return g_date_time_compare(second_time, first_time);
}

/* Every entry this application added, newest first, with missing files left out unless asked for */
static GPtrArray *collect_recent_files(gboolean missing_too) {
	GList *items = gtk_recent_manager_get_items(gtk_recent_manager_get_default());
	GPtrArray *mine = g_ptr_array_new_with_free_func((GDestroyNotify)gtk_recent_info_unref);
	for (GList *item = items; item; item = item->next) {
		GtkRecentInfo *info = item->data;
		if (gtk_recent_info_has_application(info, g_get_application_name()) &&
			(missing_too || gtk_recent_info_exists(info)))
			g_ptr_array_add(mine, gtk_recent_info_ref(info));
	}
	g_list_free_full(items, (GDestroyNotify)gtk_recent_info_unref);
	g_ptr_array_sort(mine, compare_recent_use);
	return mine;
}

void forget_recent_file(const char *uri) {
	gtk_recent_manager_remove_item(gtk_recent_manager_get_default(), uri, NULL);
}

/* Clear Recent removes only this application's entries, leaving other programs' history alone */
void clear_recent_files(void) {
	g_autoptr(GPtrArray) mine = collect_recent_files(TRUE);
	for (guint index = 0; index < mine->len; index++)
		forget_recent_file(gtk_recent_info_get_uri(g_ptr_array_index(mine, index)));
}

void open_recent_uri(Editor *editor, const char *uri) {
	g_autoptr(GFile) file = g_file_new_for_uri(uri);
	open_editor_file(editor, file);
}

/* ============================== Start view list ============================== */

static void open_recent_button(GtkButton *button, Editor *editor) {
	open_recent_uri(editor, g_object_get_data(G_OBJECT(button), "uri"));
}

static void forget_recent_button(GtkButton *button, gpointer data) {
	forget_recent_file(g_object_get_data(G_OBJECT(button), "uri"));
}

/* The remove button shows while the pointer is over its row or the keyboard is on it */
static void show_remove_button(GtkEventControllerMotion *motion, double x, double y, GtkWidget *remove) {
	gtk_widget_set_opacity(remove, 1);
}

static void hide_remove_button(GtkEventControllerMotion *motion, GtkWidget *remove) {
	if (!gtk_widget_has_focus(remove))
		gtk_widget_set_opacity(remove, 0);
}

static void focus_remove_button(GtkEventControllerFocus *focus, GtkWidget *remove) {
	gtk_widget_set_opacity(remove, 1);
}

static void leave_remove_button(GtkEventControllerFocus *focus, GtkWidget *remove) {
	gtk_widget_set_opacity(remove, 0);
}

static GtkWidget *create_recent_row(Editor *editor, GtkRecentInfo *info) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	GtkWidget *button = gtk_button_new(), *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_hexpand(button, TRUE);
	g_autoptr(GIcon) icon = gtk_recent_info_get_gicon(info);
	GtkWidget *image = icon ? gtk_image_new_from_gicon(icon) : gtk_image_new_from_icon_name("image-x-generic");
	gtk_image_set_icon_size(GTK_IMAGE(image), GTK_ICON_SIZE_LARGE);
	gtk_box_append(GTK_BOX(content), image);
	GtkWidget *name = gtk_label_new(gtk_recent_info_get_display_name(info));
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_widget_set_hexpand(name, TRUE);
	gtk_box_append(GTK_BOX(content), name);
	g_autofree char *location = gtk_recent_info_get_uri_display(info);
	g_autofree char *folder = location ? g_path_get_dirname(location) : NULL;
	GtkWidget *place = gtk_label_new(folder);
	gtk_label_set_ellipsize(GTK_LABEL(place), PANGO_ELLIPSIZE_START);
	gtk_label_set_max_width_chars(GTK_LABEL(place), 40);
	gtk_widget_add_css_class(place, "dim-label");
	gtk_box_append(GTK_BOX(content), place);
	gtk_button_set_child(GTK_BUTTON(button), content);
	gtk_widget_set_tooltip_text(button, location);
	const char *uri = gtk_recent_info_get_uri(info);
	g_object_set_data_full(G_OBJECT(button), "uri", g_strdup(uri), g_free);
	g_signal_connect(button, "clicked", G_CALLBACK(open_recent_button), editor);
	gtk_box_append(GTK_BOX(row), button);
	GtkWidget *remove = gtk_button_new_from_icon_name("window-close-symbolic");
	gtk_widget_add_css_class(remove, "flat");
	gtk_widget_add_css_class(remove, "circular");
	gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(remove, TEXT_RECENT_REMOVE);
	gtk_accessible_update_property(GTK_ACCESSIBLE(remove), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_RECENT_REMOVE, -1);
	gtk_widget_set_opacity(remove, 0);
	g_object_set_data_full(G_OBJECT(remove), "uri", g_strdup(uri), g_free);
	g_signal_connect(remove, "clicked", G_CALLBACK(forget_recent_button), NULL);
	gtk_box_append(GTK_BOX(row), remove);
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "enter", G_CALLBACK(show_remove_button), remove);
	g_signal_connect(motion, "leave", G_CALLBACK(hide_remove_button), remove);
	gtk_widget_add_controller(row, motion);
	GtkEventController *focus = gtk_event_controller_focus_new();
	g_signal_connect(focus, "enter", G_CALLBACK(focus_remove_button), remove);
	g_signal_connect(focus, "leave", G_CALLBACK(leave_remove_button), remove);
	gtk_widget_add_controller(remove, focus);
	return row;
}

static void fill_recent_list(GtkWidget *box, GPtrArray *recent) {
	Editor *editor = g_object_get_data(G_OBJECT(box), "editor");
	GtkWidget *list = g_object_get_data(G_OBJECT(box), "list"), *child;
	while ((child = gtk_widget_get_first_child(list)))
		gtk_box_remove(GTK_BOX(list), child);
	for (guint index = 0; index < recent->len && index < RECENT_FILE_LIMIT; index++)
		gtk_box_append(GTK_BOX(list), create_recent_row(editor, g_ptr_array_index(recent, index)));
	gtk_widget_set_visible(box, recent->len > 0);
}

/* ============================== File Open Recent ============================== */

static void fill_recent_menu(GMenu *menu, GPtrArray *recent) {
	g_menu_remove_all(menu);
	GMenu *files = g_menu_new(), *clearing = g_menu_new();
	for (guint index = 0; index < recent->len && index < RECENT_FILE_LIMIT; index++) {
		GtkRecentInfo *info = g_ptr_array_index(recent, index);
		g_autoptr(GMenuItem) item = g_menu_item_new(gtk_recent_info_get_display_name(info), NULL);
		g_menu_item_set_action_and_target(item, "win.open-recent", "s", gtk_recent_info_get_uri(info));
		g_menu_append_item(files, item);
	}
	g_menu_append(clearing, TEXT_RECENT_CLEAR, "win.clear-recent");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(files));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(clearing));
	g_object_unref(files);
	g_object_unref(clearing);
}

/* One store change refreshes the start view list, the menu and whether Clear Recent has anything to clear */
static void refresh_recent_files(GtkRecentManager *manager, GtkWidget *window) {
	Editor *editor = g_object_get_data(G_OBJECT(window), "editor");
	if (!editor)
		return;
	g_autoptr(GPtrArray) recent = collect_recent_files(FALSE);
	if (editor->recent_list)
		fill_recent_list(editor->recent_list, recent);
	fill_recent_menu(editor->recent_menu, recent);
	GAction *clear = g_action_map_lookup_action(G_ACTION_MAP(window), "clear-recent");
	if (clear)
		g_simple_action_set_enabled(G_SIMPLE_ACTION(clear), recent->len > 0);
}

/* Reading the recent files store waits until the window has drawn, so startup stays fast, and the window's
 * lifetime ends the subscription */
static gboolean load_recent_files(gpointer window) {
	GtkRecentManager *manager = gtk_recent_manager_get_default();
	refresh_recent_files(manager, window);
	g_signal_connect_object(manager, "changed", G_CALLBACK(refresh_recent_files), window, 0);
	return G_SOURCE_REMOVE;
}

GMenuModel *create_recent_menu(Editor *editor) {
	editor->recent_menu = g_menu_new();
	g_idle_add_full(G_PRIORITY_LOW, load_recent_files, g_object_ref(editor->window), g_object_unref);
	return G_MENU_MODEL(editor->recent_menu);
}

GtkWidget *create_recent_list(Editor *editor) {
	GtkWidget *recent = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6), *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *heading = gtk_label_new(TEXT_START_RECENT);
	gtk_widget_add_css_class(heading, "heading");
	gtk_label_set_xalign(GTK_LABEL(heading), 0);
	gtk_box_append(GTK_BOX(recent), heading);
	gtk_box_append(GTK_BOX(recent), list);
	gtk_widget_set_size_request(recent, 480, -1);
	gtk_widget_set_visible(recent, FALSE);
	g_object_set_data(G_OBJECT(recent), "editor", editor);
	g_object_set_data(G_OBJECT(recent), "list", list);
	editor->recent_list = recent;
	return recent;
}
