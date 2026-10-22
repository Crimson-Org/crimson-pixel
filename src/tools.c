#include <math.h>

#include "editor.h"
#include "snapshot-area.h"
#include "value-field.h"
#include "../lang/en.h"

#define TOOL_HAND "59"

/* One tool with its icon, label and the identifier the editor selects it by */
typedef struct {
	const char *identifier, *icon, *name;
} ToolEntry;

typedef struct {
	guint key;
	const ToolEntry tools[3];
} ToolGroup;

/* The tool groups in column order with their selection key */
static const ToolGroup TOOL_GROUPS[] = {
	{GDK_KEY_v, {{TOOL_MOVE, "move", TEXT_TOOL_MOVE}}},
	{GDK_KEY_m,
	 {{TOOL_RECTANGLE_SELECT, "rselect", TEXT_TOOL_RECTANGLE_SELECT},
	  {TOOL_ELLIPSE_SELECT, "eselect", TEXT_TOOL_ELLIPSE_SELECT}}},
	{GDK_KEY_l,
	 {{TOOL_LASSO, "lasso", TEXT_TOOL_LASSO}, {TOOL_POLYGONAL_LASSO, "plasso", TEXT_TOOL_POLYGONAL_LASSO}}},
	{GDK_KEY_w, {{TOOL_MAGIC_WAND, "mwand", TEXT_TOOL_MAGIC_WAND}}},
	{GDK_KEY_c, {{TOOL_CROP, "rcrop", TEXT_TOOL_CROP}}},
	{GDK_KEY_i, {{TOOL_EYEDROPPER, "eyedropper", TEXT_TOOL_EYEDROPPER}}},
	{GDK_KEY_b, {{PAINT_TOOL_BRUSH, "brush", TEXT_TOOL_BRUSH}, {PAINT_TOOL_PENCIL, "pencil", TEXT_TOOL_PENCIL}}},
	{GDK_KEY_e,
	 {{PAINT_TOOL_ERASER, "eraser", TEXT_TOOL_ERASER}, {PAINT_TOOL_MAGIC_ERASER, "meraser", TEXT_TOOL_MAGIC_ERASER}}},
	{GDK_KEY_g,
	 {{PAINT_TOOL_GRADIENT, "gradient", TEXT_TOOL_GRADIENT}, {PAINT_TOOL_BUCKET, "pbucket", TEXT_TOOL_PAINT_BUCKET}}},
	{GDK_KEY_h, {{TOOL_HAND, "hand", TEXT_TOOL_HAND}}}};

static int count_group_tools(const ToolGroup *group) {
	int count = 0;
	while (count < 3 && group->tools[count].identifier)
		count++;
	return count;
}

static int find_tool_group(const char *identifier, int *position) {
	for (guint group = 0; group < G_N_ELEMENTS(TOOL_GROUPS); group++)
		for (int index = 0; index < count_group_tools(&TOOL_GROUPS[group]); index++)
			if (!g_strcmp0(TOOL_GROUPS[group].tools[index].identifier, identifier)) {
				if (position)
					*position = index;
				return group;
			}
	return -1;
}

gboolean is_paint_tool(const char *identifier) {
	return !g_strcmp0(identifier, PAINT_TOOL_BRUSH) || !g_strcmp0(identifier, PAINT_TOOL_PENCIL) ||
		   !g_strcmp0(identifier, PAINT_TOOL_ERASER);
}

gboolean is_fill_tool(const char *identifier) {
	return !g_strcmp0(identifier, PAINT_TOOL_BUCKET) || !g_strcmp0(identifier, PAINT_TOOL_MAGIC_ERASER);
}

gboolean is_selection_tool(const char *identifier) {
	return !g_strcmp0(identifier, TOOL_RECTANGLE_SELECT) || !g_strcmp0(identifier, TOOL_ELLIPSE_SELECT) ||
		   is_selection_path_tool(identifier) || !g_strcmp0(identifier, TOOL_MAGIC_WAND);
}

gboolean *get_selection_anti_alias(Editor *editor) {
	if (!g_strcmp0(editor->tool, TOOL_ELLIPSE_SELECT))
		return &editor->ellipse_anti_alias;
	if (!g_strcmp0(editor->tool, TOOL_LASSO))
		return &editor->lasso_anti_alias;
	return !g_strcmp0(editor->tool, TOOL_POLYGONAL_LASSO) ? &editor->polygon_anti_alias : NULL;
}

/* Tools that find similar areas share the Tolerance, Anti-alias, Contiguous and Sample All Layers options */
static gboolean uses_wand_options(const char *identifier) {
	return is_fill_tool(identifier) || !g_strcmp0(identifier, TOOL_MAGIC_WAND);
}

FillOptions *get_fill_options(Editor *editor) {
	if (!g_strcmp0(editor->tool, TOOL_MAGIC_WAND))
		return &editor->wand_options;
	return !g_strcmp0(editor->tool, PAINT_TOOL_MAGIC_ERASER) ? &editor->magic_eraser_options : &editor->bucket_options;
}

