#include <math.h>
#include <string.h>

#include "adjustments.h"
#include "blend.h"
#include "editor.h"
#include "history.h"
#include "layer-commands.h"
#include "snapshot-area.h"
#include "value-field.h"
#include "../lang/en.h"

static void press_layer_list(GtkGestureClick *gesture, int presses, double x, double y, Editor *editor);
static void press_eye(GtkGestureDrag *gesture, double x, double y, Editor *editor);
static void drag_over_eyes(GtkGestureDrag *gesture, double offset_x, double offset_y, Editor *editor);
static void press_layer_menu(GtkGestureClick *gesture, int presses, double x, double y, Editor *editor);
static GdkDragAction move_over_layers(GtkDropTarget *target, double x, double y, Editor *editor);
static void leave_layers(GtkDropTarget *target, Editor *editor);
static gboolean drop_on_layers(GtkDropTarget *target, const GValue *value, double x, double y, Editor *editor);

static void change_layer_value(GtkWidget *field, double percent, gpointer data) {
	Editor *editor = data;
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || !gtk_widget_get_sensitive(field))
		return;
	double value = round(percent * 255 / 100);
	g_autoptr(GError) error = NULL;
	gboolean changed = field == editor->opacity
						   ? change_layer_opacity(presented->document, -1, value, FALSE, &error)
						   : change_layer_fill_opacity(presented->document, -1, value, FALSE, &error);
	if (!changed) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	refresh_document_page(editor, page, TRUE);
}

static GtkWidget *create_percent_control(Editor *editor, const char *label_text, GtkWidget **field) {
	*field = create_value_field(label_text, "%", 0, 100, change_layer_value, editor);
	return *field;
}

/* Layer values change through the panel for eight bit raster documents with an active layer */
static gboolean can_edit_layer_values(Document *document) {
	return !document->preview && document->mode == DOCUMENT_RASTER && get_number(document->record, "depth") == 8 &&
		   count_records(get_record(document->record, "j")) > 0 && get_active_layer(document);
}

int type_percent_digit(int current, int digit) {
	static gint64 last_digit = 0;
	gint64 now = g_get_monotonic_time();
	int percent;
	if (now - last_digit > G_USEC_PER_SEC)
		percent = digit == 0 ? 100 : digit * 10;
	else
		percent = ((current % 10 != 0 ? current * 10 : current) + digit) % 100;
	last_digit = now;
	return percent;
}

void type_layer_opacity_digit(Editor *editor, int digit) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || !can_edit_layer_values(presented->document))
		return;
	Record *layer = get_active_layer(presented->document);
	int percent = type_percent_digit((int)round(100 * get_number(layer, "nv") / 255), digit);
	g_autoptr(GError) error = NULL;
	if (!change_layer_opacity(presented->document, -1, round(255.0 * percent / 100), FALSE, &error)) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	refresh_document_page(editor, page, TRUE);
}

/* ============================== Blend mode ============================== */

static void apply_layer_blend_mode(Editor *editor, const char *mode) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || !can_edit_layer_values(presented->document))
		return;
	g_autoptr(GError) error = NULL;
	if (!change_layer_blend_mode(presented->document, -1, mode, FALSE, &error)) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	refresh_document_page(editor, page, TRUE);
}

static void choose_blend_mode(GtkDropDown *list, GParamSpec *specification, Editor *editor) {
	guint selected = gtk_drop_down_get_selected(list);
	if (editor->updating_blend_mode || selected == GTK_INVALID_LIST_POSITION)
		return;
	int mode = (int)selected - (editor->blend_mode_lists_pass ? 1 : 0);
	apply_layer_blend_mode(editor, mode < 0 ? "pass" : blend_mode_keys[mode]);
}

void step_layer_blend_mode(Editor *editor, gboolean forward) {
	PresentedDocument *presented = get_page_document(get_current_page(editor));
	if (!presented || !can_edit_layer_values(presented->document))
		return;
	Record *layer = get_active_layer(presented->document);
	int offset = is_layer_group(layer) ? 1 : 0, count = BLEND_MODE_COUNT + offset;
	const char *current = get_string(layer, "qu");
	int position = !g_strcmp0(current, "pass") ? 0 : find_blend_mode(current) + offset;
	int next = (position + count + (forward ? 1 : -1)) % count - offset;
	apply_layer_blend_mode(editor, next < 0 ? "pass" : blend_mode_keys[next]);
}

static GtkWidget *create_blend_mode_list(Editor *editor) {
	const char *names[] = {TEXT_BLEND_MODE_NAMES, NULL};
	editor->blend_mode = gtk_drop_down_new_from_strings(names);
	gtk_widget_set_tooltip_text(editor->blend_mode, TEXT_BLEND_MODE);
	gtk_accessible_update_property(GTK_ACCESSIBLE(editor->blend_mode), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_BLEND_MODE,
								   -1);
	g_signal_connect(editor->blend_mode, "notify::selected", G_CALLBACK(choose_blend_mode), editor);
	return editor->blend_mode;
}

