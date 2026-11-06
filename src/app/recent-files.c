#include <string.h>

#include "window/editor.h"
#include "lang/en.h"
#include "common/text/paths.h"
#include "common/widgets/start-view.h"

/* Opened and saved files go to the desktop recent files store, so the file manager sees them too, and the start
 * view and File Open Recent list this application's entries that still exist, newest first */

enum { RECENT_FILE_LIMIT = 10 };
#define FALLBACK_ICON "image-x-generic"

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

/* The start view shows the newest entries with each file's icon and the folder it sits in */
static void fill_recent_list(GtkWidget *view, GPtrArray *recent) {
	guint count = MIN(recent->len, RECENT_FILE_LIMIT);
	StartRecent items[RECENT_FILE_LIMIT] = {0};
	GIcon *icons[RECENT_FILE_LIMIT];
	char *locations[RECENT_FILE_LIMIT], *folders[RECENT_FILE_LIMIT];
	for (guint index = 0; index < count; index++) {
		GtkRecentInfo *info = g_ptr_array_index(recent, index);
		icons[index] = gtk_recent_info_get_gicon(info);
		if (!icons[index])
			icons[index] = g_themed_icon_new(FALLBACK_ICON);
		locations[index] = gtk_recent_info_get_uri_display(info);
		g_autofree char *folder = locations[index] ? g_path_get_dirname(locations[index]) : NULL;
		folders[index] = folder ? shorten_home_path(folder) : g_strdup("");
		items[index] = (StartRecent){icons[index], gtk_recent_info_get_display_name(info), folders[index],
			locations[index], gtk_recent_info_get_uri(info)};
	}
	set_start_view_recent(view, items, count);
	for (guint index = 0; index < count; index++) {
		g_object_unref(icons[index]);
		g_free(locations[index]);
		g_free(folders[index]);
	}
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
	if (editor->start_view)
		fill_recent_list(editor->start_view, recent);
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
