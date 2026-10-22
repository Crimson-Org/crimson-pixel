#include "command-dialog.h"
#include "fill-commands.h"
#include "history.h"
#include "pixel-selection.h"
#include "selection-modify.h"
#include "../lang/en.h"

/* Modify, Fill and Stroke inputs after the shared dialog */
typedef struct {
	CommandDialog base;
	/* Modify dialogs keep their amount field, canvas bounds switch and the history step their preview made */
	SelectionModification modification;
	GtkWidget *amount, *bounds;
	Record *preview;
	/* Fill and Stroke inputs */
	GtkWidget *content, *color, *opacity, *preserve, *position;
} FillDialog;

static int modify_amounts[] = {1, 1, 1, 1, 1};
static gboolean modify_bounds[G_N_ELEMENTS(modify_amounts)];
static FillContent fill_content = FILL_FOREGROUND;
static int fill_opacity = 100;
static gboolean fill_preserve;
static guint32 fill_custom_color;
static int stroke_width = 3, stroke_opacity = 100;
static StrokeLocation stroke_location = STROKE_OUTSIDE;

/* ============================== Modify ============================== */

static int read_modify_amount(FillDialog *dialog) {
	return gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(dialog->amount));
}

static void undo_modify_preview(FillDialog *dialog) {
	Document *document = get_command_document(dialog->base.editor);
	if (dialog->preview && document && get_latest_history_entry(document) == dialog->preview)
		edit_document_history(dialog->base.editor, FALSE);
	dialog->preview = NULL;
}

static void preview_modification(FillDialog *dialog) {
	undo_modify_preview(dialog);
	Document *document = get_command_document(dialog->base.editor);
	gboolean bounds = dialog->bounds && gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->bounds));
	modify_amounts[dialog->modification] = read_modify_amount(dialog);
	modify_bounds[dialog->modification] = bounds;
	if (!document || !modify_selection(document, dialog->modification, read_modify_amount(dialog), bounds, FALSE))
		return;
	dialog->preview = get_latest_history_entry(document);
	refresh_command_page(dialog->base.editor, FALSE);
}

static void cancel_modification(CommandDialog *base) {
	undo_modify_preview((FillDialog *)base);
}

static void change_modify_amount(GtkSpinButton *button, FillDialog *dialog) {
	preview_modification(dialog);
}

static void change_modify_bounds(GtkCheckButton *button, FillDialog *dialog) {
	preview_modification(dialog);
}

static gboolean accept_modification(CommandDialog *base) {
	FillDialog *dialog = (FillDialog *)base;
	gtk_spin_button_update(GTK_SPIN_BUTTON(dialog->amount));
	return TRUE;
}

void show_modify_dialog(Editor *editor, int modification) {
	Document *document = get_command_document(editor);
	if (!document || !get_document_selection(document))
		return;
	const char *titles[] = {TEXT_MODIFY_BORDER, TEXT_MODIFY_SMOOTH, TEXT_MODIFY_EXPAND, TEXT_MODIFY_CONTRACT,
							TEXT_MODIFY_FEATHER};
	FillDialog *dialog = (FillDialog *)create_command_dialog(editor, titles[modification], sizeof(FillDialog));
	dialog->modification = modification;
	dialog->base.accept = accept_modification;
	dialog->base.cancel = cancel_modification;
	dialog->amount =
		add_number_input(&dialog->base, titles[modification], TEXT_PIXEL_UNIT, 0, 255, modify_amounts[modification], 0);
	if (modification != MODIFY_BORDER) {
		dialog->bounds = gtk_check_button_new_with_label(TEXT_APPLY_AT_CANVAS_BOUNDS);
		gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->bounds), modify_bounds[modification]);
		gtk_grid_attach(GTK_GRID(dialog->base.grid), dialog->bounds, 0, 1, 3, 1);
		g_signal_connect(dialog->bounds, "toggled", G_CALLBACK(change_modify_bounds), dialog);
	}
	g_signal_connect(dialog->amount, "value-changed", G_CALLBACK(change_modify_amount), dialog);
	preview_modification(dialog);
	gtk_window_present(GTK_WINDOW(dialog->base.window));
}

/* ============================== Fill ============================== */

static const char *get_fill_operation(Editor *editor) {
	return !g_strcmp0(editor->tool, PAINT_TOOL_PENCIL) ? PAINT_TOOL_PENCIL : PAINT_TOOL_BRUSH;
}

static gboolean run_fill(Editor *editor, guint32 color, int opacity, gboolean preserve, GError **error) {
	Document *document = get_command_document(editor);
	if (!document)
		return TRUE;
	if (!fill_document(document, get_fill_operation(editor), color, opacity, preserve, error))
		return FALSE;
	refresh_command_page(editor, TRUE);
	return TRUE;
}

void fill_with_editor_color(Editor *editor, gboolean background) {
	g_autoptr(GError) error = NULL;
	guint32 color = read_color_button(background ? editor->background : editor->foreground);
	if (!run_fill(editor, color, 100, FALSE, &error))
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
}

static void change_fill_content(GObject *content, GParamSpec *property, FillDialog *dialog) {
	gtk_widget_set_visible(dialog->color, gtk_drop_down_get_selected(GTK_DROP_DOWN(content)) == FILL_COLOR);
}