static void refresh_blend_mode(Editor *editor, Record *layer, gboolean editable) {
	gboolean group = layer && is_layer_group(layer);
	editor->updating_blend_mode = TRUE;
	GtkStringList *names = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(editor->blend_mode)));
	if (group != editor->blend_mode_lists_pass) {
		if (group)
			gtk_string_list_splice(names, 0, 0, (const char *[]){TEXT_BLEND_PASS_THROUGH, NULL});
		else
			gtk_string_list_remove(names, 0);
		editor->blend_mode_lists_pass = group;
	}
	const char *key = layer ? get_string(layer, "qu") : "norm";
	int mode = find_blend_mode(key);
	guint position = mode >= 0							? (guint)(mode + group)
					 : group && !g_strcmp0(key, "pass") ? 0
														: GTK_INVALID_LIST_POSITION;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(editor->blend_mode), position);
	gtk_widget_set_sensitive(editor->blend_mode, editable);
	editor->updating_blend_mode = FALSE;
}

/* ============================== Layer thumbnails ============================== */

enum { THUMBNAIL_SIZE = 36 };

/* A nearest sample of the layer at the document's aspect, straight alpha like the layer channels */
static GdkTexture *create_layer_thumbnail(Document *document, Record *layer) {
	Record *buffer = get_record(layer, "buffer");
	if (!buffer || buffer->type != RECORD_BUFFER)
		return NULL;
	int document_w = document_width(document), document_h = document_height(document);
	if (document_w <= 0 || document_h <= 0)
		return NULL;
	double scale = MIN((double)THUMBNAIL_SIZE / document_w, (double)THUMBNAIL_SIZE / document_h);
	int width = MAX(1, (int)round(document_w * scale)), height = MAX(1, (int)round(document_h * scale));
	RasterExtent extent = read_extent(get_record(layer, "V"));
	double maximum = channel_maximum(buffer->value.buffer.depth);
	guint8 *pixels = g_malloc0((gsize)width * height * 4);
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++) {
			int source_x = (int)((x + 0.5) / scale) - extent.x, source_y = (int)((y + 0.5) / scale) - extent.y;
			if (source_x < 0 || source_y < 0 || source_x >= extent.width || source_y >= extent.height)
				continue;
			gsize index = ((gsize)source_y * extent.width + source_x) * 4;
			for (int channel = 0; channel < 4; channel++)
				pixels[((gsize)y * width + x) * 4 + channel] =
					CLAMP(read_channel(buffer, index + channel) / maximum * 255 + 0.5, 0, 255);
		}
	g_autoptr(GBytes) bytes = g_bytes_new_take(pixels, (gsize)width * height * 4);
	return gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes, (gsize)width * 4);
}

static void draw_layer_thumbnail(GtkWidget *widget, GtkSnapshot *snapshot, int width, int height, gpointer data) {
	GdkTexture *texture = data;
	double scale =
		MIN((double)width / gdk_texture_get_width(texture), (double)height / gdk_texture_get_height(texture));
	double w = round(gdk_texture_get_width(texture) * scale), h = round(gdk_texture_get_height(texture) * scale);
	graphene_rect_t bounds = GRAPHENE_RECT_INIT(round((width - w) / 2), round((height - h) / 2), w, h);
	const GdkRGBA dark = {0.75, 0.75, 0.75, 1}, light = {0.95, 0.95, 0.95, 1};
	gtk_snapshot_push_repeat(snapshot, &bounds, &GRAPHENE_RECT_INIT(0, 0, 8, 8));
	gtk_snapshot_append_color(snapshot, &dark, &GRAPHENE_RECT_INIT(0, 0, 8, 8));
	gtk_snapshot_append_color(snapshot, &light, &GRAPHENE_RECT_INIT(0, 0, 4, 4));
	gtk_snapshot_append_color(snapshot, &light, &GRAPHENE_RECT_INIT(4, 4, 4, 4));
	gtk_snapshot_pop(snapshot);
	gtk_snapshot_append_scaled_texture(snapshot, texture, GSK_SCALING_FILTER_LINEAR, &bounds);
}

static GtkWidget *create_thumbnail_widget(Document *document, Record *layer) {
	GdkTexture *texture = create_layer_thumbnail(document, layer);
	if (!texture)
		return NULL;
	GtkWidget *area = create_snapshot_area(draw_layer_thumbnail, NULL, texture);
	g_object_set_data_full(G_OBJECT(area), "thumbnail", texture, g_object_unref);
	gtk_widget_set_size_request(area, THUMBNAIL_SIZE, THUMBNAIL_SIZE);
	gtk_widget_set_valign(area, GTK_ALIGN_CENTER);
	return area;
}

static void edit_adjustment(GtkButton *button, Editor *editor) {
	show_adjustment_panel(editor, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "layer-index")));
}

/* An adjustment layer has no pixels to show, so its row offers its settings in the thumbnail's place */
static GtkWidget *create_adjustment_button(Editor *editor, int layer_index) {
	GtkWidget *button = gtk_button_new_from_icon_name("display-brightness-symbolic");
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_size_request(button, THUMBNAIL_SIZE, THUMBNAIL_SIZE);
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(button, TEXT_EDIT_ADJUSTMENT);
	gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_EDIT_ADJUSTMENT, -1);
	g_object_set_data(G_OBJECT(button), "layer-index", GINT_TO_POINTER(layer_index));
	g_signal_connect(button, "clicked", G_CALLBACK(edit_adjustment), editor);
	return button;
}