PaintOptions *get_tool_options(Editor *editor) {
	if (!g_strcmp0(editor->tool, PAINT_TOOL_PENCIL))
		return &editor->pencil_options;
	if (!g_strcmp0(editor->tool, PAINT_TOOL_ERASER))
		return &editor->eraser_options;
	return &editor->brush_options;
}

/* ============================== Tool icons ======================================= */

/* Original tool artwork is an alpha mask drawn in the theme's foreground color */
static GtkWidget *create_tool_icon(const char *name) {
	g_autofree char *icon_name = g_strdup_printf("crimson-tool-%s-symbolic", name);
	GtkWidget *image = gtk_image_new_from_icon_name(icon_name);
	gtk_image_set_pixel_size(GTK_IMAGE(image), 22);
	return image;
}

/* ============================== Tool column ====================================== */

static char *describe_tool(const ToolEntry *tool, guint key) {
	g_autofree char *shortcut = key ? g_strdup_printf(" (%c)", g_ascii_toupper(key)) : g_strdup("");
	return g_strdup_printf("%s%s", tool->name, shortcut);
}

/* The tool column marks buttons that hold more tools with a small corner triangle */
static void draw_group_marker(GtkWidget *widget, GtkSnapshot *snapshot, int width, int height, gpointer data) {
	GdkRGBA color;
	gtk_widget_get_color(widget, &color);
	color.alpha *= .7;
	g_autoptr(GskPathBuilder) builder = gsk_path_builder_new();
	gsk_path_builder_move_to(builder, width, 0);
	gsk_path_builder_line_to(builder, width, height);
	gsk_path_builder_line_to(builder, 0, height);
	gsk_path_builder_close(builder);
	g_autoptr(GskPath) path = gsk_path_builder_to_path(builder);
	gtk_snapshot_push_fill(snapshot, path, GSK_FILL_RULE_WINDING);
	gtk_snapshot_append_color(snapshot, &color, &GRAPHENE_RECT_INIT(0, 0, width, height));
	gtk_snapshot_pop(snapshot);
}

static GtkWidget *create_group_icon(const ToolGroup *group, const ToolEntry *tool) {
	GtkWidget *icon = create_tool_icon(tool->icon);
	if (count_group_tools(group) < 2)
		return icon;
	GtkWidget *overlay = gtk_overlay_new(), *marker = create_snapshot_area(draw_group_marker, NULL, NULL);
	gtk_overlay_set_child(GTK_OVERLAY(overlay), icon);
	gtk_widget_set_size_request(marker, 5, 5);
	gtk_widget_set_halign(marker, GTK_ALIGN_END);
	gtk_widget_set_valign(marker, GTK_ALIGN_END);
	gtk_widget_set_can_target(marker, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), marker);
	return overlay;
}

static void refresh_tool_buttons(Editor *editor) {
	int current_position = 0, current_group = find_tool_group(editor->tool, &current_position);
	for (guint group = 0; group < editor->tool_buttons->len; group++) {
		GtkWidget *button = g_ptr_array_index(editor->tool_buttons, group);
		int position = editor->remembered_tools[group];
		const ToolEntry *tool = &TOOL_GROUPS[group].tools[position];
		gtk_button_set_child(GTK_BUTTON(button), create_group_icon(&TOOL_GROUPS[group], tool));
		g_autofree char *description = describe_tool(tool, TOOL_GROUPS[group].key);
		gtk_widget_set_tooltip_text(button, description);
		gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, tool->name, -1);
		g_signal_handlers_block_matched(button, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, editor);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), (int)group == current_group);
		g_signal_handlers_unblock_matched(button, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, editor);
	}
}

static void refresh_options_bar(Editor *editor);

void select_editor_tool(Editor *editor, const char *identifier) {
	int position = 0, group = find_tool_group(identifier, &position);
	if (group < 0)
		return;
	finish_transform_for_tool_change(editor);
	cancel_selection_paths(editor);
	cancel_paint_tool(&editor->paint_tool);
	editor->tool = TOOL_GROUPS[group].tools[position].identifier;
	editor->remembered_tools[group] = position;
	reset_paint_tool(&editor->paint_tool, editor->tool);
	refresh_tool_buttons(editor);
	refresh_options_bar(editor);
	if (!g_strcmp0(editor->tool, TOOL_CROP))
		start_crop_tool(editor);
	queue_canvas_redraw(editor);
}

static void toggle_tool_group(GtkToggleButton *button, Editor *editor) {
	guint group = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "group"));
	select_editor_tool(editor, TOOL_GROUPS[group].tools[editor->remembered_tools[group]].identifier);
	refresh_tool_buttons(editor);
}

static void choose_group_tool(GtkButton *button, Editor *editor) {
	select_editor_tool(editor, g_object_get_data(G_OBJECT(button), "tool"));
	gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER)));
}

