#include "editor.h"
#include "../lang/en.h"

/* Creation inputs are an explicit source command request without profile or artboard fields */
typedef struct {
	Editor *editor;
	GtkWidget *window, *name, *width, *height, *mode, *depth, *message;
} NewDocumentDialog;

static void finish_file_dialog(GObject *source, GAsyncResult *result, gpointer data) {
	GtkWindow *window = data;
	Editor *editor = g_object_get_data(G_OBJECT(window), "editor");
	g_autoptr(GError) error = NULL;
	g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, &error);
	if (!editor->closing && files) {
		for (guint index = 0; index < g_list_model_get_n_items(files); index++) {
			g_autoptr(GFile) file = g_list_model_get_item(files, index);
			open_editor_file(editor, file);
		}
	}
	if (!editor->closing && error && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
	g_object_unref(window);
}

void show_open_dialog(Editor *editor) {
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, TEXT_OPEN_IMAGES);
	g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_autoptr(GtkFileFilter) supported = gtk_file_filter_new();
	gtk_file_filter_set_name(supported, TEXT_SUPPORTED_FILTER);
	/* Photoshop files and swatch libraries use the translated source readers rather than a raster decoder */
	const char *source_patterns[] = {"*.psd", "*.PSD", "*.psb", "*.PSB", "*.aco", "*.ACO", "*.ase", "*.ASE"};
	for (guint index = 0; index < G_N_ELEMENTS(source_patterns); index++)
		gtk_file_filter_add_pattern(supported, source_patterns[index]);
	g_autoptr(GSList) formats = gdk_pixbuf_get_formats();
	for (GSList *item = formats; item; item = item->next) {
		g_auto(GStrv) extensions = gdk_pixbuf_format_get_extensions(item->data);
		for (guint index = 0; extensions[index]; index++) {
			if (g_str_equal(extensions[index], "svg") || g_str_equal(extensions[index], "svgz") ||
				g_str_equal(extensions[index], "psd") || g_str_equal(extensions[index], "psb") ||
				g_str_equal(extensions[index], "pdf"))
				continue;
			g_autofree char *pattern = g_strconcat("*.", extensions[index], NULL);
			g_autofree char *uppercase = g_ascii_strup(pattern, -1);
			gtk_file_filter_add_pattern(supported, pattern);
			gtk_file_filter_add_pattern(supported, uppercase);
		}
	}
	g_list_store_append(filters, supported);
	g_autoptr(GtkFileFilter) all = gtk_file_filter_new();
	gtk_file_filter_set_name(all, TEXT_ALL_FILTER);
	gtk_file_filter_add_pattern(all, "*");
	g_list_store_append(filters, all);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	gtk_file_dialog_set_default_filter(dialog, supported);
	gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(editor->window), NULL, finish_file_dialog,
								  g_object_ref(editor->window));
}

static void create_requested_document(GtkButton *button, NewDocumentDialog *dialog) {
	const unsigned depths[] = {8, 16, 32};
	create_editor_document(dialog->editor, gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(dialog->width)),
						   gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(dialog->height)),
						   depths[gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->depth))],
						   gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->mode)),
						   gtk_editable_get_text(GTK_EDITABLE(dialog->name)));
	gtk_window_destroy(GTK_WINDOW(dialog->window));
}

void add_dialog_input(GtkWidget *grid, const char *name, GtkWidget *input, int row) {
	GtkWidget *label = gtk_label_new(name);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), input, 1, row, 1, 1);
	gtk_accessible_update_property(GTK_ACCESSIBLE(input), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
}

static void change_document_mode(GObject *mode, GParamSpec *property, NewDocumentDialog *dialog) {
	gboolean illustration = gtk_drop_down_get_selected(GTK_DROP_DOWN(mode)) == DOCUMENT_VECTOR;
	/* lg starts image documents at 1280 by 720 and illustration documents at 8.3 by 11.7 inches */
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->width), illustration ? 598 : 1280);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->height), illustration ? 842 : 720);
}

static gboolean close_new_with_escape(GtkEventControllerKey *controller, guint key, guint code,
									  GdkModifierType modifiers, NewDocumentDialog *dialog) {
	if (key != GDK_KEY_Escape)
		return FALSE;
	gtk_window_destroy(GTK_WINDOW(dialog->window));
	return TRUE;
}

void show_new_dialog(Editor *editor) {
	NewDocumentDialog *dialog = g_new0(NewDocumentDialog, 1);
	dialog->editor = editor;
	dialog->window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(dialog->window), TEXT_NEW_TRANSPARENT);
	gtk_window_set_transient_for(GTK_WINDOW(dialog->window), GTK_WINDOW(editor->window));
	gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog->window), TRUE);
	gtk_window_set_modal(GTK_WINDOW(dialog->window), TRUE);
	g_object_set_data_full(G_OBJECT(dialog->window), "new-document", dialog, g_free);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_start(box, 16);
	gtk_widget_set_margin_end(box, 16);
	gtk_widget_set_margin_top(box, 16);
	gtk_widget_set_margin_bottom(box, 16);
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	dialog->name = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(dialog->name), TEXT_NEW_PROJECT);
	dialog->width = gtk_spin_button_new_with_range(1, 32768, 1);
	dialog->height = gtk_spin_button_new_with_range(1, 32768, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->width), 1280);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->height), 720);
	dialog->mode = gtk_drop_down_new_from_strings((const char *[]){TEXT_IMAGE_MODE, TEXT_ILLUSTRATION_MODE, NULL});
	dialog->depth = gtk_drop_down_new_from_strings((const char *[]){TEXT_DEPTH_8, TEXT_DEPTH_16, TEXT_DEPTH_32, NULL});
	g_signal_connect(dialog->mode, "notify::selected", G_CALLBACK(change_document_mode), dialog);
	add_dialog_input(grid, TEXT_NAME, dialog->name, 0);
	add_dialog_input(grid, TEXT_WIDTH, dialog->width, 1);
	add_dialog_input(grid, TEXT_HEIGHT, dialog->height, 2);
	add_dialog_input(grid, TEXT_DOCUMENT_MODE, dialog->mode, 3);
	add_dialog_input(grid, TEXT_CHANNEL_DEPTH, dialog->depth, 4);
	gtk_box_append(GTK_BOX(box), grid);
	gtk_box_append(GTK_BOX(box), gtk_label_new(TEXT_NEW_COLOR_SETTINGS));
	dialog->message = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(dialog->message), TRUE);
	gtk_box_append(GTK_BOX(box), dialog->message);
	GtkWidget *create = gtk_button_new_with_label(TEXT_CREATE);
	g_signal_connect(create, "clicked", G_CALLBACK(create_requested_document), dialog);
	gtk_box_append(GTK_BOX(box), create);
	gtk_window_set_child(GTK_WINDOW(dialog->window), box);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(close_new_with_escape), dialog);
	gtk_widget_add_controller(dialog->window, keys);
	gtk_window_present(GTK_WINDOW(dialog->window));
}