/* ============================== Layer list ============================== */

/* The layer tree with the blend mode, Opacity and Fill above it and the layer buttons below */
GtkWidget *create_layers_panel(Editor *editor) {
	GtkWidget *layers = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_box_append(GTK_BOX(layers), create_blend_mode_list(editor));
	GtkWidget *values = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_set_homogeneous(GTK_BOX(values), TRUE);
	gtk_box_append(GTK_BOX(values), create_percent_control(editor, TEXT_OPACITY, &editor->opacity));
	gtk_box_append(GTK_BOX(values), create_percent_control(editor, TEXT_FILL, &editor->fill));
	gtk_box_append(GTK_BOX(layers), values);
	editor->layer_list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(editor->layer_list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(editor->layer_list, "boxed-list");
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(press_layer_list), editor);
	gtk_widget_add_controller(editor->layer_list, GTK_EVENT_CONTROLLER(click));
	/* Eye presses are claimed before the rows see them, so a drag over the eyes never starts moving a layer */
	GtkGesture *eyes = gtk_gesture_drag_new();
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(eyes), GTK_PHASE_CAPTURE);
	g_signal_connect(eyes, "drag-begin", G_CALLBACK(press_eye), editor);
	g_signal_connect(eyes, "drag-update", G_CALLBACK(drag_over_eyes), editor);
	gtk_widget_add_controller(editor->layer_list, GTK_EVENT_CONTROLLER(eyes));
	GtkGesture *menu_click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menu_click), GDK_BUTTON_SECONDARY);
	g_signal_connect(menu_click, "pressed", G_CALLBACK(press_layer_menu), editor);
	gtk_widget_add_controller(editor->layer_list, GTK_EVENT_CONTROLLER(menu_click));
	GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE | GDK_ACTION_COPY);
	g_signal_connect(drop, "motion", G_CALLBACK(move_over_layers), editor);
	g_signal_connect(drop, "leave", G_CALLBACK(leave_layers), editor);
	g_signal_connect(drop, "drop", G_CALLBACK(drop_on_layers), editor);
	gtk_widget_add_controller(editor->layer_list, GTK_EVENT_CONTROLLER(drop));
	GtkWidget *scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), editor->layer_list);
	gtk_box_append(GTK_BOX(layers), scroll);
	GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_widget_set_halign(actions, GTK_ALIGN_END);

	const struct {
		const char *icon, *tooltip, *action;
	} buttons[] = {{"document-new-symbolic", TEXT_NEW_LAYER, "win.new-layer"},
				   {"edit-copy-symbolic", TEXT_DUPLICATE_LAYER, "win.duplicate-layer"},
				   {"user-trash-symbolic", TEXT_DELETE_LAYER, "win.delete-layer"}};

	GtkWidget *adjustment = gtk_menu_button_new();
	g_autoptr(GMenuModel) adjustments = create_adjustment_menu();
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(adjustment), adjustments);
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(adjustment), "display-brightness-symbolic");
	gtk_widget_add_css_class(adjustment, "flat");
	gtk_widget_set_tooltip_text(adjustment, TEXT_NEW_ADJUSTMENT_LAYER);
	gtk_accessible_update_property(GTK_ACCESSIBLE(adjustment), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_NEW_ADJUSTMENT_LAYER,
								   -1);
	gtk_box_append(GTK_BOX(actions), adjustment);
	for (guint index = 0; index < G_N_ELEMENTS(buttons); index++) {
		GtkWidget *button = gtk_button_new_from_icon_name(buttons[index].icon);
		gtk_widget_add_css_class(button, "flat");
		gtk_widget_set_tooltip_text(button, buttons[index].tooltip);
		gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, buttons[index].tooltip,
									   -1);
		gtk_actionable_set_action_name(GTK_ACTIONABLE(button), buttons[index].action);
		gtk_box_append(GTK_BOX(actions), button);
	}
	gtk_box_append(GTK_BOX(layers), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
	gtk_box_append(GTK_BOX(layers), actions);
	return layers;
}

static void set_percent_value(Editor *editor, GtkWidget *field, double value, gboolean supported) {
	gtk_widget_set_sensitive(field, supported);
	set_value_field(field, round(value * 100 / 255));
}

static void toggle_visibility(GtkButton *button, Editor *editor) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented)
		return;
	int layer_index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "layer-index"));
	g_autoptr(GError) error = NULL;
	GdkDevice *keyboard =
		gdk_seat_get_keyboard(gdk_display_get_default_seat(gtk_widget_get_display(GTK_WIDGET(button))));
	gboolean solo = keyboard && (gdk_device_get_modifier_state(keyboard) & GDK_ALT_MASK);
	if (!(solo ? solo_layer_visible : toggle_layer_visible)(presented->document, layer_index, FALSE, &error)) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	refresh_document_page(editor, page, TRUE);
}

