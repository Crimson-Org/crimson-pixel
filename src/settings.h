#pragma once

#include <glib.h>

/* Preferences that last between launches, each read by the feature it changes */
typedef struct {
	int history_states;
	gboolean restore_layout;
	int checker_size, checker_colors;
	int grid_zoom, grid_color;
	gboolean smooth_zoomed_out;
	gboolean brush_crosshair;
	gboolean clear_selection_on_crop;
	gboolean snap, snap_layers, snap_bounds;
} Settings;

/* The kinds of control a preference shows as */
typedef enum { SETTING_SWITCH, SETTING_CHOICE } SettingKind;

/* Preferences window pages in tab order, and hidden settings that are remembered without a control */
typedef enum { PAGE_GENERAL, PAGE_DISPLAY, PAGE_TOOLS, PAGE_COUNT, PAGE_HIDDEN = PAGE_COUNT } SettingPage;

/* One stored preference with its window section, stored group and key, labels, control, range and default */
typedef struct {
	SettingPage page;
	const char *section, *group, *key, *title, *help;
	SettingKind kind;
	gsize offset;
	int initial, minimum, maximum;
	const char *const *choices;
	/* Stored values of the choices when they are not simply the choice positions */
	const int *values;
} SettingEntry;

extern const SettingEntry SETTING_ENTRIES[];
extern const guint SETTING_ENTRY_COUNT;

char *get_settings_path(void);
void load_settings(Settings *settings, const char *path);
void save_settings(const Settings *settings, const char *path);
int *get_setting_value(Settings *settings, const SettingEntry *entry);
int get_setting_choice(const SettingEntry *entry, int value);
const SettingEntry *find_setting(const char *key);
int get_checker_size(const Settings *settings);
int get_grid_zoom(const Settings *settings);