/* A secondary click lists the other tools of a group like the original column */
static void show_group_tools(GtkGestureClick *gesture, int presses, double x, double y, Editor *editor) {
	GtkWidget *button = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	guint group = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "group"));
	if (count_group_tools(&TOOL_GROUPS[group]) < 2)
		return;
	GtkWidget *popover = gtk_popover_new(), *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	for (int index = 0; index < count_group_tools(&TOOL_GROUPS[group]); index++) {
		const ToolEntry *tool = &TOOL_GROUPS[group].tools[index];
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8), *item = gtk_button_new();
		gtk_box_append(GTK_BOX(row), create_tool_icon(tool->icon));
		gtk_box_append(GTK_BOX(row), gtk_label_new(tool->name));
		gtk_button_set_child(GTK_BUTTON(item), row);
		gtk_widget_add_css_class(item, "flat");
		g_object_set_data(G_OBJECT(item), "tool", (gpointer)tool->identifier);
		g_signal_connect(item, "clicked", G_CALLBACK(choose_group_tool), editor);
		gtk_box_append(GTK_BOX(list), item);
	}
	gtk_popover_set_child(GTK_POPOVER(popover), list);
	gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_RIGHT);
	gtk_widget_set_parent(popover, button);
	g_signal_connect(popover, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
	gtk_popover_popup(GTK_POPOVER(popover));
}

guint32 read_color_button(GtkWidget *button) {
	const GdkRGBA *color = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button));
	return (guint32)round(color->red * 255) << 16 | (guint32)round(color->green * 255) << 8 |
		   (guint32)round(color->blue * 255);
}

void write_color_button(GtkWidget *button, guint32 color) {
	GdkRGBA rgba = {(color >> 16 & 255) / 255.0, (color >> 8 & 255) / 255.0, (color & 255) / 255.0, 1};
	gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button), &rgba);
}

guint32 get_foreground_color(Editor *editor) {
	return read_color_button(editor->foreground);
}

guint32 get_background_color(Editor *editor) {
	return read_color_button(editor->background);
}

static void swap_colors(Editor *editor) {
	guint32 foreground = read_color_button(editor->foreground);
	write_color_button(editor->foreground, read_color_button(editor->background));
	write_color_button(editor->background, foreground);
}

static void reset_colors(Editor *editor) {
	write_color_button(editor->foreground, 0x000000);
	write_color_button(editor->background, 0xffffff);
}

static void click_swap_colors(GtkButton *button, Editor *editor) {
	swap_colors(editor);
}

void set_editor_color(Editor *editor, guint32 color, gboolean background) {
	write_color_button(background ? editor->background : editor->foreground, color);
}

static GtkWidget *create_color_buttons(Editor *editor) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	g_autoptr(GtkColorDialog) dialog = gtk_color_dialog_new();
	gtk_color_dialog_set_with_alpha(dialog, FALSE);
	editor->foreground = gtk_color_dialog_button_new(g_object_ref(dialog));
	editor->background = gtk_color_dialog_button_new(g_object_ref(dialog));
	gtk_widget_set_tooltip_text(editor->foreground, TEXT_FOREGROUND_COLOR);
	gtk_widget_set_tooltip_text(editor->background, TEXT_BACKGROUND_COLOR);
	reset_colors(editor);
	GtkWidget *swap = gtk_button_new_from_icon_name("media-playlist-repeat-symbolic");
	gtk_widget_add_css_class(swap, "flat");
	gtk_widget_set_tooltip_text(swap, TEXT_SWAP_COLORS);
	g_signal_connect(swap, "clicked", G_CALLBACK(click_swap_colors), editor);
	gtk_box_append(GTK_BOX(box), editor->foreground);
	gtk_box_append(GTK_BOX(box), editor->background);
	gtk_box_append(GTK_BOX(box), swap);
	return box;
}

GtkWidget *create_tool_column(Editor *editor) {
	GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	gtk_widget_add_css_class(column, "toolbar");
	gtk_widget_set_margin_start(column, 4);
	gtk_widget_set_margin_end(column, 4);
	gtk_widget_set_margin_top(column, 4);
	editor->tool_buttons = g_ptr_array_new();
	for (guint group = 0; group < G_N_ELEMENTS(TOOL_GROUPS); group++) {
		GtkWidget *button = gtk_toggle_button_new();
		gtk_widget_add_css_class(button, "flat");
		g_object_set_data(G_OBJECT(button), "group", GUINT_TO_POINTER(group));
		g_signal_connect(button, "toggled", G_CALLBACK(toggle_tool_group), editor);
		GtkGesture *secondary = gtk_gesture_click_new();
		gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
		g_signal_connect(secondary, "pressed", G_CALLBACK(show_group_tools), editor);
		gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(secondary));
		gtk_box_append(GTK_BOX(column), button);
		g_ptr_array_add(editor->tool_buttons, button);
	}
	GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_vexpand(spacer, TRUE);
	gtk_box_append(GTK_BOX(column), spacer);
	gtk_box_append(GTK_BOX(column), create_color_buttons(editor));
	editor->tool_column = column;
	return column;
}

/* ============================== Options bar ====================================== */

static void change_tool_option(GtkWidget *field, double value, gpointer data) {
	Editor *editor = data;
	PaintOptions *options = get_tool_options(editor);
	if (field == editor->tool_size)
		options->shape.diameter = value;
	if (field == editor->tool_hardness)
		options->shape.hardness = value;
	if (field == editor->tool_opacity)
		options->opacity = value / 100;
	if (field == editor->tool_flow)
		options->flow = value / 100;
	if (field == editor->fill_opacity)
		get_fill_options(editor)->opacity = value / 100;
	if (field == editor->fill_tolerance)
		get_fill_options(editor)->wand.tolerance = value;
	if (field == editor->gradient_opacity)
		editor->gradient_options.opacity = value;
}