static GtkWidget *find_eye_at(Editor *editor, double x, double y) {
	GtkWidget *widget = gtk_widget_pick(editor->layer_list, x, y, GTK_PICK_DEFAULT);
	for (; widget && widget != editor->layer_list; widget = gtk_widget_get_parent(widget))
		if (g_object_get_data(G_OBJECT(widget), "eye"))
			return widget;
	return NULL;
}

static void press_eye(GtkGestureDrag *gesture, double x, double y, Editor *editor) {
	GtkWidget *eye = find_eye_at(editor, x, y);
	if (!eye || !gtk_widget_get_sensitive(eye)) {
		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
		return;
	}
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	editor->eye_layer = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(eye), "layer-index"));
	toggle_visibility(GTK_BUTTON(eye), editor);
}

static void drag_over_eyes(GtkGestureDrag *gesture, double offset_x, double offset_y, Editor *editor) {
	double x, y;
	gtk_gesture_drag_get_start_point(gesture, &x, &y);
	GtkWidget *eye = find_eye_at(editor, x + offset_x, y + offset_y);
	int layer_index = eye ? GPOINTER_TO_INT(g_object_get_data(G_OBJECT(eye), "layer-index")) : -1;
	if (!eye || layer_index == editor->eye_layer)
		return;
	editor->eye_layer = layer_index;
	toggle_visibility(GTK_BUTTON(eye), editor);
}

typedef struct {
	Editor *editor;
	GtkWidget *entry;
	int layer_index;
	char *name;
	gboolean apply;
} PendingRename;

/* The rename and the list rebuild run after the current event, never while focus is still leaving the field */
static gboolean apply_layer_rename(gpointer data) {
	PendingRename *pending = data;
	Editor *editor = pending->editor;
	/* The label returns in place of the field, which matters when the name stays and nothing is rebuilt */
	GtkWidget *row = gtk_widget_get_parent(pending->entry);
	if (row) {
		GtkWidget *label = g_object_get_data(G_OBJECT(row), "name-label");
		gtk_box_remove(GTK_BOX(row), pending->entry);
		if (label)
			gtk_widget_set_visible(label, TRUE);
	}
	g_object_unref(pending->entry);
	g_clear_pointer(&editor->layer_rows, g_free);
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	g_autoptr(GError) error = NULL;
	if (pending->apply && presented &&
		rename_layer(presented->document, pending->layer_index, pending->name, FALSE, &error))
		refresh_document_page(editor, page, FALSE);
	else {
		if (error)
			gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		refresh_editor_dock(editor);
	}
	g_free(pending->name);
	g_free(pending);
	return G_SOURCE_REMOVE;
}

static void end_layer_rename(Editor *editor, gboolean apply) {
	GtkWidget *entry = editor->rename_entry;
	if (!entry)
		return;
	editor->rename_entry = NULL;
	PendingRename *pending = g_new0(PendingRename, 1);
	pending->editor = editor;
	pending->entry = g_object_ref(entry);
	pending->layer_index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "layer-index"));
	pending->name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
	pending->apply = apply;
	g_idle_add(apply_layer_rename, pending);
}

static void finish_layer_rename(GtkEntry *entry, Editor *editor) {
	end_layer_rename(editor, TRUE);
}

static void leave_layer_rename(GtkEventControllerFocus *focus, Editor *editor) {
	end_layer_rename(editor, TRUE);
}

static gboolean press_rename_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType modifiers,
								 Editor *editor) {
	if (key != GDK_KEY_Escape)
		return FALSE;
	end_layer_rename(editor, FALSE);
	return TRUE;
}

static void press_outside_rename(GtkGestureClick *gesture, int presses, double x, double y, Editor *editor) {
	GtkWidget *entry = editor->rename_entry;
	if (!entry)
		return;
	GtkWidget *picked = gtk_widget_pick(editor->window, x, y, GTK_PICK_DEFAULT);
	if (picked && (picked == entry || gtk_widget_is_ancestor(picked, entry)))
		return;
	end_layer_rename(editor, TRUE);
}

void watch_layer_rename(Editor *editor) {
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
	g_signal_connect(click, "pressed", G_CALLBACK(press_outside_rename), editor);
	gtk_widget_add_controller(editor->window, GTK_EVENT_CONTROLLER(click));
}

/* Double-clicking a layer name edits it in place panel */
static void begin_layer_rename(Editor *editor, GtkWidget *row) {
	GtkWidget *label = g_object_get_data(G_OBJECT(row), "name-label");
	if (!label || !gtk_widget_get_visible(label))
		return;
	/* An open rename saves first; its list rebuild would remove a second field */
	if (editor->rename_entry) {
		end_layer_rename(editor, TRUE);
		return;
	}
	GtkWidget *entry = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(entry), gtk_label_get_text(GTK_LABEL(label)));
	gtk_widget_set_hexpand(entry, TRUE);
	g_object_set_data(G_OBJECT(entry), "layer-index", g_object_get_data(G_OBJECT(row), "layer-index"));
	editor->rename_entry = entry;
	g_signal_connect(entry, "activate", G_CALLBACK(finish_layer_rename), editor);
	GtkEventController *focus = gtk_event_controller_focus_new();
	g_signal_connect(focus, "leave", G_CALLBACK(leave_layer_rename), editor);
	gtk_widget_add_controller(entry, focus);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(press_rename_key), editor);
	gtk_widget_add_controller(entry, keys);
	gtk_widget_set_visible(label, FALSE);
	gtk_box_insert_child_after(GTK_BOX(row), entry, label);
	gtk_widget_grab_focus(entry);
	gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
}

