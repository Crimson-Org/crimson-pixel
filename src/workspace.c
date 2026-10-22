#include <adwaita.h>
#include <libpanel.h>

#include "common/key-store.h"
#include "editor.h"
#include "../lang/en.h"

/* A workspace records, for each visible panel, its dock edge, frame row, tab order, selected tab and frame
 * size, together with the edge sizes. libpanel builds each edge as one paned of frames, so that is the
 * whole layout. Workspaces live in a key file group per name, and "Current" holds the last arrangement. */

static const char *PANEL_IDS[PANEL_COUNT] = {"color", "navigator", "layers", "swatches"};
static const char *PANEL_ACTIONS[PANEL_COUNT] = {"show-color", "show-navigator", "show-layers", "show-swatches"};

static const struct {
	const char *name;
	PanelArea area;
	gboolean vertical;
} EDGES[] = {{"start", PANEL_AREA_START, TRUE},
			 {"end", PANEL_AREA_END, TRUE},
			 {"top", PANEL_AREA_TOP, FALSE},
			 {"bottom", PANEL_AREA_BOTTOM, FALSE}};

static const char *CURRENT_GROUP = "Current";
static const char *DEFAULT_LAYOUT = "[Default]\nend-width=300\ncolor=end;0;0;true;1\nnavigator=end;1;0;true;1\nlayers="
									"end;2;0;true;0\nswatches=end;0;1;false;1\n";

typedef struct {
	int panel, edge, row, order, size;
	gboolean selected;
} Placement;

static char *get_workspace_path(void) {
	return get_config_file_path("workspaces.ini");
}

static GKeyFile *load_workspaces(void) {
	GKeyFile *file = g_key_file_new();
	g_autofree char *path = get_workspace_path();
	g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL);
	return file;
}

static void store_workspaces(GKeyFile *file) {
	g_autofree char *path = get_workspace_path();
	g_autofree char *directory = g_path_get_dirname(path);
	g_mkdir_with_parents(directory, 0700);
	g_autoptr(GError) error = NULL;
	if (!g_key_file_save_to_file(file, path, &error))
		g_message("Workspaces not saved: %s", error->message);
}

static char *get_group_name(const char *workspace) {
	return g_strdup_printf("Workspace %s", workspace);
}

static PanelFrame *get_panel_frame(Editor *editor, int panel) {
	GtkWidget *frame = gtk_widget_get_ancestor(editor->panels[panel], PANEL_TYPE_FRAME);
	return frame && editor->dock && gtk_widget_is_ancestor(frame, editor->dock) ? PANEL_FRAME(frame) : NULL;
}

/* ============================== Capture ============================== */

static void capture_layout(Editor *editor, GKeyFile *file, const char *group) {
	g_key_file_remove_group(file, group, NULL);
	int start_width, end_width, top_height, bottom_height;
	g_object_get(editor->dock, "start-width", &start_width, "end-width", &end_width, "top-height", &top_height,
				 "bottom-height", &bottom_height, NULL);
	g_key_file_set_integer(file, group, "start-width", start_width);
	g_key_file_set_integer(file, group, "end-width", end_width);
	g_key_file_set_integer(file, group, "top-height", top_height);
	g_key_file_set_integer(file, group, "bottom-height", bottom_height);
	for (guint edge = 0; edge < G_N_ELEMENTS(EDGES); edge++) {
		g_autofree char *key = g_strdup_printf("reveal-%s", EDGES[edge].name);
		g_key_file_set_boolean(file, group, key,
							   panel_dock_get_reveal_area(PANEL_DOCK(editor->dock), EDGES[edge].area));
	}
	for (int panel = 0; panel < PANEL_COUNT; panel++) {
		PanelFrame *frame = get_panel_frame(editor, panel);
		if (!frame)
			continue;
		g_autoptr(PanelPosition) position = panel_frame_get_position(frame);
		int edge = -1;
		for (guint index = 0; index < G_N_ELEMENTS(EDGES); index++)
			if (panel_position_get_area(position) == EDGES[index].area)
				edge = index;
		if (edge < 0)
			continue;
		guint order = 0;
		for (guint page = 0; page < panel_frame_get_n_pages(frame); page++)
			if (panel_frame_get_page(frame, page) == editor->panels[panel])
				order = page;
		int size =
			EDGES[edge].vertical ? gtk_widget_get_height(GTK_WIDGET(frame)) : gtk_widget_get_width(GTK_WIDGET(frame));
		g_autofree char *value = g_strdup_printf(
			"%s;%u;%u;%s;%d", EDGES[edge].name, panel_position_get_row(position) + panel_position_get_column(position),
			order, panel_frame_get_visible_child(frame) == editor->panels[panel] ? "true" : "false", size);
		g_key_file_set_string(file, group, PANEL_IDS[panel], value);
	}
}