static GtkWidget *create_option(Editor *editor, const char *label_text, const char *unit, double minimum,
								double maximum, GtkWidget **field) {
	*field = create_value_field(label_text, unit, minimum, maximum, change_tool_option, editor);
	return *field;
}

static void refresh_fill_options(Editor *editor) {
	FillOptions *options = get_fill_options(editor);
	set_value_field(editor->fill_opacity, round(options->opacity * 100));
	set_value_field(editor->fill_tolerance, options->wand.tolerance);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(editor->fill_smooth), options->wand.smooth_edges);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(editor->fill_contiguous), options->wand.contiguous);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(editor->fill_all_layers), options->all_layers);
}

/* Brush and the brush Eraser have hardness and flow, while Pencil tips are hard */
gboolean uses_soft_tip(Editor *editor) {
	gboolean pencil = !g_strcmp0(editor->tool, PAINT_TOOL_PENCIL), eraser = !g_strcmp0(editor->tool, PAINT_TOOL_ERASER);
	return !pencil && !(eraser && get_tool_options(editor)->eraser_mode == 1);
}

static void refresh_options_bar(Editor *editor) {
	gboolean painting = is_paint_tool(editor->tool);
	gtk_widget_set_visible(gtk_widget_get_first_child(editor->options_bar), painting);
	gtk_widget_set_visible(editor->eyedropper_options, !g_strcmp0(editor->tool, TOOL_EYEDROPPER));
	gtk_widget_set_visible(editor->move_options, !g_strcmp0(editor->tool, TOOL_MOVE));
	gtk_widget_set_visible(editor->crop_bar, !g_strcmp0(editor->tool, TOOL_CROP));
	gtk_widget_set_visible(editor->fill_options, uses_wand_options(editor->tool));
	gtk_widget_set_visible(editor->fill_opacity, is_fill_tool(editor->tool));
	gtk_widget_set_visible(editor->gradient_bar, !g_strcmp0(editor->tool, PAINT_TOOL_GRADIENT));
	gboolean *anti_alias = get_selection_anti_alias(editor);
	gtk_widget_set_visible(editor->selection_options, anti_alias != NULL);
	if (anti_alias)
		gtk_check_button_set_active(GTK_CHECK_BUTTON(editor->selection_anti_alias), *anti_alias);
	if (uses_wand_options(editor->tool))
		refresh_fill_options(editor);
	if (!painting)
		return;
	PaintOptions *options = get_tool_options(editor);
	set_value_field(editor->tool_size, options->shape.diameter);
	set_value_field(editor->tool_hardness, options->shape.hardness);
	set_value_field(editor->tool_opacity, round(options->opacity * 100));
	set_value_field(editor->tool_flow, round(options->flow * 100));
	gboolean eraser = !g_strcmp0(editor->tool, PAINT_TOOL_ERASER), soft = uses_soft_tip(editor);
	gtk_widget_set_visible(editor->hardness_row, soft);
	gtk_widget_set_visible(editor->flow_row, soft);
	gtk_widget_set_visible(editor->eraser_mode, eraser);
	gtk_widget_set_visible(editor->tip_shape, !soft);
	gtk_widget_set_visible(editor->pixel_perfect, !soft);
	g_signal_handlers_block_matched(editor->pixel_perfect, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, editor);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(editor->pixel_perfect), options->pixel_perfect);
	g_signal_handlers_unblock_matched(editor->pixel_perfect, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, editor);
	g_signal_handlers_block_matched(editor->tip_shape, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, editor);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(editor->tip_shape), options->square ? 1 : 0);
	g_signal_handlers_unblock_matched(editor->tip_shape, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, editor);
	g_signal_handlers_block_matched(editor->eraser_mode, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, editor);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(editor->eraser_mode), options->eraser_mode);
	g_signal_handlers_unblock_matched(editor->eraser_mode, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, editor);
}

static void initialize_paint_options(PaintOptions *options) {
	*options = (PaintOptions){{15, 100, 0, 100, 25}, 1, 1, 0, 0, TRUE, FALSE, FALSE};
}

/* Native pixel art option: the Pencil's tip is round square */
static void change_tip_shape(GtkDropDown *shape, GParamSpec *specification, Editor *editor) {
	get_tool_options(editor)->square = gtk_drop_down_get_selected(shape) == 1;
	queue_canvas_redraw(editor);
}

/* Native pixel art option: one pixel lines of the Pencil and the pencil Eraser leave out doubled corners */
static void toggle_pixel_perfect(GtkCheckButton *button, Editor *editor) {
	get_tool_options(editor)->pixel_perfect = gtk_check_button_get_active(button);
}

/* Native painting symmetry in the order of the SYMMETRY bits, with the axes shown on the canvas */
static void change_symmetry(GtkDropDown *symmetry, GParamSpec *specification, Editor *editor) {
	editor->symmetry = gtk_drop_down_get_selected(symmetry);
	queue_canvas_redraw(editor);
}