/* Where a layer's row sits in the list, for popovers that point at it */
gboolean get_layer_row_bounds(Editor *editor, int layer_index, GtkWidget *relative, GdkRectangle *bounds) {
	for (GtkWidget *row = gtk_widget_get_first_child(editor->layer_list); row; row = gtk_widget_get_next_sibling(row)) {
		if (!GTK_IS_LIST_BOX_ROW(row))
			continue;
		GtkWidget *child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
		graphene_rect_t area;
		if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "layer-index")) != layer_index ||
			!gtk_widget_compute_bounds(row, relative, &area))
			continue;
		*bounds = (GdkRectangle){(int)area.origin.x, (int)area.origin.y, (int)area.size.width, (int)area.size.height};
		return TRUE;
	}
	return FALSE;
}

static GtkWidget *find_layer_row(Editor *editor, double y, double *fraction) {
	GtkListBoxRow *list_row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(editor->layer_list), (int)y);
	if (!list_row)
		return NULL;
	graphene_rect_t bounds;
	if (fraction && gtk_widget_compute_bounds(GTK_WIDGET(list_row), editor->layer_list, &bounds))
		*fraction = (y - bounds.origin.y) / MAX(1, bounds.size.height);
	return gtk_list_box_row_get_child(list_row);
}

/* The list owns the click handling, so a double click still counts after the first click rebuilt the rows */
static void press_layer_list(GtkGestureClick *gesture, int presses, double x, double y, Editor *editor) {
	GtkWidget *target = gtk_widget_pick(editor->layer_list, x, y, GTK_PICK_DEFAULT);
	if (target && (GTK_IS_BUTTON(target) || gtk_widget_get_ancestor(target, GTK_TYPE_BUTTON) ||
				   GTK_IS_EDITABLE(target) || gtk_widget_get_ancestor(target, GTK_TYPE_ENTRY)))
		return;
	GtkWidget *row = find_layer_row(editor, y, NULL);
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || !row)
		return;
	if (presses == 2) {
		begin_layer_rename(editor, row);
		return;
	}
	if (presses != 1)
		return;
	GdkModifierType modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
	int layer_index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "layer-index"));
	g_autoptr(GError) error = NULL;
	if (!select_layer(presented->document, layer_index, (modifiers & GDK_CONTROL_MASK) != 0,
					  (modifiers & GDK_SHIFT_MASK) != 0, &error)) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	gboolean clip = (modifiers & GDK_ALT_MASK) && can_clip_layer(presented->document, layer_index);
	if (clip && !toggle_clipping_mask(presented->document, FALSE, &error)) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	refresh_document_page(editor, page, clip);
}

/* ============================== Layer menu ============================== */

static gboolean is_layer_selected(Document *document, int layer_index) {
	Record *selected = get_record(document->record, "j");
	for (guint index = 0; index < count_records(selected); index++)
		if (get_item(selected, index)->value.number == layer_index)
			return TRUE;
	return FALSE;
}

static const guint32 LABEL_COLORS[] = {0xffffff, 0xff0000, 0xff9000, 0xffcc00, 0x44dd00, 0x0055ff, 0xaa00ff, 0x777777};

static int get_label_color(Document *document, int layer_index) {
	for (LayerTree *node = g_ptr_array_index(document->lookup, layer_index); node && node->layer; node = node->parent) {
		int color = get_number(get_record(node->layer, "add"), "lclr");
		if (color > 0 && color < (int)G_N_ELEMENTS(LABEL_COLORS))
			return color;
	}
	return 0;
}

static void draw_color_bar(GtkWidget *widget, GtkSnapshot *snapshot, int width, int height, gpointer data) {
	guint32 color = LABEL_COLORS[GPOINTER_TO_INT(data)];
	gtk_snapshot_append_color(
		snapshot, &(GdkRGBA){(color >> 16 & 255) / 255.0, (color >> 8 & 255) / 255.0, (color & 255) / 255.0, 1},
		&GRAPHENE_RECT_INIT(0, 2, width, height - 4));
}

/* A thin bar at the row start shows the label color, and rows without one keep the space empty */
static GtkWidget *create_color_bar(Document *document, int layer_index) {
	int color = get_label_color(document, layer_index);
	GtkWidget *bar = color ? create_snapshot_area(draw_color_bar, NULL, GINT_TO_POINTER(color))
						   : gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_size_request(bar, 4, -1);
	return bar;
}

