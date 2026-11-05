#include <adwaita.h>

#include "command-dialog.h"
#include "lang/en.h"
#include "common/widgets/spacing.h"

void refresh_command_page(Editor *editor, gboolean render) {
	GtkWidget *page = get_current_page(editor);
	if (!page)
		return;
	refresh_selection_display(get_page_view(page, "primary"));
	refresh_selection_display(get_page_view(page, "secondary"));
	refresh_document_page(editor, page, render);
}

Document *get_command_document(Editor *editor) {
	PresentedDocument *presented = get_page_document(get_current_page(editor));
	return presented ? presented->document : NULL;
}

/* Closing by Cancel, Escape or the window undoes what the dialog previewed, while OK keeps the result */
static gboolean close_command_dialog(GtkWindow *window, CommandDialog *dialog) {
	if (!dialog->accepted && dialog->cancel)
		dialog->cancel(dialog);
	return FALSE;
}

static void press_cancel(GtkButton *button, CommandDialog *dialog) {
	gtk_window_close(GTK_WINDOW(dialog->window));
}

void press_ok(GtkButton *button, CommandDialog *dialog) {
	if (!dialog->accept(dialog))
		return;
	dialog->accepted = TRUE;
	gtk_window_close(GTK_WINDOW(dialog->window));
}

static gboolean press_dialog_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType modifiers,
								 CommandDialog *dialog) {
	if (key != GDK_KEY_Escape)
		return FALSE;
	gtk_window_close(GTK_WINDOW(dialog->window));
	return TRUE;
}

/* A header with Cancel and OK above a grid of labelled inputs, the way libadwaita dialogs ask for values
 * Each sits in a small window of its own rather than over the workspace, since some preview their result on the
 * canvas, which must stay in full view and can be uncovered by moving the window aside */
CommandDialog *create_command_dialog(Editor *editor, const char *title, gsize size) {
	CommandDialog *dialog = g_malloc0(MAX(size, sizeof(CommandDialog)));
	dialog->editor = editor;
	dialog->window = adw_window_new();
	gtk_window_set_title(GTK_WINDOW(dialog->window), title);
	gtk_window_set_transient_for(GTK_WINDOW(dialog->window), GTK_WINDOW(editor->window));
	gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog->window), TRUE);
	gtk_window_set_modal(GTK_WINDOW(dialog->window), TRUE);
	gtk_window_set_resizable(GTK_WINDOW(dialog->window), FALSE);
	/* The window is as big as its inputs, without the smallest size libadwaita gives a main window */
	gtk_widget_set_size_request(dialog->window, -1, -1);
	g_object_set_data_full(G_OBJECT(dialog->window), "command-dialog", dialog, g_free);
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
	adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
	GtkWidget *cancel = gtk_button_new_with_label(TEXT_CANCEL), *ok = gtk_button_new_with_label(TEXT_OK);
	gtk_widget_add_css_class(ok, "suggested-action");
	g_signal_connect(cancel, "clicked", G_CALLBACK(press_cancel), dialog);
	g_signal_connect(ok, "clicked", G_CALLBACK(press_ok), dialog);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), ok);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_widget_add_css_class(box, "command-body");
	/* The inputs sit in the middle when the header is the wider part, so both sides keep the same room */
	gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
	dialog->grid = gtk_grid_new();
	gtk_widget_add_css_class(dialog->grid, "form-grid");
	gtk_grid_set_row_spacing(GTK_GRID(dialog->grid), SPACING_NORMAL);
	gtk_grid_set_column_spacing(GTK_GRID(dialog->grid), SPACING_NORMAL);
	gtk_box_append(GTK_BOX(box), dialog->grid);
	dialog->message = gtk_label_new(NULL);
	gtk_widget_add_css_class(dialog->message, "error");
	gtk_label_set_wrap(GTK_LABEL(dialog->message), TRUE);
	gtk_label_set_xalign(GTK_LABEL(dialog->message), 0);
	gtk_widget_set_visible(dialog->message, FALSE);
	gtk_box_append(GTK_BOX(box), dialog->message);
	GtkWidget *view = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), box);
	adw_window_set_content(ADW_WINDOW(dialog->window), view);
	gtk_window_set_default_widget(GTK_WINDOW(dialog->window), ok);
	g_signal_connect(dialog->window, "close-request", G_CALLBACK(close_command_dialog), dialog);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(press_dialog_key), dialog);
	gtk_widget_add_controller(dialog->window, keys);
	return dialog;
}

/* The first input takes the keyboard once the inputs are on screen, so a number can be typed at once */
static void focus_first_input(GtkWidget *grid, gpointer data) {
	gtk_widget_child_focus(grid, GTK_DIR_TAB_FORWARD);
}

void present_command_dialog(CommandDialog *dialog) {
	g_signal_connect(dialog->grid, "map", G_CALLBACK(focus_first_input), NULL);
	gtk_window_present(GTK_WINDOW(dialog->window));
}

/* Enter in a number field takes its typed value and confirms the dialog at once */
static gboolean press_number_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType modifiers,
								 CommandDialog *dialog) {
	if (key != GDK_KEY_Return && key != GDK_KEY_KP_Enter && key != GDK_KEY_ISO_Enter)
		return FALSE;
	gtk_spin_button_update(GTK_SPIN_BUTTON(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller))));
	press_ok(NULL, dialog);
	return TRUE;
}

/* A number field followed by its unit */
GtkWidget *add_number_input(CommandDialog *dialog, const char *name, const char *unit, double minimum, double maximum,
							double value) {
	GtkWidget *input = gtk_spin_button_new_with_range(minimum, maximum, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(input), value);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(press_number_key), dialog);
	gtk_widget_add_controller(input, keys);
	add_dialog_input(dialog->grid, name, input);
	GtkWidget *unit_label = gtk_label_new(unit);
	gtk_widget_add_css_class(unit_label, "dim-label");
	gtk_label_set_xalign(GTK_LABEL(unit_label), 0);
	gtk_grid_attach(GTK_GRID(dialog->grid), unit_label, EXTRA_COLUMN, get_grid_row(dialog->grid), 1, 1);
	return input;
}

void show_dialog_message(CommandDialog *dialog, const GError *error) {
	gtk_label_set_text(GTK_LABEL(dialog->message), error ? error->message : "");
	gtk_widget_set_visible(dialog->message, error != NULL);
}