static void change_eraser_mode(GtkDropDown *mode, GParamSpec *specification, Editor *editor) {
	editor->eraser_options.eraser_mode = gtk_drop_down_get_selected(mode);
	refresh_options_bar(editor);
}

static void toggle_fill_option(GtkCheckButton *button, Editor *editor) {
	FillOptions *options = get_fill_options(editor);
	gboolean active = gtk_check_button_get_active(button);
	if (GTK_WIDGET(button) == editor->fill_smooth)
		options->wand.smooth_edges = active;
	if (GTK_WIDGET(button) == editor->fill_contiguous)
		options->wand.contiguous = active;
	if (GTK_WIDGET(button) == editor->fill_all_layers)
		options->all_layers = active;
}

static GtkWidget *create_fill_switch(Editor *editor, const char *label, GtkWidget **button) {
	*button = gtk_check_button_new_with_label(label);
	g_signal_connect(*button, "toggled", G_CALLBACK(toggle_fill_option), editor);
	return *button;
}

static GtkWidget *create_fill_options(Editor *editor) {
	editor->bucket_options = (FillOptions){1, {40, TRUE, TRUE}, FALSE};
	editor->magic_eraser_options = editor->bucket_options;
	editor->wand_options = (FillOptions){1, {16, TRUE, TRUE}, FALSE};
	editor->fill_options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
	GtkWidget *box = editor->fill_options;
	gtk_box_append(GTK_BOX(box), create_option(editor, TEXT_OPTION_OPACITY, "%", 0, 100, &editor->fill_opacity));
	gtk_box_append(GTK_BOX(box), create_option(editor, TEXT_OPTION_TOLERANCE, "", 0, 255, &editor->fill_tolerance));
	gtk_box_append(GTK_BOX(box), create_fill_switch(editor, TEXT_OPTION_ANTI_ALIAS, &editor->fill_smooth));
	gtk_box_append(GTK_BOX(box), create_fill_switch(editor, TEXT_OPTION_CONTIGUOUS, &editor->fill_contiguous));
	gtk_box_append(GTK_BOX(box), create_fill_switch(editor, TEXT_OPTION_ALL_LAYERS, &editor->fill_all_layers));
	gtk_widget_set_visible(box, FALSE);
	return box;
}

void refresh_tool_options(Editor *editor) {
	refresh_options_bar(editor);
}

static void change_gradient_type(GtkDropDown *type, GParamSpec *specification, Editor *editor) {
	editor->gradient_options.type = gtk_drop_down_get_selected(type);
}

static void toggle_gradient_option(GtkCheckButton *button, Editor *editor) {
	gboolean *option = g_object_get_data(G_OBJECT(button), "option");
	*option = gtk_check_button_get_active(button);
}

static GtkWidget *create_gradient_switch(Editor *editor, const char *label, gboolean *option) {
	GtkWidget *button = gtk_check_button_new_with_label(label);
	g_object_set_data(G_OBJECT(button), "option", option);
	g_signal_connect(button, "toggled", G_CALLBACK(toggle_gradient_option), editor);
	return button;
}

static GtkWidget *create_gradient_options(Editor *editor) {
	editor->gradient_options = (GradientOptions){GRADIENT_LINEAR, GRADIENT_CLASSIC, 100, FALSE, FALSE};
	editor->gradient_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
	editor->gradient_type =
		gtk_drop_down_new_from_strings((const char *[]){TEXT_GRADIENT_LINEAR, TEXT_GRADIENT_RADIAL, TEXT_GRADIENT_ANGLE,
														TEXT_GRADIENT_REFLECTED, TEXT_GRADIENT_DIAMOND, NULL});
	gtk_widget_set_tooltip_text(editor->gradient_type, TEXT_GRADIENT_STYLE);
	gtk_accessible_update_property(GTK_ACCESSIBLE(editor->gradient_type), GTK_ACCESSIBLE_PROPERTY_LABEL,
								   TEXT_GRADIENT_STYLE, -1);
	g_signal_connect(editor->gradient_type, "notify::selected", G_CALLBACK(change_gradient_type), editor);
	gtk_box_append(GTK_BOX(editor->gradient_bar), editor->gradient_type);
	gtk_box_append(GTK_BOX(editor->gradient_bar),
				   create_option(editor, TEXT_OPTION_OPACITY, "%", 0, 100, &editor->gradient_opacity));
	set_value_field(editor->gradient_opacity, 100);
	gtk_box_append(GTK_BOX(editor->gradient_bar),
				   create_gradient_switch(editor, TEXT_OPTION_REVERSE, &editor->gradient_options.reverse));
	GtkWidget *dither = create_gradient_switch(editor, TEXT_OPTION_DITHER, &editor->gradient_options.dither);
	gtk_widget_set_tooltip_text(dither, TEXT_DITHER_HELP);
	gtk_box_append(GTK_BOX(editor->gradient_bar), dither);
	gtk_widget_set_visible(editor->gradient_bar, FALSE);
	return editor->gradient_bar;
}

static void toggle_auto_select(GtkCheckButton *button, Editor *editor) {
	editor->move_auto_select = gtk_check_button_get_active(button);
}

static void change_auto_select_target(GtkDropDown *target, GParamSpec *specification, Editor *editor) {
	editor->move_select_groups = gtk_drop_down_get_selected(target) == 0;
}