static GMenuModel *create_layer_menu(Document *document) {
	GMenu *menu = g_menu_new(), *pixels = g_menu_new(), *layers = g_menu_new(), *clipping = g_menu_new();
	GMenu *merging = g_menu_new(), *colors = g_menu_new();
	g_menu_append(pixels, TEXT_SELECT_PIXELS, "win.select-layer-pixels");
	g_menu_append(layers, TEXT_DUPLICATE_LAYER, "win.duplicate-layer");
	g_menu_append(layers, TEXT_DELETE, "win.delete-layer");
	g_menu_append(clipping, TEXT_CLIPPING_MASK, "win.clipping-mask");
	Record *selected = get_record(document->record, "j");
	gboolean several =
		count_records(selected) > 1 || (count_records(selected) == 1 && is_layer_group(get_active_layer(document)));
	g_menu_append(merging, several ? TEXT_MERGE_SELECTED : TEXT_MERGE_LAYERS, "win.merge-layers");
	g_menu_append(merging, TEXT_FLATTEN_IMAGE, "win.flatten-image");
	const char *names[] = {TEXT_COLOR_NONE,	 TEXT_COLOR_RED,  TEXT_COLOR_ORANGE, TEXT_COLOR_YELLOW,
						   TEXT_COLOR_GREEN, TEXT_COLOR_BLUE, TEXT_COLOR_PURPLE, TEXT_COLOR_GRAY};
	for (guint color = 0; color < G_N_ELEMENTS(names); color++) {
		g_autoptr(GMenuItem) item = g_menu_item_new(names[color], NULL);
		g_menu_item_set_action_and_target_value(item, "win.layer-color", g_variant_new_int32(color));
		g_menu_append_item(colors, item);
	}
	GMenu *color_section = g_menu_new();
	g_menu_append_submenu(color_section, TEXT_LABEL_COLOR, G_MENU_MODEL(colors));
	GMenu *sections[] = {pixels, layers, clipping, merging, color_section};
	for (guint index = 0; index < G_N_ELEMENTS(sections); index++) {
		g_menu_append_section(menu, NULL, G_MENU_MODEL(sections[index]));
		g_object_unref(sections[index]);
	}
	g_object_unref(colors);
	return G_MENU_MODEL(menu);
}

/* A right click selects an unselected row first, then opens the layer menu there */
static void press_layer_menu(GtkGestureClick *gesture, int presses, double x, double y, Editor *editor) {
	GtkWidget *row = find_layer_row(editor, y, NULL);
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || !row || presented->document->preview)
		return;
	Document *document = presented->document;
	int layer_index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "layer-index"));
	g_autoptr(GError) error = NULL;
	if (!is_layer_selected(document, layer_index) && select_layer(document, layer_index, FALSE, FALSE, &error))
		refresh_document_page(editor, page, FALSE);
	g_autoptr(GMenuModel) model = create_layer_menu(document);
	/* The list rebuilds its rows after a command, so the menu hangs off the panel around it instead */
	GtkWidget *anchor = gtk_widget_get_ancestor(editor->layer_list, GTK_TYPE_SCROLLED_WINDOW);
	graphene_point_t point;
	if (gtk_widget_compute_point(editor->layer_list, anchor, &GRAPHENE_POINT_INIT(x, y), &point))
		show_context_menu(anchor, model, point.x, point.y);
}

/* ============================== Layer order by dragging ============================== */

static GdkContentProvider *prepare_layer_drag(GtkDragSource *source, double x, double y, GtkWidget *row) {
	return gdk_content_provider_new_typed(G_TYPE_INT, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "layer-index")));
}

static void begin_layer_drag(GtkDragSource *source, GdkDrag *drag, GtkWidget *row) {
	g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(row);
	gtk_drag_source_set_icon(source, paintable, 0, 0);
}

static GdkDragAction move_over_layers(GtkDropTarget *target, double x, double y, Editor *editor) {
	GtkWidget *row = find_layer_row(editor, y, NULL);
	if (row)
		gtk_list_box_drag_highlight_row(GTK_LIST_BOX(editor->layer_list), GTK_LIST_BOX_ROW(gtk_widget_get_parent(row)));
	else
		gtk_list_box_drag_unhighlight_row(GTK_LIST_BOX(editor->layer_list));
	GdkModifierType modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(target));
	return modifiers & GDK_ALT_MASK ? GDK_ACTION_COPY : GDK_ACTION_MOVE;
}

static void leave_layers(GtkDropTarget *target, Editor *editor) {
	gtk_list_box_drag_unhighlight_row(GTK_LIST_BOX(editor->layer_list));
}

static gboolean drop_on_layers(GtkDropTarget *target, const GValue *value, double x, double y, Editor *editor) {
	gtk_list_box_drag_unhighlight_row(GTK_LIST_BOX(editor->layer_list));
	double fraction = 0;
	GtkWidget *row = find_layer_row(editor, y, &fraction), *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!row || !presented || !G_VALUE_HOLDS_INT(value))
		return FALSE;
	GdkModifierType modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(target));
	g_autoptr(GError) error = NULL;
	if (!move_layers_to(presented->document, g_value_get_int(value),
						GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "layer-index")), fraction > .5,
						(modifiers & GDK_ALT_MASK) != 0, FALSE, &error)) {
		if (error)
			gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return FALSE;
	}
	refresh_document_page(editor, page, TRUE);
	return TRUE;
}

