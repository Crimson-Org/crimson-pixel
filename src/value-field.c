#include <math.h>
#include <stdlib.h>

#include "value-field.h"

/* A labelled number with typing, label scrubbing and a slider popover fields */
typedef struct {
	GtkWidget *entry, *scale;
	double minimum, maximum, value, scrub_start;
	/* Decimal places kept when a value is typed */
	int digits;
	ValueFieldChanged changed;
	gpointer data;
	gboolean updating;
} ValueField;

static ValueField *get_value_field(GtkWidget *field) {
	return g_object_get_data(G_OBJECT(field), "value-field");
}

static void show_value(ValueField *field) {
	field->updating = TRUE;
	g_autofree char *text = g_strdup_printf("%g", field->value);
	gtk_editable_set_text(GTK_EDITABLE(field->entry), text);
	gtk_range_set_value(GTK_RANGE(field->scale), field->value);
	field->updating = FALSE;
}

static void apply_value(GtkWidget *widget, ValueField *field, double value, gboolean notify) {
	double scale = pow(10, field->digits);
	value = CLAMP(round(value * scale) / scale, field->minimum, field->maximum);
	gboolean changed = value != field->value;
	field->value = value;
	show_value(field);
	if (changed && notify && field->changed)
		field->changed(widget, value, field->data);
}

static void commit_entry(GtkWidget *entry, GtkWidget *widget) {
	ValueField *field = get_value_field(widget);
	if (field->updating)
		return;
	char *end;
	double value = g_ascii_strtod(gtk_editable_get_text(GTK_EDITABLE(entry)), &end);
	apply_value(widget, field, end == gtk_editable_get_text(GTK_EDITABLE(entry)) ? field->value : value, TRUE);
}

static void leave_entry(GtkEventControllerFocus *controller, GtkWidget *widget) {
	commit_entry(get_value_field(widget)->entry, widget);
}

static void move_scale(GtkRange *range, GtkWidget *widget) {
	ValueField *field = get_value_field(widget);
	if (!field->updating)
		apply_value(widget, field, gtk_range_get_value(range), TRUE);
}

static void begin_scrub(GtkGestureDrag *gesture, double x, double y, GtkWidget *widget) {
	ValueField *field = get_value_field(widget);
	field->scrub_start = field->value;
}

/* Dragging the label changes the value faster for wide ranges */
static void update_scrub(GtkGestureDrag *gesture, double x, double y, GtkWidget *widget) {
	ValueField *field = get_value_field(widget);
	double step = MAX(1, (field->maximum - field->minimum) / 400);
	apply_value(widget, field, field->scrub_start + x * step, TRUE);
}

static gboolean scroll_value(GtkEventControllerScroll *controller, double dx, double dy, GtkWidget *widget) {
	ValueField *field = get_value_field(widget);
	apply_value(widget, field, field->value - dy, TRUE);
	return TRUE;
}

GtkWidget *create_value_field(const char *label_text, const char *unit, double minimum, double maximum,
							  ValueFieldChanged changed, gpointer data) {
	ValueField *field = g_new0(ValueField, 1);
	*field = (ValueField){.minimum = minimum, .maximum = maximum, .value = minimum, .changed = changed, .data = data};
	GtkWidget *widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_add_css_class(widget, "value-field");
	g_object_set_data_full(G_OBJECT(widget), "value-field", field, g_free);
	GtkWidget *label = gtk_label_new_with_mnemonic(label_text);
	gtk_widget_set_cursor_from_name(label, "ew-resize");
	gtk_widget_add_css_class(label, "value-label");
	GtkGesture *scrub = gtk_gesture_drag_new();
	g_signal_connect(scrub, "drag-begin", G_CALLBACK(begin_scrub), widget);
	g_signal_connect(scrub, "drag-update", G_CALLBACK(update_scrub), widget);
	gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(scrub));
	field->entry = gtk_entry_new();
	gtk_editable_set_width_chars(GTK_EDITABLE(field->entry), 4);
	gtk_editable_set_max_width_chars(GTK_EDITABLE(field->entry), 5);
	gtk_entry_set_input_purpose(GTK_ENTRY(field->entry), GTK_INPUT_PURPOSE_NUMBER);
	gtk_widget_add_css_class(field->entry, "value-entry");
	gtk_label_set_mnemonic_widget(GTK_LABEL(label), field->entry);
	g_signal_connect(field->entry, "activate", G_CALLBACK(commit_entry), widget);
	GtkEventController *focus = gtk_event_controller_focus_new();
	g_signal_connect(focus, "leave", G_CALLBACK(leave_entry), widget);
	gtk_widget_add_controller(field->entry, focus);
	GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	g_signal_connect(scroll, "scroll", G_CALLBACK(scroll_value), widget);
	gtk_widget_add_controller(widget, scroll);
	field->scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, minimum, maximum, 1);
	gtk_widget_set_size_request(field->scale, 220, -1);
	gtk_scale_set_draw_value(GTK_SCALE(field->scale), FALSE);
	g_signal_connect(field->scale, "value-changed", G_CALLBACK(move_scale), widget);
	GtkWidget *popover = gtk_popover_new(), *slider = gtk_menu_button_new();
	gtk_popover_set_child(GTK_POPOVER(popover), field->scale);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(slider), popover);
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(slider), "pan-down-symbolic");
	gtk_widget_add_css_class(slider, "flat");
	gtk_widget_set_tooltip_text(slider, label_text);
	gtk_box_append(GTK_BOX(widget), label);
	gtk_box_append(GTK_BOX(widget), field->entry);
	if (unit) {
		GtkWidget *unit_label = gtk_label_new(unit);
		gtk_widget_add_css_class(unit_label, "dim-label");
		gtk_box_append(GTK_BOX(widget), unit_label);
	}
	gtk_box_append(GTK_BOX(widget), slider);
	show_value(field);
	return widget;
}

void set_value_field(GtkWidget *widget, double value) {
	ValueField *field = get_value_field(widget);
	field->value = CLAMP(value, field->minimum, field->maximum);
	show_value(field);
}

double get_value_field_value(GtkWidget *widget) {
	return get_value_field(widget)->value;
}

void set_value_field_digits(GtkWidget *widget, int digits) {
	get_value_field(widget)->digits = digits;
}
