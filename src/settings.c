#include "common/key-store.h"
#include "settings.h"
#include "../lang/en.h"

static const char *const CHECKER_SIZES[] = {TEXT_SETTING_SMALL, TEXT_SETTING_MEDIUM, TEXT_SETTING_LARGE, NULL};
static const char *const CHECKER_COLORS[] = {TEXT_SETTING_LIGHT, TEXT_SETTING_MEDIUM, TEXT_SETTING_DARK, NULL};
static const char *const GRID_ZOOMS[] = {"400%", "600%", "800%", "1200%", "1600%", NULL};
static const char *const GRID_COLORS[] = {TEXT_SETTING_DARK, TEXT_SETTING_LIGHT, NULL};
static const char *const HISTORY_SIZES[] = {"20", "50", "100", "200", "500", "1000", NULL};
static const int HISTORY_STATES[] = {20, 50, 100, 200, 500, 1000};
static const int CHECKER_PIXELS[] = {8, 12, 16};
static const int GRID_ZOOM_LEVELS[] = {4, 6, 8, 12, 16};

const SettingEntry SETTING_ENTRIES[] = {
	{PAGE_GENERAL, TEXT_SETTING_HISTORY, "General", "history-states", TEXT_SETTING_HISTORY_STATES,
	 TEXT_SETTING_HISTORY_STATES_HELP, SETTING_CHOICE, G_STRUCT_OFFSET(Settings, history_states), 200, 0, 5,
	 HISTORY_SIZES, HISTORY_STATES},
	{PAGE_GENERAL, TEXT_SETTING_WORKSPACE, "General", "restore-layout", TEXT_SETTING_RESTORE_LAYOUT,
	 TEXT_SETTING_RESTORE_LAYOUT_HELP, SETTING_SWITCH, G_STRUCT_OFFSET(Settings, restore_layout), TRUE, 0, 1, NULL,
	 NULL},
	{PAGE_DISPLAY, TEXT_SETTING_TRANSPARENCY, "Display", "checker-size", TEXT_SETTING_CHECKER_SIZE, NULL,
	 SETTING_CHOICE, G_STRUCT_OFFSET(Settings, checker_size), 1, 0, 2, CHECKER_SIZES, NULL},
	{PAGE_DISPLAY, TEXT_SETTING_TRANSPARENCY, "Display", "checker-colors", TEXT_SETTING_CHECKER_COLORS, NULL,
	 SETTING_CHOICE, G_STRUCT_OFFSET(Settings, checker_colors), 0, 0, 2, CHECKER_COLORS, NULL},
	{PAGE_DISPLAY, TEXT_SETTING_PIXEL_GRID, "Display", "grid-zoom", TEXT_SETTING_GRID_ZOOM, TEXT_SETTING_GRID_ZOOM_HELP,
	 SETTING_CHOICE, G_STRUCT_OFFSET(Settings, grid_zoom), 2, 0, 4, GRID_ZOOMS, NULL},
	{PAGE_DISPLAY, TEXT_SETTING_PIXEL_GRID, "Display", "grid-color", TEXT_SETTING_GRID_COLOR, NULL, SETTING_CHOICE,
	 G_STRUCT_OFFSET(Settings, grid_color), 0, 0, 1, GRID_COLORS, NULL},
	{PAGE_DISPLAY, TEXT_SETTING_ZOOMING, "Display", "smooth-zoomed-out", TEXT_SETTING_SMOOTH_ZOOM,
	 TEXT_SETTING_SMOOTH_ZOOM_HELP, SETTING_SWITCH, G_STRUCT_OFFSET(Settings, smooth_zoomed_out), TRUE, 0, 1, NULL,
	 NULL},
	{PAGE_TOOLS, TEXT_SETTING_PAINTING, "Tools", "brush-crosshair", TEXT_SETTING_BRUSH_CROSSHAIR,
	 TEXT_SETTING_BRUSH_CROSSHAIR_HELP, SETTING_SWITCH, G_STRUCT_OFFSET(Settings, brush_crosshair), FALSE, 0, 1, NULL,
	 NULL},
	{PAGE_TOOLS, TEXT_PREFERENCES_CROP, "Tools", "clear-selection", TEXT_SETTING_CLEAR_SELECTION_ON_CROP,
	 TEXT_SETTING_CLEAR_SELECTION_ON_CROP_HELP, SETTING_SWITCH, G_STRUCT_OFFSET(Settings, clear_selection_on_crop),
	 TRUE, 0, 1, NULL, NULL},
	{PAGE_HIDDEN, NULL, "Snap", "enabled", NULL, NULL, SETTING_SWITCH, G_STRUCT_OFFSET(Settings, snap), TRUE, 0, 1,
	 NULL, NULL},
	{PAGE_HIDDEN, NULL, "Snap", "layers", NULL, NULL, SETTING_SWITCH, G_STRUCT_OFFSET(Settings, snap_layers), FALSE, 0,
	 1, NULL, NULL},
	{PAGE_HIDDEN, NULL, "Snap", "bounds", NULL, NULL, SETTING_SWITCH, G_STRUCT_OFFSET(Settings, snap_bounds), TRUE, 0,
	 1, NULL, NULL}};