/* Ctrl applies the toggle to every sibling group state */
static void press_group_disclosure(GtkGestureClick *gesture, int presses, double x, double y, Editor *editor) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented)
		return;
	GtkWidget *button = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	GdkModifierType modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
	toggle_group_open(presented->document, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "layer-index")),
					  (modifiers & GDK_CONTROL_MASK) != 0);
	refresh_document_page(editor, page, FALSE);
}

/* Clipped rows step in this far so the layer they clip to stands out */
enum { CLIPPED_INDENT = 20 };

static gboolean is_clipping_base(Document *document, int layer_index) {
	LayerTree *node = g_ptr_array_index(document->lookup, layer_index);
	if (!node || !node->parent || get_boolean(node->layer, "jl"))
		return FALSE;
	GPtrArray *siblings = node->parent->children;
	for (guint index = 0; index + 1 < siblings->len; index++)
		if (g_ptr_array_index(siblings, index) == node)
			return get_boolean(((LayerTree *)g_ptr_array_index(siblings, index + 1))->layer, "jl");
	return FALSE;
}

static GtkWidget *create_layer_row(Editor *editor, Document *document, int layer_index, int depth) {
	Record *layer = get_item(get_record(document->record, "Z"), layer_index);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gboolean visible = is_layer_visible(layer);
	GtkWidget *eye = gtk_button_new_from_icon_name(visible ? "view-reveal-symbolic" : "view-conceal-symbolic");
	gtk_widget_add_css_class(eye, "flat");
	gtk_widget_set_tooltip_text(eye, visible ? TEXT_HIDE_LAYER : TEXT_SHOW_LAYER);
	gtk_accessible_update_property(GTK_ACCESSIBLE(eye), GTK_ACCESSIBLE_PROPERTY_LABEL,
								   visible ? TEXT_HIDE_LAYER : TEXT_SHOW_LAYER, -1);
	g_object_set_data(G_OBJECT(eye), "layer-index", GINT_TO_POINTER(layer_index));
	g_object_set_data(G_OBJECT(eye), "eye", eye);
	/* Pointer presses go through press_eye, so this signal comes from the keyboard */
	g_signal_connect(eye, "clicked", G_CALLBACK(toggle_visibility), editor);
	gtk_widget_set_sensitive(eye, !document->preview);
	gtk_box_append(GTK_BOX(row), create_color_bar(document, layer_index));
	gtk_box_append(GTK_BOX(row), eye);
	if (get_boolean(layer, "jl")) {
		GtkWidget *clipped = gtk_image_new_from_icon_name("go-down-symbolic");
		gtk_widget_set_margin_start(clipped, CLIPPED_INDENT);
		gtk_widget_set_tooltip_text(clipped, TEXT_CLIPPED_LAYER);
		gtk_accessible_update_property(GTK_ACCESSIBLE(clipped), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_CLIPPED_LAYER, -1);
		gtk_box_append(GTK_BOX(row), clipped);
	}
	if (is_layer_group(layer)) {
		gboolean open = get_number(get_record(layer, "add"), "lsct") == 1;
		GtkWidget *disclosure = gtk_button_new_from_icon_name(open ? "pan-down-symbolic" : "pan-end-symbolic");
		gtk_widget_add_css_class(disclosure, "flat");
		gtk_widget_set_tooltip_text(disclosure, open ? TEXT_CLOSE_GROUP : TEXT_OPEN_GROUP);
		gtk_accessible_update_property(GTK_ACCESSIBLE(disclosure), GTK_ACCESSIBLE_PROPERTY_LABEL,
									   open ? TEXT_CLOSE_GROUP : TEXT_OPEN_GROUP, -1);
		gtk_widget_set_margin_start(disclosure, depth * 16);
		g_object_set_data(G_OBJECT(disclosure), "layer-index", GINT_TO_POINTER(layer_index));
		GtkGesture *click = gtk_gesture_click_new();
		g_signal_connect(click, "pressed", G_CALLBACK(press_group_disclosure), editor);
		gtk_widget_add_controller(disclosure, GTK_EVENT_CONTROLLER(click));
		gtk_box_append(GTK_BOX(row), disclosure);
		depth = 0;
	}
	GtkWidget *thumbnail = is_layer_group(layer)		? NULL
						   : is_adjustment_layer(layer) ? create_adjustment_button(editor, layer_index)
														: create_thumbnail_widget(document, layer);
	if (thumbnail) {
		gtk_widget_set_margin_start(thumbnail, depth * 16);
		gtk_box_append(GTK_BOX(row), thumbnail);
		depth = 0;
	}
	GtkWidget *label = gtk_label_new(get_layer_name(layer));
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_widget_set_margin_start(label, depth * 16);
	if (!visible)
		gtk_widget_add_css_class(label, "dim-label");
	if (is_layer_group(layer))
		gtk_widget_add_css_class(label, "heading");
	/* Photoshop underlines the name of the layer that others clip to */
	if (is_clipping_base(document, layer_index)) {
		PangoAttrList *attributes = pango_attr_list_new();
		pango_attr_list_insert(attributes, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
		gtk_label_set_attributes(GTK_LABEL(label), attributes);
		pango_attr_list_unref(attributes);
	}
	gtk_widget_set_tooltip_text(label, TEXT_RENAME_LAYER);
	g_object_set_data(G_OBJECT(row), "name-label", label);
	gtk_box_append(GTK_BOX(row), label);
	gtk_widget_set_margin_top(row, 2);
	gtk_widget_set_margin_bottom(row, 2);
	g_object_set_data(G_OBJECT(row), "layer-index", GINT_TO_POINTER(layer_index));
	GtkDragSource *source = gtk_drag_source_new();
	gtk_drag_source_set_actions(source, GDK_ACTION_MOVE | GDK_ACTION_COPY);
	g_signal_connect(source, "prepare", G_CALLBACK(prepare_layer_drag), row);
	g_signal_connect(source, "drag-begin", G_CALLBACK(begin_layer_drag), row);
	gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));
	return row;
}