/* ============================== Build ============================== */

static gboolean read_placement(GKeyFile *file, const char *group, int panel, Placement *placement) {
	g_autofree char *value = g_key_file_get_string(file, group, PANEL_IDS[panel], NULL);
	if (!value)
		return FALSE;
	g_auto(GStrv) parts = g_strsplit(value, ";", 0);
	if (g_strv_length(parts) < 5)
		return FALSE;
	placement->panel = panel;
	placement->edge = -1;
	for (guint edge = 0; edge < G_N_ELEMENTS(EDGES); edge++)
		if (!strcmp(parts[0], EDGES[edge].name))
			placement->edge = edge;
	placement->row = atoi(parts[1]);
	placement->order = atoi(parts[2]);
	placement->selected = !strcmp(parts[3], "true");
	placement->size = atoi(parts[4]);
	return placement->edge >= 0;
}

static gint compare_placements(gconstpointer first, gconstpointer second) {
	const Placement *a = first, *b = second;
	return a->edge != b->edge ? a->edge - b->edge : a->row != b->row ? a->row - b->row : a->order - b->order;
}

static void follow_panel(GObject *widget, GParamSpec *property, Editor *editor);

/* The dock is generated as builder markup, since libpanel only accepts edge children that way */
static GtkWidget *build_dock(Editor *editor, GKeyFile *file, const char *group, GtkWidget *center) {
	GArray *placements = g_array_new(FALSE, FALSE, sizeof(Placement));
	for (int panel = 0; panel < PANEL_COUNT; panel++) {
		Placement placement;
		if (read_placement(file, group, panel, &placement))
			g_array_append_val(placements, placement);
	}
	g_array_sort(placements, compare_placements);
	GString *markup = g_string_new(
		"<interface><object class=\"PanelDock\" id=\"dock\">"
		"<property name=\"hexpand\">true</property><property name=\"vexpand\">true</property>"
		"<child internal-child=\"center\"><object class=\"PanelDockChild\"><property name=\"child\">"
		"<object class=\"GtkBox\" id=\"center\"><property name=\"orientation\">vertical</property></object>"
		"</property></object></child>");
	g_autofree int *frames = g_new(int, placements->len);
	int frame_count = 0;
	for (guint edge = 0; edge < G_N_ELEMENTS(EDGES); edge++) {
		int previous_row = -1;
		gboolean open = FALSE;
		for (guint index = 0; index < placements->len; index++) {
			Placement *placement = &g_array_index(placements, Placement, index);
			if (placement->edge != (int)edge)
				continue;
			if (!open) {
				g_string_append_printf(markup,
									   "<child type=\"%s\"><object class=\"PanelPaned\">"
									   "<property name=\"orientation\">%s</property>",
									   EDGES[edge].name, EDGES[edge].vertical ? "vertical" : "horizontal");
				open = TRUE;
			}
			if (placement->row != previous_row) {
				g_string_append_printf(markup, "<child><object class=\"PanelFrame\" id=\"frame%d\"/></child>",
									   frame_count++);
				previous_row = placement->row;
			}
			frames[index] = frame_count - 1;
		}
		if (open)
			g_string_append(markup, "</object></child>");
	}
	g_string_append(markup, "</object></interface>");
	g_autoptr(GtkBuilder) builder = gtk_builder_new_from_string(markup->str, -1);
	g_string_free(markup, TRUE);
	GtkWidget *dock = g_object_ref(GTK_WIDGET(gtk_builder_get_object(builder, "dock")));
	const char *sizes[] = {"start-width", "end-width", "top-height", "bottom-height"};
	for (guint edge = 0; edge < G_N_ELEMENTS(EDGES); edge++) {
		int size = g_key_file_get_integer(file, group, sizes[edge], NULL);
		if (size > 0)
			g_object_set(dock, sizes[edge], size, NULL);
		g_autofree char *key = g_strdup_printf("reveal-%s", EDGES[edge].name);
		gboolean used = FALSE;
		for (guint index = 0; index < placements->len; index++)
			if (g_array_index(placements, Placement, index).edge == (int)edge)
				used = TRUE;
		gboolean reveal =
			g_key_file_has_key(file, group, key, NULL) ? g_key_file_get_boolean(file, group, key, NULL) : used;
		panel_dock_set_reveal_area(PANEL_DOCK(dock), EDGES[edge].area, reveal && used);
	}
	/* Frames keep the default switcher header, the libpanel header that drags side panels between frames */
	for (int frame = 0; frame < frame_count; frame++) {
		g_autofree char *id = g_strdup_printf("frame%d", frame);
		PanelFrame *target = PANEL_FRAME(gtk_builder_get_object(builder, id));
		PanelWidget *selected = NULL;
		for (guint index = 0; index < placements->len; index++) {
			Placement *placement = &g_array_index(placements, Placement, index);
			if (frames[index] != frame)
				continue;
			panel_frame_add(target, editor->panels[placement->panel]);
			if (placement->selected)
				selected = editor->panels[placement->panel];
			if (placement->size > 0)
				panel_frame_set_requested_size(target, placement->size);
		}
		if (selected)
			panel_frame_set_visible_child(target, selected);
	}
	gtk_box_append(GTK_BOX(gtk_builder_get_object(builder, "center")), center);
	g_array_unref(placements);
	return dock;
}

