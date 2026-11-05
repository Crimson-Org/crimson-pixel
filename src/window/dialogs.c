#include <adwaita.h>

#include "editor.h"
#include "lang/en.h"
#include "common/widgets/spacing.h"

/* The New dialog is as wide as a short preferences page */
#define NEW_DIALOG_WIDTH 420
/* Image documents start at 1280 by 720 and illustration documents at 8.3 by 11.7 inches */
#define IMAGE_WIDTH 1280
#define IMAGE_HEIGHT 720
#define ILLUSTRATION_WIDTH 598
#define ILLUSTRATION_HEIGHT 842

/* Creation inputs are an explicit source command request without profile or artboard fields */
typedef struct {
	Editor *editor;
	GtkWidget *window, *name, *width, *height, *mode, *depth;
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
	/* Photoshop files and swatch libraries use their own readers rather than a raster decoder */
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
	adw_spin_row_update(ADW_SPIN_ROW(dialog->width));
	adw_spin_row_update(ADW_SPIN_ROW(dialog->height));
	create_editor_document(dialog->editor, (int)adw_spin_row_get_value(ADW_SPIN_ROW(dialog->width)),
						   (int)adw_spin_row_get_value(ADW_SPIN_ROW(dialog->height)),
						   depths[adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->depth))],
						   adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->mode)),
						   gtk_editable_get_text(GTK_EDITABLE(dialog->name)));
	adw_dialog_close(ADW_DIALOG(dialog->window));
}

static void cancel_new_document(GtkButton *button, NewDocumentDialog *dialog) {
	adw_dialog_close(ADW_DIALOG(dialog->window));
}

/* The grid keeps how many rows it has handed out */
int take_grid_row(GtkWidget *grid) {
	int row = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(grid), "next-row"));
	g_object_set_data(G_OBJECT(grid), "next-row", GINT_TO_POINTER(row + 1));
	return row;
}

int get_grid_row(GtkWidget *grid) {
	return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(grid), "next-row")) - 1;
}

void add_dialog_input(GtkWidget *grid, const char *name, GtkWidget *input) {
	int row = take_grid_row(grid);
	GtkWidget *label = gtk_label_new(name);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_grid_attach(GTK_GRID(grid), label, LABEL_COLUMN, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), input, INPUT_COLUMN, row, 1, 1);
	gtk_accessible_update_property(GTK_ACCESSIBLE(input), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
}

void add_grid_row(GtkWidget *grid, GtkWidget *widget, int column) {
	gtk_grid_attach(GTK_GRID(grid), widget, column, take_grid_row(grid), GRID_COLUMNS - column, 1);
}

static void change_document_mode(GObject *mode, GParamSpec *property, NewDocumentDialog *dialog) {
	gboolean illustration = adw_combo_row_get_selected(ADW_COMBO_ROW(mode)) == DOCUMENT_VECTOR;
	adw_spin_row_set_value(ADW_SPIN_ROW(dialog->width), illustration ? ILLUSTRATION_WIDTH : IMAGE_WIDTH);
	adw_spin_row_set_value(ADW_SPIN_ROW(dialog->height), illustration ? ILLUSTRATION_HEIGHT : IMAGE_HEIGHT);
}

static GtkWidget *create_choice_row(const char *title, const char *const *choices) {
	GtkWidget *row = adw_combo_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	g_autoptr(GtkStringList) model = gtk_string_list_new(choices);
	adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
	return row;
}

/* Sizes are in pixels, with the unit after the field as in the other size dialogs */
static GtkWidget *create_size_row(const char *title, int value) {
	GtkWidget *row = adw_spin_row_new_with_range(1, DOCUMENT_LARGEST_SIDE, 1);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_spin_row_set_value(ADW_SPIN_ROW(row), value);
	GtkWidget *unit = gtk_label_new(TEXT_PIXEL_UNIT);
	gtk_widget_add_css_class(unit, "dim-label");
	adw_action_row_add_suffix(ADW_ACTION_ROW(row), unit);
	return row;
}

static void create_group(GtkWidget *page, GtkWidget *const *rows, guint count) {
	GtkWidget *group = adw_preferences_group_new();
	for (guint index = 0; index < count; index++)
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), rows[index]);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
}

/* The name, the size and the kind of document as rows, with Cancel and Create in the header */
void show_new_dialog(Editor *editor) {
	NewDocumentDialog *dialog = g_new0(NewDocumentDialog, 1);
	dialog->editor = editor;
	dialog->window = GTK_WIDGET(adw_dialog_new());
	adw_dialog_set_title(ADW_DIALOG(dialog->window), TEXT_NEW_PROJECT);
	adw_dialog_set_content_width(ADW_DIALOG(dialog->window), NEW_DIALOG_WIDTH);
	g_object_set_data_full(G_OBJECT(dialog->window), "new-document", dialog, g_free);
	GtkWidget *header = adw_header_bar_new();
	adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
	adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
	GtkWidget *cancel = gtk_button_new_with_label(TEXT_CANCEL), *create = gtk_button_new_with_label(TEXT_CREATE);
	gtk_widget_add_css_class(create, "suggested-action");
	g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_new_document), dialog);
	g_signal_connect(create, "clicked", G_CALLBACK(create_requested_document), dialog);
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), create);
	/* The colour settings every new document gets sit quietly under the title */
	GtkWidget *title = adw_window_title_new(TEXT_NEW_PROJECT, TEXT_NEW_COLOR_SETTINGS);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
	dialog->name = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->name), TEXT_NAME);
	gtk_editable_set_text(GTK_EDITABLE(dialog->name), TEXT_UNTITLED);
	dialog->width = create_size_row(TEXT_WIDTH, IMAGE_WIDTH);
	dialog->height = create_size_row(TEXT_HEIGHT, IMAGE_HEIGHT);
	dialog->mode = create_choice_row(TEXT_DOCUMENT_MODE, (const char *[]){TEXT_IMAGE_MODE, TEXT_ILLUSTRATION_MODE, NULL});
	dialog->depth =
		create_choice_row(TEXT_CHANNEL_DEPTH, (const char *[]){TEXT_DEPTH_8, TEXT_DEPTH_16, TEXT_DEPTH_32, NULL});
	g_signal_connect(dialog->mode, "notify::selected", G_CALLBACK(change_document_mode), dialog);
	GtkWidget *page = adw_preferences_page_new();
	GtkWidget *name_rows[] = {dialog->name}, *size_rows[] = {dialog->width, dialog->height};
	GtkWidget *kind_rows[] = {dialog->mode, dialog->depth};
	create_group(page, name_rows, G_N_ELEMENTS(name_rows));
	create_group(page, size_rows, G_N_ELEMENTS(size_rows));
	create_group(page, kind_rows, G_N_ELEMENTS(kind_rows));
	GtkWidget *view = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page);
	adw_dialog_set_child(ADW_DIALOG(dialog->window), view);
	adw_dialog_set_default_widget(ADW_DIALOG(dialog->window), create);
	adw_dialog_present(ADW_DIALOG(dialog->window), editor->window);
	/* The name takes the keyboard, so typing renames the new document at once */
	gtk_widget_grab_focus(dialog->name);
}
