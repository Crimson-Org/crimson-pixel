#include "command-dialog.h"
#include "crop.h"
#include "../lang/en.h"

/* Image Size and Canvas Size inputs after the shared dialog */
typedef struct {
	CommandDialog base;
	GtkWidget *width, *height, *resolution, *proportions, *resample, *relative, *trim_colors[3], *trim_sides[4];
	GtkWidget *anchors[9];
	int document_width, document_height, anchor;
	gboolean updating;
} SizeDialog;

static gboolean keep_proportions = TRUE;
static ResampleMode resample_mode = RESAMPLE_BILINEAR;
static gboolean relative_size;
static int canvas_anchor = 4;
static TrimColor trim_color = TRIM_TOP_LEFT;
static gboolean trim_sides[4] = {TRUE, TRUE, TRUE, TRUE};

static int read_number(GtkWidget *input) {
	gtk_spin_button_update(GTK_SPIN_BUTTON(input));
	return gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(input));
}

static void set_number_quietly(SizeDialog *dialog, GtkWidget *input, double value) {
	dialog->updating = TRUE;
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(input), value);
	dialog->updating = FALSE;
}

static void refresh_after_resize(Editor *editor) {
	refresh_command_page(editor, TRUE);
	GtkWidget *page = get_current_page(editor);
	if (page)
		fit_document_view(get_page_view(page, "primary"));
}

/* ============================== Image Size ============================== */

static void change_image_side(GtkSpinButton *input, SizeDialog *dialog) {
	if (dialog->updating || !gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->proportions)))
		return;
	double ratio = (double)dialog->document_width / dialog->document_height;
	if (GTK_WIDGET(input) == dialog->width)
		set_number_quietly(dialog, dialog->height, MAX(1, round(gtk_spin_button_get_value(input) / ratio)));
	else
		set_number_quietly(dialog, dialog->width, MAX(1, round(gtk_spin_button_get_value(input) * ratio)));
}

static void toggle_proportions(GtkCheckButton *button, SizeDialog *dialog) {
	keep_proportions = gtk_check_button_get_active(button);
	if (keep_proportions)
		change_image_side(GTK_SPIN_BUTTON(dialog->width), dialog);
}

static gboolean accept_image_size(CommandDialog *base) {
	SizeDialog *dialog = (SizeDialog *)base;
	Document *document = get_command_document(base->editor);
	resample_mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->resample));
	if (!document)
		return TRUE;
	if (resize_image(document, read_number(dialog->width), read_number(dialog->height), read_number(dialog->resolution),
					 resample_mode, FALSE))
		refresh_after_resize(base->editor);
	return TRUE;
}

void show_image_size_dialog(Editor *editor) {
	Document *document = get_command_document(editor);
	if (!document)
		return;
	SizeDialog *dialog = (SizeDialog *)create_command_dialog(editor, TEXT_IMAGE_SIZE, sizeof(SizeDialog));
	dialog->base.accept = accept_image_size;
	dialog->document_width = document_width(document);
	dialog->document_height = document_height(document);
	dialog->width = add_number_input(&dialog->base, TEXT_WIDTH, TEXT_PIXEL_UNIT, 1, 30000, dialog->document_width, 0);
	dialog->height =
		add_number_input(&dialog->base, TEXT_HEIGHT, TEXT_PIXEL_UNIT, 1, 30000, dialog->document_height, 1);
	dialog->proportions = gtk_check_button_new_with_label(TEXT_KEEP_PROPORTIONS);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->proportions), keep_proportions);
	gtk_grid_attach(GTK_GRID(dialog->base.grid), dialog->proportions, 1, 2, 2, 1);
	double resolution = get_number(document->record, "Qf");
	dialog->resolution = add_number_input(&dialog->base, TEXT_RESOLUTION, TEXT_RESOLUTION_UNIT, 1, 30000,
										  resolution > 0 ? resolution : 72, 3);
	dialog->resample = gtk_drop_down_new_from_strings(
		(const char *[]){TEXT_RESAMPLE_NEAREST, TEXT_RESAMPLE_BILINEAR, TEXT_RESAMPLE_SHARPER, NULL});
	gtk_drop_down_set_selected(GTK_DROP_DOWN(dialog->resample), resample_mode);
	add_dialog_input(dialog->base.grid, TEXT_INTERPOLATION, dialog->resample, 4);
	g_signal_connect(dialog->width, "value-changed", G_CALLBACK(change_image_side), dialog);
	g_signal_connect(dialog->height, "value-changed", G_CALLBACK(change_image_side), dialog);
	g_signal_connect(dialog->proportions, "toggled", G_CALLBACK(toggle_proportions), dialog);
	gtk_window_present(GTK_WINDOW(dialog->base.window));
}