const guint SETTING_ENTRY_COUNT = G_N_ELEMENTS(SETTING_ENTRIES);

char *get_settings_path(void) {
	return get_config_file_path("settings.ini");
}

int *get_setting_value(Settings *settings, const SettingEntry *entry) {
	return G_STRUCT_MEMBER_P(settings, entry->offset);
}

/* The position of a stored value among the choices, or -1 when it is not one of them */
int get_setting_choice(const SettingEntry *entry, int value) {
	if (!entry->values)
		return value;
	for (int index = 0; index <= entry->maximum; index++)
		if (entry->values[index] == value)
			return index;
	return -1;
}

const SettingEntry *find_setting(const char *key) {
	for (guint index = 0; index < SETTING_ENTRY_COUNT; index++)
		if (!g_strcmp0(SETTING_ENTRIES[index].key, key))
			return &SETTING_ENTRIES[index];
	return NULL;
}

/* Values outside the entry's range come from an edited or older file and fall back to the default */
static int read_setting(GKeyFile *file, const SettingEntry *entry) {
	g_autoptr(GError) error = NULL;
	int value = entry->kind == SETTING_SWITCH ? g_key_file_get_boolean(file, entry->group, entry->key, &error)
											  : g_key_file_get_integer(file, entry->group, entry->key, &error);
	if (entry->values)
		return !error && get_setting_choice(entry, value) >= 0 ? value : entry->initial;
	return error || value < entry->minimum || value > entry->maximum ? entry->initial : value;
}

void load_settings(Settings *settings, const char *path) {
	g_autoptr(GKeyFile) file = g_key_file_new();
	g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL);
	for (guint index = 0; index < SETTING_ENTRY_COUNT; index++)
		*get_setting_value(settings, &SETTING_ENTRIES[index]) = read_setting(file, &SETTING_ENTRIES[index]);
}

void save_settings(const Settings *settings, const char *path) {
	g_autoptr(GKeyFile) file = g_key_file_new();
	g_key_file_load_from_file(file, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
	for (guint index = 0; index < SETTING_ENTRY_COUNT; index++) {
		const SettingEntry *entry = &SETTING_ENTRIES[index];
		int value = *get_setting_value((Settings *)settings, entry);
		if (entry->kind == SETTING_SWITCH)
			g_key_file_set_boolean(file, entry->group, entry->key, value);
		else
			g_key_file_set_integer(file, entry->group, entry->key, value);
	}
	g_autofree char *directory = g_path_get_dirname(path);
	g_mkdir_with_parents(directory, 0700);
	g_autoptr(GError) error = NULL;
	if (!g_key_file_save_to_file(file, path, &error))
		g_message("Settings not saved: %s", error->message);
}

/* The checkerboard cell size in screen pixels */
int get_checker_size(const Settings *settings) {
	return CHECKER_PIXELS[settings->checker_size];
}

/* The zoom factor from which the pixel grid shows */
int get_grid_zoom(const Settings *settings) {
	return GRID_ZOOM_LEVELS[settings->grid_zoom];
}
