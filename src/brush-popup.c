#include <math.h>

#include "editor.h"
#include "../lang/en.h"


enum { SIZE_STEPS = 1000, SIZE_LIMIT = 2500 };

/* The size slider is logarithmic so small brushes, which pixel art uses most, get most of its length */
static double get_slider_size(double position) {
	return round(pow(SIZE_LIMIT, position / SIZE_STEPS));
}

static double get_size_position(double size) {
	return SIZE_STEPS * log(MAX(1, size)) / log(SIZE_LIMIT);
}

static char *format_size(GtkScale *scale, double position, gpointer data) {
	return g_strdup_printf(TEXT_BRUSH_SIZE_VALUE, get_slider_size(position));
}

static void change_size(GtkRange *range, Editor *editor) {
	PaintOptions *options = get_tool_options(editor);
	set_brush_shape(editor, get_slider_size(gtk_range_get_value(range)), options->shape.hardness);
	queue_canvas_redraw(editor);
}

static void change_hardness(GtkRange *range, Editor *editor) {
	set_brush_shape(editor, get_tool_options(editor)->shape.diameter, gtk_range_get_value(range));
	queue_canvas_redraw(editor);
}

static void release_brush_outline(GtkPopover *popover, DocumentView *view) {
	view->brush_anchored = FALSE;
	gtk_widget_queue_draw(view->canvas);
	gtk_widget_unparent(GTK_WIDGET(popover));
}

static GtkWidget *add_slider_row(GtkWidget *grid, const char *name, GtkWidget *scale, int row) {
	GtkWidget *label = gtk_label_new_with_mnemonic(name);
	gtk_label_set_mnemonic_widget(GTK_LABEL(label), scale);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
	gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
	gtk_widget_set_size_request(scale, 260, -1);
	gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), scale, 1, row, 1, 1);
	return scale;
}

void show_brush_popup(DocumentView *view, double x, double y) {
	Editor *editor = view->editor;
	GtkWidget *canvas = view->canvas;
	PaintOptions *options = get_tool_options(editor);
	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
	GtkWidget *size = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, SIZE_STEPS, 1);
	gtk_range_set_value(GTK_RANGE(size), get_size_position(options->shape.diameter));
	gtk_scale_set_format_value_func(GTK_SCALE(size), format_size, NULL, NULL);
	add_slider_row(grid, TEXT_OPTION_SIZE, size, 0);
	g_signal_connect(size, "value-changed", G_CALLBACK(change_size), editor);
	if (uses_soft_tip(editor)) {
		GtkWidget *hardness = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
		gtk_range_set_value(GTK_RANGE(hardness), options->shape.hardness);
		add_slider_row(grid, TEXT_OPTION_HARDNESS, hardness, 1);
		g_signal_connect(hardness, "value-changed", G_CALLBACK(change_hardness), editor);
	}
	GtkWidget *popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(popover), grid);
	gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
	gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_RIGHT);
	gtk_widget_set_parent(popover, canvas);
	int radius = (int)ceil(MAX(options->shape.diameter * view->zoom / 2, 2)) + 4;
	gtk_popover_set_pointing_to(GTK_POPOVER(popover),
								&(GdkRectangle){(int)x - radius, (int)y - radius, radius * 2, radius * 2});
	g_signal_connect(popover, "closed", G_CALLBACK(release_brush_outline), view);
	view->brush_anchored = TRUE;
	view->anchor_x = x;
	view->anchor_y = y;
	gtk_widget_queue_draw(canvas);
	gtk_popover_popup(GTK_POPOVER(popover));
}