/* ============================== Canvas Size ============================== */

/* The anchor marks its cell and arrows next to it point where the new space goes */
static void refresh_anchor_buttons(SizeDialog *dialog) {
	static const char *const ARROWS[9] = {
		NULL, "go-up-symbolic",	  NULL, "go-previous-symbolic", "media-record-symbolic", "go-next-symbolic",
		NULL, "go-down-symbolic", NULL};
	int column = dialog->anchor % 3, row = dialog->anchor / 3;
	for (int cell = 0; cell < 9; cell++) {
		int across = cell % 3 - column, down = cell / 3 - row;
		const char *icon = abs(across) + abs(down) <= 1 ? ARROWS[(down + 1) * 3 + across + 1] : NULL;
		GtkWidget *button = dialog->anchors[cell];
		if (icon)
			gtk_button_set_icon_name(GTK_BUTTON(button), icon);
		else
			gtk_button_set_child(GTK_BUTTON(button), NULL);
		if (cell == dialog->anchor)
			gtk_widget_add_css_class(button, "suggested-action");
		else
			gtk_widget_remove_css_class(button, "suggested-action");
	}
}

static void press_anchor(GtkButton *button, SizeDialog *dialog) {
	dialog->anchor = canvas_anchor = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "anchor"));
	refresh_anchor_buttons(dialog);
}

static void toggle_relative(GtkCheckButton *button, SizeDialog *dialog) {
	relative_size = gtk_check_button_get_active(button);
	set_number_quietly(dialog, dialog->width, relative_size ? 0 : dialog->document_width);
	set_number_quietly(dialog, dialog->height, relative_size ? 0 : dialog->document_height);
}

static gboolean accept_canvas_size(CommandDialog *base) {
	SizeDialog *dialog = (SizeDialog *)base;
	Document *document = get_command_document(base->editor);
	if (!document)
		return TRUE;
	int width = read_number(dialog->width), height = read_number(dialog->height);
	if (relative_size) {
		width += dialog->document_width;
		height += dialog->document_height;
	}
	if (resize_canvas(document, width, height, dialog->anchor, FALSE))
		refresh_after_resize(base->editor);
	return TRUE;
}

