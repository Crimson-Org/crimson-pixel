#include <adwaita.h>

#include "editor.h"
#include "../lang/en.h"

/* Each Preferences page with its tab title and icon, in tab order */
static const struct {
	const char *title, *icon;
} PAGES[PAGE_COUNT] = {{TEXT_PREFERENCES_GENERAL, "preferences-system-symbolic"},
					   {TEXT_PREFERENCES_DISPLAY, "video-display-symbolic"},
					   {TEXT_PREFERENCES_TOOLS, "applications-graphics-symbolic"}};

/* Open documents follow a new history size, and everything visible redraws with the new look */
void apply_settings(Editor *editor) {
	g_autofree char *path = get_settings_path();
	save_settings(&editor->settings, path);
	for (int page = 0; page < gtk_notebook_get_n_pages(GTK_NOTEBOOK(editor->notebook)); page++) {
		PresentedDocument *presented =
			get_page_document(gtk_notebook_get_nth_page(GTK_NOTEBOOK(editor->notebook), page));
		if (presented)
			presented->document->history_limit = editor->settings.history_states;
	}
	queue_canvas_redraw(editor);
	gtk_widget_queue_draw(editor->navigator);
}

static void store_setting(GObject *row, int value, Editor *editor) {
	const SettingEntry *entry = g_object_get_data(row, "setting");
	*get_setting_value(&editor->settings, entry) = value;
	apply_settings(editor);
}

static void change_switch(AdwSwitchRow *row, GParamSpec *specification, Editor *editor) {
	store_setting(G_OBJECT(row), adw_switch_row_get_active(row), editor);
}

static void change_choice(AdwComboRow *row, GParamSpec *specification, Editor *editor) {
	const SettingEntry *entry = g_object_get_data(G_OBJECT(row), "setting");
	int choice = adw_combo_row_get_selected(row);
	store_setting(G_OBJECT(row), entry->values ? entry->values[choice] : choice, editor);
}

static GtkWidget *create_setting_row(Editor *editor, const SettingEntry *entry) {
	int value = *get_setting_value(&editor->settings, entry);
	GtkWidget *row;
	if (entry->kind == SETTING_SWITCH) {
		row = adw_switch_row_new();
		adw_switch_row_set_active(ADW_SWITCH_ROW(row), value);
		g_signal_connect(row, "notify::active", G_CALLBACK(change_switch), editor);
	} else {
		row = adw_combo_row_new();
		g_autoptr(GtkStringList) choices = gtk_string_list_new((const char *const *)entry->choices);
		adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(choices));
		adw_combo_row_set_selected(ADW_COMBO_ROW(row), get_setting_choice(entry, value));
		g_object_set_data(G_OBJECT(row), "setting", (gpointer)entry);
		g_signal_connect(row, "notify::selected", G_CALLBACK(change_choice), editor);
	}
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), entry->title);
	if (entry->help)
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), entry->help);
	g_object_set_data(G_OBJECT(row), "setting", (gpointer)entry);
	return row;
}

/* Rows go into a group per section, in table order, on their page */
static GtkWidget *create_page(Editor *editor, SettingPage page) {
	GtkWidget *preferences_page = adw_preferences_page_new();
	adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(preferences_page), PAGES[page].title);
	adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(preferences_page), PAGES[page].icon);
	const char *section = NULL;
	GtkWidget *group = NULL;
	for (guint index = 0; index < SETTING_ENTRY_COUNT; index++) {
		const SettingEntry *entry = &SETTING_ENTRIES[index];
		if (entry->page != page)
			continue;
		if (!group || g_strcmp0(section, entry->section)) {
			section = entry->section;
			group = adw_preferences_group_new();
			adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), section);
			adw_preferences_page_add(ADW_PREFERENCES_PAGE(preferences_page), ADW_PREFERENCES_GROUP(group));
		}
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), create_setting_row(editor, entry));
	}
	return preferences_page;
}

static void forget_preferences(AdwDialog *dialog, Editor *editor) {
	editor->preferences = NULL;
}

void show_preferences(Editor *editor) {
	if (editor->preferences) {
		adw_dialog_present(ADW_DIALOG(editor->preferences), editor->window);
		return;
	}
	AdwDialog *dialog = adw_preferences_dialog_new();
	editor->preferences = GTK_WIDGET(dialog);
	adw_dialog_set_title(dialog, TEXT_PREFERENCES);
	for (int page = 0; page < PAGE_COUNT; page++)
		adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(create_page(editor, page)));
	g_signal_connect(dialog, "closed", G_CALLBACK(forget_preferences), editor);
	adw_dialog_present(dialog, editor->window);
}