/* What the rows show: each layer's record, name, flags, group state and pixels; a selection change alone keeps it */
static char *describe_layer_rows(Document *document) {
	Record *layers = get_record(document->record, "Z");
	GString *description = g_string_new(NULL);
	/* The history position changes whenever pixels may have, such as after painting, so thumbnails refresh */
	g_string_append_printf(description, "%p;%g;%u;", (void *)document, get_number(document->record, "NP"),
						   count_records(get_record(document->record, "history")));
	for (guint index = 0; index < count_records(layers); index++) {
		Record *layer = get_item(layers, index);
		RasterExtent extent = read_extent(get_record(layer, "V"));
		g_string_append_printf(description, "%p,%s,%g,%g,%p,%d,%d,%d,%d,%g,%d;", (void *)layer, get_layer_name(layer),
							   get_number(layer, "NF"), get_number(get_record(layer, "add"), "lsct"),
							   (void *)get_record(layer, "buffer"), extent.x, extent.y, extent.width, extent.height,
							   get_number(get_record(layer, "add"), "lclr"), get_boolean(layer, "jl"));
	}
	return g_string_free(description, FALSE);
}

/* Only the highlight follows a new selection, so rows under the pointer survive for double clicks and drags */
static void refresh_layer_selection(Editor *editor, Document *document) {
	for (GtkWidget *row = gtk_widget_get_first_child(editor->layer_list); row; row = gtk_widget_get_next_sibling(row)) {
		if (!GTK_IS_LIST_BOX_ROW(row))
			continue;
		GtkWidget *child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
		int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "layer-index"));
		if (is_layer_selected(document, index))
			gtk_widget_set_state_flags(row, GTK_STATE_FLAG_SELECTED, FALSE);
		else
			gtk_widget_unset_state_flags(row, GTK_STATE_FLAG_SELECTED);
	}
}

void refresh_layers_panel(Editor *editor) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	g_autofree char *rows = presented ? describe_layer_rows(presented->document) : g_strdup("");
	gboolean same_rows = editor->layer_rows && !strcmp(rows, editor->layer_rows);
	if (!same_rows) {
		/* A rebuild removes an open name field, so its text is saved before it goes */
		end_layer_rename(editor, TRUE);
		g_free(editor->layer_rows);
		editor->layer_rows = g_steal_pointer(&rows);
		gtk_list_box_remove_all(GTK_LIST_BOX(editor->layer_list));
	}
	if (!presented) {
		set_percent_value(editor, editor->opacity, 255, FALSE);
		set_percent_value(editor, editor->fill, 255, FALSE);
		refresh_blend_mode(editor, NULL, FALSE);
		return;
	}
	Document *document = presented->document;
	Record *layers = get_record(document->record, "Z");
	if (same_rows)
		refresh_layer_selection(editor, document);
	/* Group closing records sit below their contents so the depth grows while walking down */
	int depth = 0, hidden_below = -1;
	for (gint index = same_rows ? -1 : (gint)count_records(layers) - 1; index >= 0; index--) {
		Record *layer = get_item(layers, index);
		if (get_number(get_record(layer, "add"), "lsct") == 3) {
			depth = MAX(0, depth - 1);
			if (depth == hidden_below)
				hidden_below = -1;
			continue;
		}
		if (hidden_below >= 0) {
			if (is_layer_group(layer))
				depth++;
			continue;
		}
		/* Closed groups hide their contents until their own closing record */
		if (get_number(get_record(layer, "add"), "lsct") == 2)
			hidden_below = depth;
		gtk_list_box_append(GTK_LIST_BOX(editor->layer_list), create_layer_row(editor, document, index, depth));
		GtkWidget *list_row = gtk_widget_get_last_child(editor->layer_list);
		if (is_layer_selected(document, index))
			gtk_widget_set_state_flags(list_row, GTK_STATE_FLAG_SELECTED, FALSE);
		if (is_layer_group(layer))
			depth++;
	}
	Record *layer = get_active_layer(document);
	gboolean editable = can_edit_layer_values(document);
	Record *fill = get_record(get_record(layer, "add"), "iOpa");
	set_percent_value(editor, editor->opacity, layer ? get_number(layer, "nv") : 255, editable);
	set_percent_value(editor, editor->fill, !fill || fill->type == RECORD_NULL ? 255 : fill->value.number,
					  editable && !is_layer_group(layer));
	refresh_blend_mode(editor, layer, editable);
}