static GtkWidget *create_move_options(Editor *editor) {
	editor->move_auto_select = TRUE;
	editor->move_options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *auto_select = gtk_check_button_new_with_label(TEXT_OPTION_AUTO_SELECT);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(auto_select), TRUE);
	gtk_widget_set_tooltip_text(auto_select, TEXT_AUTO_SELECT_HELP);
	g_signal_connect(auto_select, "toggled", G_CALLBACK(toggle_auto_select), editor);
	GtkWidget *target =
		gtk_drop_down_new_from_strings((const char *[]){TEXT_AUTO_SELECT_GROUP, TEXT_AUTO_SELECT_LAYER, NULL});
	gtk_drop_down_set_selected(GTK_DROP_DOWN(target), 1);
	gtk_accessible_update_property(GTK_ACCESSIBLE(target), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_AUTO_SELECT_TARGET, -1);
	g_signal_connect(target, "notify::selected", G_CALLBACK(change_auto_select_target), editor);
	gtk_box_append(GTK_BOX(editor->move_options), auto_select);
	gtk_box_append(GTK_BOX(editor->move_options), target);
	gtk_widget_set_visible(editor->move_options, FALSE);
	return editor->move_options;
}

static void toggle_selection_anti_alias(GtkCheckButton *button, Editor *editor) {
	gboolean *anti_alias = get_selection_anti_alias(editor);
	if (anti_alias)
		*anti_alias = gtk_check_button_get_active(button);
}

static GtkWidget *create_selection_options(Editor *editor) {
	editor->ellipse_anti_alias = editor->lasso_anti_alias = editor->polygon_anti_alias = TRUE;
	editor->selection_options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	editor->selection_anti_alias = gtk_check_button_new_with_label(TEXT_OPTION_ANTI_ALIAS);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(editor->selection_anti_alias), TRUE);
	g_signal_connect(editor->selection_anti_alias, "toggled", G_CALLBACK(toggle_selection_anti_alias), editor);
	gtk_box_append(GTK_BOX(editor->selection_options), editor->selection_anti_alias);
	gtk_widget_set_visible(editor->selection_options, FALSE);
	return editor->selection_options;
}

static void press_transform_button(GtkButton *button, Editor *editor) {
	finish_transform(editor, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "apply")));
}