/* ============================== Apply ============================== */

static void follow_frame_page(PanelFrame *frame, GParamSpec *property, gpointer data);

/* libpanel frames always claim spare height, so each frame claims it only when its shown panel grows, leaving
 * Color and Navigator at their natural size and the rest to Layers */
static void refresh_frame_expand(Editor *editor) {
	for (int panel = 0; panel < PANEL_COUNT; panel++) {
		PanelFrame *frame = get_panel_frame(editor, panel);
		if (!frame)
			continue;
		PanelWidget *shown = panel_frame_get_visible_child(frame);
		gtk_widget_set_vexpand(GTK_WIDGET(frame), shown && gtk_widget_get_vexpand(GTK_WIDGET(shown)));
		if (g_object_get_data(G_OBJECT(frame), "follows-page"))
			continue;
		g_object_set_data(G_OBJECT(frame), "follows-page", frame);
		g_signal_connect(frame, "notify::visible-child", G_CALLBACK(follow_frame_page), NULL);
	}
}

static void follow_frame_page(PanelFrame *frame, GParamSpec *property, gpointer data) {
	PanelWidget *shown = panel_frame_get_visible_child(frame);
	gtk_widget_set_vexpand(GTK_WIDGET(frame), shown && gtk_widget_get_vexpand(GTK_WIDGET(shown)));
}

static void refresh_panel_actions(Editor *editor) {
	refresh_frame_expand(editor);
	for (int panel = 0; panel < PANEL_COUNT; panel++) {
		GAction *action = g_action_map_lookup_action(G_ACTION_MAP(editor->window), PANEL_ACTIONS[panel]);
		if (action)
			g_simple_action_set_state(G_SIMPLE_ACTION(action),
									  g_variant_new_boolean(get_panel_frame(editor, panel) != NULL));
	}
}

static void follow_panel(GObject *widget, GParamSpec *property, Editor *editor) {
	refresh_panel_actions(editor);
}

/* Panels and the document area move from the old dock into a freshly built one */
static void apply_layout(Editor *editor, GKeyFile *file, const char *group) {
	if (!g_key_file_has_group(file, group))
		return;
	for (int panel = 0; panel < PANEL_COUNT; panel++) {
		PanelFrame *frame = get_panel_frame(editor, panel);
		if (frame)
			panel_frame_remove(frame, editor->panels[panel]);
	}
	GtkWidget *center = g_object_ref(editor->documents);
	gtk_box_remove(GTK_BOX(gtk_widget_get_parent(center)), center);
	GtkWidget *old = editor->dock, *parent = gtk_widget_get_parent(old);
	GtkWidget *dock = build_dock(editor, file, group, center);
	g_object_unref(center);
	gtk_box_insert_child_after(GTK_BOX(parent), dock, old);
	gtk_box_remove(GTK_BOX(parent), old);
	g_object_unref(dock);
	editor->dock = dock;
	refresh_panel_actions(editor);
}

