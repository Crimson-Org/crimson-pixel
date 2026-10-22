#include "command-dialog.h"
#include "../lang/en.h"

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

static void cancel_dialog(CommandDialog *dialog) {
	if (dialog->cancel)
		dialog->cancel(dialog);
	gtk_window_destroy(GTK_WINDOW(dialog->window));
}

static void press_cancel(GtkButton *button, CommandDialog *dialog) {
	cancel_dialog(dialog);
}

void press_ok(GtkButton *button, CommandDialog *dialog) {
	if (dialog->accept(dialog))
		gtk_window_destroy(GTK_WINDOW(dialog->window));
}

static gboolean close_dialog_window(GtkWindow *window, CommandDialog *dialog) {
	if (dialog->cancel)
		dialog->cancel(dialog);
	return FALSE;
}

static gboolean press_dialog_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType modifiers,
								 CommandDialog *dialog) {
	if (key != GDK_KEY_Escape)
		return FALSE;
	cancel_dialog(dialog);
	return TRUE;
}

CommandDialog *create_command_dialog(Editor *editor, const char *title, gsize size) {
	CommandDialog *dialog = g_malloc0(MAX(size, sizeof(CommandDialog)));
	dialog->editor = editor;
	dialog->window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(dialog->window), title);
	gtk_window_set_transient_for(GTK_WINDOW(dialog->window), GTK_WINDOW(editor->window));
	gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog->window), TRUE);
	gtk_window_set_modal(GTK_WINDOW(dialog->window), TRUE);
	gtk_window_set_resizable(GTK_WINDOW(dialog->window), FALSE);
	g_object_set_data_full(G_OBJECT(dialog->window), "command-dialog", dialog, g_free);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_start(box, 16);
	gtk_widget_set_margin_end(box, 16);
	gtk_widget_set_margin_top(box, 16);
	gtk_widget_set_margin_bottom(box, 16);
	dialog->grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(dialog->grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(dialog->grid), 12);
	gtk_box_append(GTK_BOX(box), dialog->grid);
	dialog->message = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(dialog->message), TRUE);
	gtk_widget_set_visible(dialog->message, FALSE);
	gtk_box_append(GTK_BOX(box), dialog->message);
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	GtkWidget *cancel = gtk_button_new_with_label(TEXT_CANCEL), *ok = gtk_button_new_with_label(TEXT_OK);
	gtk_widget_add_css_class(ok, "suggested-action");
	g_signal_connect(cancel, "clicked", G_CALLBACK(press_cancel), dialog);
	g_signal_connect(ok, "clicked", G_CALLBACK(press_ok), dialog);
	gtk_box_append(GTK_BOX(buttons), cancel);
	gtk_box_append(GTK_BOX(buttons), ok);
	gtk_box_append(GTK_BOX(box), buttons);
	gtk_window_set_child(GTK_WINDOW(dialog->window), box);
	gtk_window_set_default_widget(GTK_WINDOW(dialog->window), ok);
	g_signal_connect(dialog->window, "close-request", G_CALLBACK(close_dialog_window), dialog);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(press_dialog_key), dialog);
	gtk_widget_add_controller(dialog->window, keys);
	return dialog;
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

/* A number field followed by its unitshow it */
GtkWidget *add_number_input(CommandDialog *dialog, const char *name, const char *unit, double minimum, double maximum,
							double value, int row) {
	GtkWidget *input = gtk_spin_button_new_with_range(minimum, maximum, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(input), value);
	GtkEventController *keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(press_number_key), dialog);
	gtk_widget_add_controller(input, keys);
	add_dialog_input(dialog->grid, name, input, row);
	gtk_grid_attach(GTK_GRID(dialog->grid), gtk_label_new(unit), 2, row, 1, 1);
	return input;
}

void show_dialog_message(CommandDialog *dialog, const GError *error) {
	gtk_label_set_text(GTK_LABEL(dialog->message), error ? error->message : "");
	gtk_widget_set_visible(dialog->message, error != NULL);
}