GtkWidget *create_options_bar(Editor *editor) {
	initialize_paint_options(&editor->brush_options);
	initialize_paint_options(&editor->pencil_options);
	/* Native pixel art default: the Pencil paints square blocks of whole pixels */
	editor->pencil_options.square = TRUE;
	initialize_paint_options(&editor->eraser_options);
	editor->options_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_margin_start(editor->options_bar, 8);
	gtk_widget_set_margin_end(editor->options_bar, 8);
	gtk_widget_set_margin_top(editor->options_bar, 4);
	gtk_widget_set_margin_bottom(editor->options_bar, 4);
	gtk_widget_set_size_request(editor->options_bar, -1, 34);
	GtkWidget *paint = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
	editor->eraser_mode = gtk_drop_down_new_from_strings((const char *[]){TEXT_ERASER_BRUSH, TEXT_ERASER_PENCIL, NULL});
	gtk_widget_set_tooltip_text(editor->eraser_mode, TEXT_ERASER_MODE);
	g_signal_connect(editor->eraser_mode, "notify::selected", G_CALLBACK(change_eraser_mode), editor);
	gtk_box_append(GTK_BOX(paint), editor->eraser_mode);
	editor->tip_shape = gtk_drop_down_new_from_strings((const char *[]){TEXT_TIP_ROUND, TEXT_TIP_SQUARE, NULL});
	gtk_widget_set_tooltip_text(editor->tip_shape, TEXT_TIP_SHAPE);
	g_signal_connect(editor->tip_shape, "notify::selected", G_CALLBACK(change_tip_shape), editor);
	gtk_box_append(GTK_BOX(paint), editor->tip_shape);
	editor->pixel_perfect = gtk_check_button_new_with_label(TEXT_PIXEL_PERFECT);
	gtk_widget_set_tooltip_text(editor->pixel_perfect, TEXT_PIXEL_PERFECT_HELP);
	g_signal_connect(editor->pixel_perfect, "toggled", G_CALLBACK(toggle_pixel_perfect), editor);
	gtk_box_append(GTK_BOX(paint), editor->pixel_perfect);
	gtk_box_append(GTK_BOX(paint), create_option(editor, TEXT_OPTION_SIZE, "px", 1, 2500, &editor->tool_size));
	editor->hardness_row = create_option(editor, TEXT_OPTION_HARDNESS, "%", 0, 100, &editor->tool_hardness);
	gtk_box_append(GTK_BOX(paint), editor->hardness_row);
	gtk_box_append(GTK_BOX(paint), create_option(editor, TEXT_OPTION_OPACITY, "%", 0, 100, &editor->tool_opacity));
	editor->flow_row = create_option(editor, TEXT_OPTION_FLOW, "%", 0, 100, &editor->tool_flow);
	gtk_box_append(GTK_BOX(paint), editor->flow_row);
	GtkWidget *symmetry = gtk_drop_down_new_from_strings((const char *[]){
		TEXT_SYMMETRY_OFF, TEXT_SYMMETRY_VERTICAL, TEXT_SYMMETRY_HORIZONTAL, TEXT_SYMMETRY_DUAL, NULL});
	gtk_widget_set_tooltip_text(symmetry, TEXT_SYMMETRY);
	g_signal_connect(symmetry, "notify::selected", G_CALLBACK(change_symmetry), editor);
	gtk_box_append(GTK_BOX(paint), symmetry);
	gtk_box_append(GTK_BOX(editor->options_bar), paint);
	gtk_box_append(GTK_BOX(editor->options_bar), create_fill_options(editor));
	gtk_box_append(GTK_BOX(editor->options_bar), create_gradient_options(editor));
	gtk_box_append(GTK_BOX(editor->options_bar), create_move_options(editor));
	gtk_box_append(GTK_BOX(editor->options_bar), create_selection_options(editor));
	gtk_box_append(GTK_BOX(editor->options_bar), create_crop_options(editor));
	editor->transform_options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	const struct {
		const char *icon, *tooltip;
		gboolean apply;
	} transform_buttons[] = {{"object-select-symbolic", TEXT_APPLY_TRANSFORM, TRUE},
							 {"window-close-symbolic", TEXT_CANCEL_TRANSFORM, FALSE}};

	for (guint index = 0; index < G_N_ELEMENTS(transform_buttons); index++) {
		GtkWidget *button = gtk_button_new_from_icon_name(transform_buttons[index].icon);
		gtk_widget_set_tooltip_text(button, transform_buttons[index].tooltip);
		gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
									   transform_buttons[index].tooltip, -1);
		g_object_set_data(G_OBJECT(button), "apply", GINT_TO_POINTER(transform_buttons[index].apply));
		g_signal_connect(button, "clicked", G_CALLBACK(press_transform_button), editor);
		gtk_box_append(GTK_BOX(editor->transform_options), button);
	}
	gtk_widget_set_visible(editor->transform_options, FALSE);
	gtk_box_append(GTK_BOX(editor->options_bar), editor->transform_options);
	/* Snapping is one click away at the end of the options bar */
	GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(spacer, TRUE);
	gtk_box_append(GTK_BOX(editor->options_bar), spacer);
	GtkWidget *snap = gtk_toggle_button_new_with_label(TEXT_MENU_SNAP);
	gtk_widget_add_css_class(snap, "flat");
	gtk_widget_set_tooltip_text(snap, TEXT_SNAP_HELP);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(snap), "win.snap");
	gtk_box_append(GTK_BOX(editor->options_bar), snap);
	editor->eyedropper_options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *size_label = gtk_label_new_with_mnemonic(TEXT_SAMPLE_SIZE);
	editor->sample_size = gtk_drop_down_new_from_strings(
		(const char *[]){TEXT_SAMPLE_POINT, TEXT_SAMPLE_3, TEXT_SAMPLE_5, TEXT_SAMPLE_11, TEXT_SAMPLE_31, NULL});
	gtk_label_set_mnemonic_widget(GTK_LABEL(size_label), editor->sample_size);
	GtkWidget *source_label = gtk_label_new_with_mnemonic(TEXT_SAMPLE_SOURCE);
	editor->sample_source =
		gtk_drop_down_new_from_strings((const char *[]){TEXT_SAMPLE_CURRENT_LAYER, TEXT_SAMPLE_ALL_LAYERS, NULL});
	gtk_drop_down_set_selected(GTK_DROP_DOWN(editor->sample_source), 1);
	gtk_label_set_mnemonic_widget(GTK_LABEL(source_label), editor->sample_source);
	gtk_box_append(GTK_BOX(editor->eyedropper_options), size_label);
	gtk_box_append(GTK_BOX(editor->eyedropper_options), editor->sample_size);
	gtk_box_append(GTK_BOX(editor->eyedropper_options), source_label);
	gtk_box_append(GTK_BOX(editor->eyedropper_options), editor->sample_source);
	gtk_box_append(GTK_BOX(editor->options_bar), editor->eyedropper_options);
	return editor->options_bar;
}

void pick_editor_color(Editor *editor, Document *document, double x, double y, gboolean background) {
	const int sizes[] = {1, 3, 5, 11, 31};
	guint size = gtk_drop_down_get_selected(GTK_DROP_DOWN(editor->sample_size));
	gboolean active_layer = gtk_drop_down_get_selected(GTK_DROP_DOWN(editor->sample_source)) == 0;
	set_editor_color(editor, sample_document_color(document, x, y, sizes[MIN(size, 4)], active_layer), background);
}

void initialize_editor_tools(Editor *editor) {
	select_editor_tool(editor, PAINT_TOOL_BRUSH);
}

/* Shared by the bracket keys and the canvas drag gesture, keeping the option fields in step */
void set_brush_shape(Editor *editor, double diameter, double hardness) {
	PaintOptions *options = get_tool_options(editor);
	options->shape.diameter = CLAMP(round(diameter), 1, 2500);
	options->shape.hardness = CLAMP(round(hardness), 0, 100);
	refresh_options_bar(editor);
}