static GKeyFile *load_default_layout(void) {
	GKeyFile *file = g_key_file_new();
	g_key_file_load_from_data(file, DEFAULT_LAYOUT, -1, G_KEY_FILE_NONE, NULL);
	return file;
}

void create_workspace(Editor *editor, GtkWidget *center) {
	g_autoptr(GKeyFile) saved = load_workspaces();
	g_autoptr(GKeyFile) fallback = load_default_layout();
	/* Restore panel layout at launch in Preferences can start from the default arrangement instead */
	gboolean restored = editor->settings.restore_layout && g_key_file_has_group(saved, CURRENT_GROUP);
	editor->workspace = g_key_file_get_string(saved, "Settings", "workspace", NULL);
	editor->dock = build_dock(editor, restored ? saved : fallback, restored ? CURRENT_GROUP : "Default", center);
	for (int panel = 0; panel < PANEL_COUNT; panel++)
		g_signal_connect(editor->panels[panel], "notify::parent", G_CALLBACK(follow_panel), editor);
	refresh_frame_expand(editor);
}

/* The arrangement at close is restored on the next launch */
void remember_workspace(Editor *editor) {
	if (!editor->dock)
		return;
	g_autoptr(GKeyFile) file = load_workspaces();
	capture_layout(editor, file, CURRENT_GROUP);
	if (editor->workspace)
		g_key_file_set_string(file, "Settings", "workspace", editor->workspace);
	else
		g_key_file_remove_key(file, "Settings", "workspace", NULL);
	store_workspaces(file);
}

/* ============================== Menu ============================== */

static void refresh_workspace_menu(Editor *editor) {
	g_menu_remove_all(editor->workspace_menu);
	g_autoptr(GKeyFile) file = load_workspaces();
	g_auto(GStrv) groups = g_key_file_get_groups(file, NULL);
	for (int index = 0; groups && groups[index]; index++) {
		if (!g_str_has_prefix(groups[index], "Workspace "))
			continue;
		const char *name = groups[index] + strlen("Workspace ");
		g_autoptr(GMenuItem) item = g_menu_item_new(name, NULL);
		g_menu_item_set_action_and_target_value(item, "win.workspace", g_variant_new_string(name));
		g_menu_append_item(editor->workspace_menu, item);
	}
	GAction *action = g_action_map_lookup_action(G_ACTION_MAP(editor->window), "workspace");
	if (action)
		g_simple_action_set_state(G_SIMPLE_ACTION(action),
								  g_variant_new_string(editor->workspace ? editor->workspace : ""));
}

static void choose_workspace(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Editor *editor = data;
	const char *name = g_variant_get_string(parameter, NULL);
	g_autoptr(GKeyFile) file = load_workspaces();
	g_autofree char *group = get_group_name(name);
	if (!g_key_file_has_group(file, group))
		return;
	g_free(editor->workspace);
	editor->workspace = g_strdup(name);
	apply_layout(editor, file, group);
	g_simple_action_set_state(action, parameter);
}

static void finish_save_workspace(AdwAlertDialog *dialog, const char *response, Editor *editor) {
	if (strcmp(response, "save"))
		return;
	GtkWidget *entry = adw_alert_dialog_get_extra_child(dialog);
	g_autofree char *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
	if (!*name)
		return;
	g_autoptr(GKeyFile) file = load_workspaces();
	g_autofree char *group = get_group_name(name);
	capture_layout(editor, file, group);
	store_workspaces(file);
	g_free(editor->workspace);
	editor->workspace = g_strdup(name);
	refresh_workspace_menu(editor);
}

static void save_workspace(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Editor *editor = data;
	AdwDialog *dialog = adw_alert_dialog_new(TEXT_SAVE_WORKSPACE_HEADING, NULL);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", TEXT_CANCEL, "save", TEXT_SAVE_BUTTON, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "save", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "save");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(entry), TEXT_WORKSPACE_NAME);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	if (editor->workspace)
		gtk_editable_set_text(GTK_EDITABLE(entry), editor->workspace);
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);
	g_signal_connect(dialog, "response", G_CALLBACK(finish_save_workspace), editor);
	adw_dialog_present(dialog, editor->window);
}