static gboolean accept_fill(CommandDialog *base) {
	FillDialog *dialog = (FillDialog *)base;
	Editor *editor = dialog->base.editor;
	gtk_spin_button_update(GTK_SPIN_BUTTON(dialog->opacity));
	fill_content = gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->content));
	fill_opacity = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(dialog->opacity));
	fill_preserve = gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->preserve));
	fill_custom_color = read_color_button(dialog->color);
	guint32 color = get_fill_content_color(fill_content, read_color_button(editor->foreground),
										   read_color_button(editor->background), fill_custom_color);
	g_autoptr(GError) error = NULL;
	gboolean filled = run_fill(editor, color, fill_opacity, fill_preserve, &error);
	show_dialog_message(&dialog->base, error);
	return filled;
}

static GtkWidget *create_color_input(guint32 color) {
	g_autoptr(GtkColorDialog) chooser = gtk_color_dialog_new();
	gtk_color_dialog_set_with_alpha(chooser, FALSE);
	GtkWidget *button = gtk_color_dialog_button_new(g_object_ref(chooser));
	write_color_button(button, color);
	return button;
}

void show_fill_dialog(Editor *editor) {
	if (!get_command_document(editor))
		return;
	FillDialog *dialog = (FillDialog *)create_command_dialog(editor, TEXT_FILL_TITLE, sizeof(FillDialog));
	dialog->base.accept = accept_fill;
	dialog->content =
		gtk_drop_down_new_from_strings((const char *[]){TEXT_FILL_FOREGROUND, TEXT_FILL_BACKGROUND, TEXT_FILL_CUSTOM,
														TEXT_FILL_BLACK, TEXT_FILL_GRAY, TEXT_FILL_WHITE, NULL});
	gtk_drop_down_set_selected(GTK_DROP_DOWN(dialog->content), fill_content);
	add_dialog_input(dialog->base.grid, TEXT_FILL_TITLE, dialog->content, 0);
	dialog->color = create_color_input(fill_custom_color);
	gtk_accessible_update_property(GTK_ACCESSIBLE(dialog->color), GTK_ACCESSIBLE_PROPERTY_LABEL, TEXT_FILL_CUSTOM, -1);
	gtk_grid_attach(GTK_GRID(dialog->base.grid), dialog->color, 2, 0, 1, 1);
	g_signal_connect(dialog->content, "notify::selected", G_CALLBACK(change_fill_content), dialog);
	change_fill_content(G_OBJECT(dialog->content), NULL, dialog);
	dialog->opacity = add_number_input(&dialog->base, TEXT_DIALOG_OPACITY, TEXT_PERCENT_UNIT, 0, 100, fill_opacity, 1);
	dialog->preserve = gtk_check_button_new_with_label(TEXT_PRESERVE_TRANSPARENCY);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->preserve), fill_preserve);
	gtk_grid_attach(GTK_GRID(dialog->base.grid), dialog->preserve, 0, 2, 3, 1);
	gtk_window_present(GTK_WINDOW(dialog->base.window));
}

/* ============================== Stroke ============================== */

static gboolean accept_stroke(CommandDialog *base) {
	FillDialog *dialog = (FillDialog *)base;
	Editor *editor = dialog->base.editor;
	gtk_spin_button_update(GTK_SPIN_BUTTON(dialog->amount));
	gtk_spin_button_update(GTK_SPIN_BUTTON(dialog->opacity));
	stroke_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(dialog->amount));
	stroke_location = gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->position));
	stroke_opacity = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(dialog->opacity));
	Document *document = get_command_document(editor);
	if (!document)
		return TRUE;
	g_autoptr(GError) error = NULL;
	gboolean stroked = stroke_document(document, read_color_button(dialog->color), stroke_width, stroke_location,
									   stroke_opacity, &error);
	show_dialog_message(&dialog->base, error);
	if (stroked)
		refresh_command_page(editor, TRUE);
	return stroked;
}

void show_stroke_dialog(Editor *editor) {
	if (!get_command_document(editor))
		return;
	FillDialog *dialog = (FillDialog *)create_command_dialog(editor, TEXT_STROKE_TITLE, sizeof(FillDialog));
	dialog->base.accept = accept_stroke;
	dialog->amount = add_number_input(&dialog->base, TEXT_STROKE_SIZE, TEXT_PIXEL_UNIT, 1, 200, stroke_width, 0);
	dialog->position = gtk_drop_down_new_from_strings(
		(const char *[]){TEXT_STROKE_INSIDE, TEXT_STROKE_CENTER, TEXT_STROKE_OUTSIDE, NULL});
	gtk_drop_down_set_selected(GTK_DROP_DOWN(dialog->position), stroke_location);
	add_dialog_input(dialog->base.grid, TEXT_STROKE_POSITION, dialog->position, 1);
	dialog->opacity =
		add_number_input(&dialog->base, TEXT_DIALOG_OPACITY, TEXT_PERCENT_UNIT, 0, 100, stroke_opacity, 2);
	dialog->color = create_color_input(read_color_button(editor->foreground));
	add_dialog_input(dialog->base.grid, TEXT_COLOR, dialog->color, 3);
	gtk_window_present(GTK_WINDOW(dialog->base.window));
}