/* ============================== Tool keys ======================================== */

static double step_brush_size(double size, gboolean larger) {
	if (!larger) {
		if (size <= 10)
			return MAX(1, size - 1);
		if (size <= 50)
			return 5 * floor((size - 1) / 5);
		if (size <= 100)
			return 10 * floor((size - 1) / 10);
		if (size <= 200)
			return 25 * floor((size - 1) / 25);
		if (size <= 400)
			return 50 * floor((size - 1) / 50);
		return 100 * floor((size - 1) / 100);
	}
	if (size < 10)
		return size + 1;
	if (size < 50)
		return 5 * ceil((size + 1) / 5);
	if (size < 100)
		return 10 * ceil((size + 1) / 10);
	if (size < 200)
		return 25 * ceil((size + 1) / 25);
	if (size < 400)
		return 50 * ceil((size + 1) / 50);
	return 100 * ceil((size + 1) / 100);
}

void step_brush(Editor *editor, gboolean larger, gboolean hardness) {
	PaintOptions *options = get_tool_options(editor);
	double next_hardness = options->shape.hardness, diameter = options->shape.diameter;
	if (hardness)
		next_hardness = larger ? 25 * ceil((next_hardness + 1) / 25) : 25 * floor((next_hardness - 1) / 25);
	else
		diameter = step_brush_size(diameter, larger);
	set_brush_shape(editor, diameter, next_hardness);
	queue_canvas_redraw(editor);
}

static gboolean press_nudge_key(Editor *editor, guint key, GdkModifierType modifiers) {
	int step = (modifiers & GDK_SHIFT_MASK) ? 10 : 1, x = 0, y = 0;
	if (key == GDK_KEY_Left || key == GDK_KEY_KP_Left)
		x = -step;
	else if (key == GDK_KEY_Right || key == GDK_KEY_KP_Right)
		x = step;
	else if (key == GDK_KEY_Up || key == GDK_KEY_KP_Up)
		y = -step;
	else if (key == GDK_KEY_Down || key == GDK_KEY_KP_Down)
		y = step;
	else
		return FALSE;
	/* Arrow keys keep moving through lists and controls unless the canvas has the focus */
	GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(editor->window));
	gboolean moving = !g_strcmp0(editor->tool, TOOL_MOVE);
	if ((!moving && !is_selection_tool(editor->tool)) || (modifiers & (GDK_CONTROL_MASK | GDK_SUPER_MASK)) ||
		(focus && !g_object_get_data(G_OBJECT(focus), "document-view")))
		return FALSE;
	if (moving)
		nudge_with_move_tool(editor, x, y, (modifiers & GDK_ALT_MASK) != 0);
	else
		nudge_selection(editor, x, y);
	return TRUE;
}

gboolean press_tool_key(Editor *editor, guint key, GdkModifierType modifiers) {
	if (press_nudge_key(editor, key, modifiers))
		return TRUE;
	if (modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))
		return FALSE;
	GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(editor->window));
	if (focus && (GTK_IS_EDITABLE(focus) || gtk_widget_get_ancestor(focus, GTK_TYPE_SPIN_BUTTON)))
		return FALSE;
	guint lower = gdk_keyval_to_lower(key);
	int digit = key >= GDK_KEY_0 && key <= GDK_KEY_9		 ? (int)(key - GDK_KEY_0)
				: key >= GDK_KEY_KP_0 && key <= GDK_KEY_KP_9 ? (int)(key - GDK_KEY_KP_0)
															 : -1;
	if (digit >= 0 && is_paint_tool(editor->tool)) {
		PaintOptions *options = get_tool_options(editor);
		options->opacity = type_percent_digit((int)round(options->opacity * 100), digit) / 100.0;
		set_value_field(editor->tool_opacity, round(options->opacity * 100));
		return TRUE;
	}
	if (digit >= 0) {
		type_layer_opacity_digit(editor, digit);
		return TRUE;
	}
	if (lower == GDK_KEY_x) {
		swap_colors(editor);
		return TRUE;
	}
	if (lower == GDK_KEY_d) {
		reset_colors(editor);
		return TRUE;
	}
	if ((key == GDK_KEY_bracketleft || key == GDK_KEY_bracketright || key == GDK_KEY_braceleft ||
		 key == GDK_KEY_braceright) &&
		is_paint_tool(editor->tool)) {
		step_brush(editor, key == GDK_KEY_bracketright || key == GDK_KEY_braceright, modifiers & GDK_SHIFT_MASK);
		return TRUE;
	}
	int position = 0, current = find_tool_group(editor->tool, &position);
	for (guint group = 0; group < G_N_ELEMENTS(TOOL_GROUPS); group++) {
		if (!TOOL_GROUPS[group].key || TOOL_GROUPS[group].key != lower)
			continue;
		int next = (int)group != current ? editor->remembered_tools[group] : position;
		if ((int)group == current && (modifiers & GDK_SHIFT_MASK))
			next = (next + 1) % count_group_tools(&TOOL_GROUPS[group]);
		select_editor_tool(editor, TOOL_GROUPS[group].tools[next].identifier);
		return TRUE;
	}
	return FALSE;
}