/* Reset returns to the chosen workspace as saved, or to the default arrangement */
static void reset_workspace(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Editor *editor = data;
	g_autoptr(GKeyFile) file = load_workspaces();
	g_autofree char *group = editor->workspace ? get_group_name(editor->workspace) : NULL;
	if (group && g_key_file_has_group(file, group)) {
		apply_layout(editor, file, group);
		return;
	}
	g_autoptr(GKeyFile) fallback = load_default_layout();
	apply_layout(editor, fallback, "Default");
}

static void delete_workspace(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Editor *editor = data;
	if (!editor->workspace)
		return;
	g_autoptr(GKeyFile) file = load_workspaces();
	g_autofree char *group = get_group_name(editor->workspace);
	g_key_file_remove_group(file, group, NULL);
	store_workspaces(file);
	g_clear_pointer(&editor->workspace, g_free);
	refresh_workspace_menu(editor);
}

/* Showing a closed panel adds it as a new frame below the others on its usual edge */
static void toggle_panel(GSimpleAction *action, GVariant *state, gpointer data) {
	Editor *editor = data;
	int panel = 0;
	while (panel < PANEL_COUNT && strcmp(PANEL_ACTIONS[panel], g_action_get_name(G_ACTION(action))))
		panel++;
	if (panel == PANEL_COUNT)
		return;
	gboolean show = g_variant_get_boolean(state);
	if (show == (get_panel_frame(editor, panel) != NULL))
		return;
	g_autoptr(GKeyFile) file = g_key_file_new();
	capture_layout(editor, file, "Layout");
	if (show) {
		g_autofree char *value = g_strdup_printf("end;%d;0;true;0", 100 + panel);
		g_key_file_set_string(file, "Layout", PANEL_IDS[panel], value);
		g_key_file_set_boolean(file, "Layout", "reveal-end", TRUE);
	} else
		g_key_file_remove_key(file, "Layout", PANEL_IDS[panel], NULL);
	apply_layout(editor, file, "Layout");
}

void add_workspace_actions(Editor *editor) {
	const GActionEntry actions[] = {
		{.name = "workspace", .activate = choose_workspace, .parameter_type = "s", .state = "''"},
		{.name = "save-workspace", .activate = save_workspace},
		{.name = "reset-workspace", .activate = reset_workspace},
		{.name = "delete-workspace", .activate = delete_workspace},
		{.name = "show-color", .state = "true", .change_state = toggle_panel},
		{.name = "show-navigator", .state = "true", .change_state = toggle_panel},
		{.name = "show-layers", .state = "true", .change_state = toggle_panel},
		{.name = "show-swatches", .state = "true", .change_state = toggle_panel}};
	g_action_map_add_action_entries(G_ACTION_MAP(editor->window), actions, G_N_ELEMENTS(actions), editor);
	refresh_panel_actions(editor);
	refresh_workspace_menu(editor);
}

GMenuModel *create_window_menu(Editor *editor) {
	GMenu *menu = g_menu_new(), *panels = g_menu_new(), *workspace = g_menu_new(), *manage = g_menu_new();
	editor->workspace_menu = g_menu_new();
	g_menu_append_section(workspace, NULL, G_MENU_MODEL(editor->workspace_menu));
	g_menu_append(manage, TEXT_SAVE_WORKSPACE, "win.save-workspace");
	g_menu_append(manage, TEXT_RESET_WORKSPACE, "win.reset-workspace");
	g_menu_append(manage, TEXT_DELETE_WORKSPACE, "win.delete-workspace");
	g_menu_append_section(workspace, NULL, G_MENU_MODEL(manage));
	g_menu_append_submenu(menu, TEXT_MENU_WORKSPACE, G_MENU_MODEL(workspace));
	g_menu_append(panels, TEXT_COLOR, "win.show-color");
	g_menu_append(panels, TEXT_NAVIGATOR, "win.show-navigator");
	g_menu_append(panels, TEXT_LAYERS, "win.show-layers");
	g_menu_append(panels, TEXT_SWATCHES, "win.show-swatches");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(panels));
	g_object_unref(panels);
	g_object_unref(workspace);
	g_object_unref(manage);
	return G_MENU_MODEL(menu);
}
