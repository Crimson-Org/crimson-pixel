#include "editor.h"
#include "value-field.h"
#include "../lang/en.h"

typedef struct {
	Editor *editor;
	GtkWidget *mode, *width, *height, *swap, *resolution;
	double remembered[3][2];
	CropMode shown;
	gboolean updating;
} CropControls;

static const double CROP_PRESETS[][2] = {{0, 0}, {1, 1}, {4, 3}, {3, 2}, {16, 9}};

enum { PRESET_ORIGINAL = 3 };

static void read_crop_controls(CropControls *controls) {
	CropOptions *options = &controls->editor->crop_options;
	options->mode = controls->shown;
	options->width = get_value_field_value(controls->width);
	options->height = get_value_field_value(controls->height);
	if (options->mode == CROP_SIZE) {
		options->width = round_like_source(options->width);
		options->height = round_like_source(options->height);
	}
	options->resolution = get_value_field_value(controls->resolution);
}

static void show_crop_values(CropControls *controls, double width, double height) {
	controls->updating = TRUE;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(controls->mode), controls->shown);
	set_value_field(controls->width, width);
	set_value_field(controls->height, height);
	gboolean fixed = controls->shown != CROP_FREE;
	gtk_widget_set_sensitive(controls->width, fixed);
	gtk_widget_set_sensitive(controls->height, fixed);
	gtk_widget_set_sensitive(controls->swap, fixed);
	gtk_widget_set_visible(controls->resolution, controls->shown == CROP_SIZE);
	controls->updating = FALSE;
}

static void apply_crop_controls(CropControls *controls) {
	if (get_value_field_value(controls->width) < 1)
		set_value_field(controls->width, 1);
	if (get_value_field_value(controls->height) < 1)
		set_value_field(controls->height, 1);
	read_crop_controls(controls);
	configure_crop(controls->editor);
}

static void get_preset(CropControls *controls, guint preset, double *width, double *height) {
	*width = CROP_PRESETS[preset][0];
	*height = CROP_PRESETS[preset][1];
	if (preset != 0)
		return;
	PresentedDocument *presented = get_page_document(get_current_page(controls->editor));
	*width = presented ? document_width(presented->document) : 10;
	*height = presented ? document_height(presented->document) : 5;
}

static void choose_crop_mode(GtkDropDown *list, GParamSpec *specification, CropControls *controls) {
	if (controls->updating)
		return;
	guint chosen = gtk_drop_down_get_selected(list);
	if (chosen >= PRESET_ORIGINAL) {
		double width, height;
		get_preset(controls, chosen - PRESET_ORIGINAL, &width, &height);
		controls->remembered[CROP_RATIO][0] = width;
		controls->remembered[CROP_RATIO][1] = height;
		show_crop_values(controls, width, height);
		chosen = CROP_RATIO;
	}
	controls->remembered[controls->shown][0] = get_value_field_value(controls->width);
	controls->remembered[controls->shown][1] = get_value_field_value(controls->height);
	controls->shown = chosen;
	show_crop_values(controls, controls->remembered[chosen][0], controls->remembered[chosen][1]);
	apply_crop_controls(controls);
}

static void change_crop_value(GtkWidget *field, double value, gpointer data) {
	CropControls *controls = data;
	if (!controls->updating)
		apply_crop_controls(controls);
}

static void swap_crop_sides(GtkButton *button, CropControls *controls) {
	double width = get_value_field_value(controls->width);
	set_value_field(controls->width, get_value_field_value(controls->height));
	set_value_field(controls->height, width);
	apply_crop_controls(controls);
}

static void press_straighten(GtkButton *button, Editor *editor) {
	start_straighten(editor);
}

static void toggle_crop_delete(GtkCheckButton *button, Editor *editor) {
	editor->crop_options.delete_pixels = gtk_check_button_get_active(button);
}

static GtkWidget *create_crop_field(CropControls *controls, const char *label, double maximum, int digits) {
	GtkWidget *field = create_value_field(label, NULL, 0, maximum, change_crop_value, controls);
	set_value_field_digits(field, digits);
	return field;
}

GtkWidget *create_crop_options(Editor *editor) {
	CropControls *controls = g_new0(CropControls, 1);
	*controls = (CropControls){.editor = editor, .remembered = {{0, 0}, {1, 1}, {100, 100}}};
	editor->crop_options = (CropOptions){CROP_FREE, 0, 0, 0, FALSE};
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	g_object_set_data_full(G_OBJECT(box), "crop-controls", controls, g_free);
	controls->mode = gtk_drop_down_new_from_strings((const char *[]){TEXT_CROP_FREE, TEXT_CROP_RATIO, TEXT_CROP_SIZE,
																	 TEXT_CROP_ORIGINAL_RATIO, "1 : 1", "4 : 3",
																	 "3 : 2", "16 : 9", NULL});
	gtk_accessible_update_property(GTK_ACCESSIBLE(controls->mode), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_CROP_MODE, -1);
	g_signal_connect(controls->mode, "notify::selected", G_CALLBACK(choose_crop_mode), controls);
	controls->width = create_crop_field(controls, TEXT_CROP_WIDTH, 30000, 2);
	controls->swap = gtk_button_new_with_label("⇄");
	gtk_widget_add_css_class(controls->swap, "flat");
	gtk_widget_set_tooltip_text(controls->swap, TEXT_CROP_SWAP);
	g_signal_connect(controls->swap, "clicked", G_CALLBACK(swap_crop_sides), controls);
	controls->height = create_crop_field(controls, TEXT_CROP_HEIGHT, 30000, 2);
	controls->resolution = create_crop_field(controls, TEXT_CROP_RESOLUTION, 10000, 3);
	GtkWidget *straighten = gtk_button_new_with_label(TEXT_CROP_STRAIGHTEN);
	gtk_widget_set_tooltip_text(straighten, TEXT_CROP_STRAIGHTEN_HELP);
	g_signal_connect(straighten, "clicked", G_CALLBACK(press_straighten), editor);
	GtkWidget *delete_pixels = gtk_check_button_new_with_label(TEXT_CROP_DELETE);
	g_signal_connect(delete_pixels, "toggled", G_CALLBACK(toggle_crop_delete), editor);
	GtkWidget *children[] = {controls->mode,	   controls->width, controls->swap, controls->height,
							 controls->resolution, straighten,		delete_pixels};
	for (guint index = 0; index < G_N_ELEMENTS(children); index++)
		gtk_box_append(GTK_BOX(box), children[index]);
	show_crop_values(controls, 0, 0);
	editor->crop_bar = box;
	gtk_widget_set_visible(box, FALSE);
	return box;
}