void show_canvas_size_dialog(Editor *editor) {
	Document *document = get_command_document(editor);
	if (!document)
		return;
	SizeDialog *dialog = (SizeDialog *)create_command_dialog(editor, TEXT_CANVAS_SIZE, sizeof(SizeDialog));
	dialog->base.accept = accept_canvas_size;
	dialog->document_width = document_width(document);
	dialog->document_height = document_height(document);
	dialog->anchor = canvas_anchor;
	dialog->width = add_number_input(&dialog->base, TEXT_WIDTH, TEXT_PIXEL_UNIT, -30000, 30000,
									 relative_size ? 0 : dialog->document_width, 0);
	dialog->height = add_number_input(&dialog->base, TEXT_HEIGHT, TEXT_PIXEL_UNIT, -30000, 30000,
									  relative_size ? 0 : dialog->document_height, 1);
	dialog->relative = gtk_check_button_new_with_label(TEXT_RELATIVE);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->relative), relative_size);
	gtk_grid_attach(GTK_GRID(dialog->base.grid), dialog->relative, 1, 2, 2, 1);
	GtkWidget *anchors = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(anchors), 2);
	gtk_grid_set_column_spacing(GTK_GRID(anchors), 2);
	for (int cell = 0; cell < 9; cell++) {
		GtkWidget *button = dialog->anchors[cell] = gtk_button_new();
		gtk_widget_set_size_request(button, 32, 32);
		g_object_set_data(G_OBJECT(button), "anchor", GINT_TO_POINTER(cell));
		gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_ANCHOR, -1);
		g_signal_connect(button, "clicked", G_CALLBACK(press_anchor), dialog);
		gtk_grid_attach(GTK_GRID(anchors), button, cell % 3, cell / 3, 1, 1);
	}
	refresh_anchor_buttons(dialog);
	gtk_widget_set_halign(anchors, GTK_ALIGN_START);
	add_dialog_input(dialog->base.grid, TEXT_ANCHOR, anchors, 3);
	g_signal_connect(dialog->relative, "toggled", G_CALLBACK(toggle_relative), dialog);
	gtk_window_present(GTK_WINDOW(dialog->base.window));
}

/* ============================== Trim and Reveal All ============================== */

void trim_editor_document(Editor *editor, TrimColor color, const gboolean sides[4]) {
	Document *document = get_command_document(editor);
	if (document && trim_document(document, color, sides, FALSE))
		refresh_after_resize(editor);
}

void reveal_editor_document(Editor *editor) {
	Document *document = get_command_document(editor);
	if (document && reveal_all(document, FALSE))
		refresh_after_resize(editor);
}

static gboolean accept_trim(CommandDialog *base) {
	SizeDialog *dialog = (SizeDialog *)base;
	for (int color = 0; color < 3; color++)
		if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->trim_colors[color])))
			trim_color = color;
	for (int side = 0; side < 4; side++)
		trim_sides[side] = gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->trim_sides[side]));
	trim_editor_document(base->editor, trim_color, trim_sides);
	return TRUE;
}

void show_trim_dialog(Editor *editor) {
	if (!get_command_document(editor))
		return;
	SizeDialog *dialog = (SizeDialog *)create_command_dialog(editor, TEXT_TRIM, sizeof(SizeDialog));
	dialog->base.accept = accept_trim;
	const char *colors[] = {TEXT_TRIM_TOP_LEFT, TEXT_TRIM_BOTTOM_RIGHT, TEXT_TRIM_TRANSPARENT};
	GtkWidget *color_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	for (int color = 0; color < 3; color++) {
		dialog->trim_colors[color] = gtk_check_button_new_with_label(colors[color]);
		if (color)
			gtk_check_button_set_group(GTK_CHECK_BUTTON(dialog->trim_colors[color]),
									   GTK_CHECK_BUTTON(dialog->trim_colors[0]));
		gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->trim_colors[color]), (TrimColor)color == trim_color);
		gtk_box_append(GTK_BOX(color_box), dialog->trim_colors[color]);
	}
	add_dialog_input(dialog->base.grid, TEXT_TRIM_COLOR, color_box, 0);
	const char *sides[] = {TEXT_SIDE_TOP, TEXT_SIDE_LEFT, TEXT_SIDE_BOTTOM, TEXT_SIDE_RIGHT};
	GtkWidget *side_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	for (int side = 0; side < 4; side++) {
		dialog->trim_sides[side] = gtk_check_button_new_with_label(sides[side]);
		gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->trim_sides[side]), trim_sides[side]);
		gtk_box_append(GTK_BOX(side_box), dialog->trim_sides[side]);
	}
	add_dialog_input(dialog->base.grid, TEXT_TRIM_SIDES, side_box, 1);
	gtk_window_present(GTK_WINDOW(dialog->base.window));
}
