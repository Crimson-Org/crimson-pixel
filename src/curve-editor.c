#include <math.h>
#include <string.h>

#include "adjustment-kernels.h"
#include "curve-editor.h"
#include "snapshot-area.h"

/* The graph keeps a square of this size, with room for the point handles at its edges */
enum { CURVE_EDITOR_SIZE = 256, CURVE_EDITOR_MARGIN = 6, CURVE_POINT_SIZE = 7, CURVE_REMOVE_DISTANCE = 24 };

/* How close in levels a press must be to grab a point instead of adding one */
#define CURVE_GRAB_DISTANCE 10.0

/* The state behind one graph */
typedef struct {
	GArray *points;
	guint32 counts[256];
	gboolean has_histogram;
	int dragged;
	CurveEditorChanged changed;
	gpointer data;
} CurveEditor;

/* ============================== Geometry ======================================== */

static CurveEditor *get_editor(GtkWidget *widget) {
	return g_object_get_data(G_OBJECT(widget), "curve-editor");
}

static double get_graph_size(GtkWidget *widget) {
	return MAX(1, MIN(gtk_widget_get_width(widget), gtk_widget_get_height(widget)) - 2 * CURVE_EDITOR_MARGIN);
}

static void get_level(GtkWidget *widget, double x, double y, double *input, double *output) {
	double size = get_graph_size(widget);
	*input = (x - CURVE_EDITOR_MARGIN) / size * 255;
	*output = 255 - (y - CURVE_EDITOR_MARGIN) / size * 255;
}

static graphene_point_t get_position(GtkWidget *widget, double input, double output) {
	double size = get_graph_size(widget);
	return GRAPHENE_POINT_INIT(CURVE_EDITOR_MARGIN + input / 255 * size,
							   CURVE_EDITOR_MARGIN + (255 - output) / 255 * size);
}

/* ============================== Drawing ========================================= */

static void draw_curve_editor(GtkWidget *widget, GtkSnapshot *snapshot, int width, int height, gpointer data) {
	CurveEditor *editor = data;
	GdkRGBA color, faint, line;
	gtk_widget_get_color(widget, &color);
	faint = line = color;
	faint.alpha *= .25;
	line.alpha *= .12;
	double size = get_graph_size(widget);
	graphene_rect_t frame = GRAPHENE_RECT_INIT(CURVE_EDITOR_MARGIN, CURVE_EDITOR_MARGIN, size, size);
	if (editor->has_histogram) {
		guint32 peak = 1;
		for (int level = 0; level < 256; level++)
			peak = MAX(peak, editor->counts[level]);
		for (int level = 0; level < 256; level++) {
			/* A square root scale keeps small counts visible next to large peaks */
			double bar = round(sqrt((double)editor->counts[level] / peak) * size);
			gtk_snapshot_append_color(snapshot, &faint,
									  &GRAPHENE_RECT_INIT(CURVE_EDITOR_MARGIN + level * size / 256,
														  CURVE_EDITOR_MARGIN + size - bar, ceil(size / 256), bar));
		}
	}
	for (int quarter = 1; quarter < 4; quarter++) {
		double offset = round(quarter * size / 4);
		double start = CURVE_EDITOR_MARGIN, across = CURVE_EDITOR_MARGIN + offset;
		gtk_snapshot_append_color(snapshot, &line, &GRAPHENE_RECT_INIT(across, start, 1, size));
		gtk_snapshot_append_color(snapshot, &line, &GRAPHENE_RECT_INIT(start, across, size, 1));
	}
	GskRoundedRect border;
	gsk_rounded_rect_init_from_rect(&border, &frame, 0);
	gtk_snapshot_append_border(snapshot, &border, (const float[]){1, 1, 1, 1},
							   (const GdkRGBA[]){faint, faint, faint, faint});
	g_autoptr(GskPathBuilder) diagonal = gsk_path_builder_new();
	gsk_path_builder_move_to(diagonal, CURVE_EDITOR_MARGIN, CURVE_EDITOR_MARGIN + size);
	gsk_path_builder_line_to(diagonal, CURVE_EDITOR_MARGIN + size, CURVE_EDITOR_MARGIN);
	g_autoptr(GskPath) baseline = gsk_path_builder_free_to_path(g_steal_pointer(&diagonal));
	g_autoptr(GskStroke) thin = gsk_stroke_new(1);
	gtk_snapshot_append_stroke(snapshot, baseline, thin, &faint);
	if (editor->points->len < 2)
		return;
	float curve[CURVE_EDITOR_SIZE];
	sample_spline((const CurvePoint *)editor->points->data, editor->points->len, curve, CURVE_EDITOR_SIZE);
	g_autoptr(GskPathBuilder) builder = gsk_path_builder_new();
	for (int index = 0; index < CURVE_EDITOR_SIZE; index++) {
		double value = isnan(curve[index]) ? 0 : curve[index];
		graphene_point_t point = get_position(widget, index * 255.0 / (CURVE_EDITOR_SIZE - 1), value * 255);
		if (index)
			gsk_path_builder_line_to(builder, point.x, point.y);
		else
			gsk_path_builder_move_to(builder, point.x, point.y);
	}
	g_autoptr(GskPath) path = gsk_path_builder_free_to_path(g_steal_pointer(&builder));
	g_autoptr(GskStroke) stroke = gsk_stroke_new(2);
	gtk_snapshot_push_clip(snapshot, &frame);
	gtk_snapshot_append_stroke(snapshot, path, stroke, &color);
	gtk_snapshot_pop(snapshot);
	for (guint index = 0; index < editor->points->len; index++) {
		const CurvePoint *point = &g_array_index(editor->points, CurvePoint, index);
		graphene_point_t center = get_position(widget, point->input, point->output);
		graphene_rect_t handle = GRAPHENE_RECT_INIT(
			center.x - CURVE_POINT_SIZE / 2.0, center.y - CURVE_POINT_SIZE / 2.0, CURVE_POINT_SIZE, CURVE_POINT_SIZE);
		/* The dragged point is filled and the others are outlined */
		if ((int)index == editor->dragged)
			gtk_snapshot_append_color(snapshot, &color, &handle);
		else {
			GskRoundedRect outline;
			gsk_rounded_rect_init_from_rect(&outline, &handle, 0);
			gtk_snapshot_append_border(snapshot, &outline, (const float[]){1.5, 1.5, 1.5, 1.5},
									   (const GdkRGBA[]){color, color, color, color});
		}
	}
}

/* ============================== Dragging ======================================== */

static void report_change(GtkWidget *widget, CurveEditor *editor) {
	gtk_widget_queue_draw(widget);
	if (editor->changed)
		editor->changed(widget, editor->data);
}

/* Keeps the point between its neighbours, since the spline needs rising inputs */
static void place_point(CurveEditor *editor, int index, double input, double output) {
	double low = index > 0 ? g_array_index(editor->points, CurvePoint, index - 1).input + 1 : 0;
	double high =
		index + 1 < (int)editor->points->len ? g_array_index(editor->points, CurvePoint, index + 1).input - 1 : 255;
	CurvePoint *point = &g_array_index(editor->points, CurvePoint, index);
	point->input = CLAMP(round(input), low, MAX(low, high));
	point->output = CLAMP(round(output), 0, 255);
}

static void begin_drag(GtkGestureDrag *gesture, double x, double y, GtkWidget *widget) {
	CurveEditor *editor = get_editor(widget);
	double input, output, nearest = G_MAXDOUBLE;
	get_level(widget, x, y, &input, &output);
	editor->dragged = -1;
	for (guint index = 0; index < editor->points->len; index++) {
		const CurvePoint *point = &g_array_index(editor->points, CurvePoint, index);
		double distance = hypot(point->input - input, point->output - output);
		if (distance < nearest && distance <= CURVE_GRAB_DISTANCE) {
			nearest = distance;
			editor->dragged = index;
		}
	}
	if (editor->dragged < 0) {
		guint position = 0;
		while (position < editor->points->len && g_array_index(editor->points, CurvePoint, position).input < input)
			position++;
		/* Two points on one input would leave the spline without a slope */
		if (position < editor->points->len && g_array_index(editor->points, CurvePoint, position).input == round(input))
			return;
		CurvePoint point = {round(input), round(output), TRUE};
		g_array_insert_val(editor->points, position, point);
		editor->dragged = position;
	}
	place_point(editor, editor->dragged, input, output);
	report_change(widget, editor);
}

static void update_drag(GtkGestureDrag *gesture, double x, double y, GtkWidget *widget) {
	CurveEditor *editor = get_editor(widget);
	if (editor->dragged < 0)
		return;
	double start_x, start_y, input, output;
	gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
	x += start_x;
	y += start_y;
	double size = get_graph_size(widget);
	double low = CURVE_EDITOR_MARGIN - CURVE_REMOVE_DISTANCE, high = CURVE_EDITOR_MARGIN + size + CURVE_REMOVE_DISTANCE;
	gboolean outside = x < low || y < low || x > high || y > high;
	if (outside && editor->points->len > 2) {
		g_array_remove_index(editor->points, editor->dragged);
		editor->dragged = -1;
		report_change(widget, editor);
		return;
	}
	get_level(widget, x, y, &input, &output);
	place_point(editor, editor->dragged, input, output);
	report_change(widget, editor);
}

static void end_drag(GtkGestureDrag *gesture, double x, double y, GtkWidget *widget) {
	CurveEditor *editor = get_editor(widget);
	editor->dragged = -1;
	gtk_widget_queue_draw(widget);
}

/* ============================== Editor ========================================== */

static void free_editor(CurveEditor *editor) {
	g_array_unref(editor->points);
	g_free(editor);
}

GtkWidget *create_curve_editor(CurveEditorChanged changed, gpointer data) {
	CurveEditor *editor = g_new0(CurveEditor, 1);
	editor->points = g_array_new(FALSE, FALSE, sizeof(CurvePoint));
	editor->dragged = -1;
	editor->changed = changed;
	editor->data = data;
	GtkWidget *widget = create_snapshot_area(draw_curve_editor, NULL, editor);
	g_object_set_data_full(G_OBJECT(widget), "curve-editor", editor, (GDestroyNotify)free_editor);
	const int side = CURVE_EDITOR_SIZE + 2 * CURVE_EDITOR_MARGIN;
	gtk_widget_set_size_request(widget, side, side);
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(begin_drag), widget);
	g_signal_connect(drag, "drag-update", G_CALLBACK(update_drag), widget);
	g_signal_connect(drag, "drag-end", G_CALLBACK(end_drag), widget);
	gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(drag));
	return widget;
}

void set_curve_editor_points(GtkWidget *widget, const CurvePoint *points, guint count) {
	CurveEditor *editor = get_editor(widget);
	g_array_set_size(editor->points, 0);
	g_array_append_vals(editor->points, points, count);
	editor->dragged = -1;
	gtk_widget_queue_draw(widget);
}

void set_curve_editor_histogram(GtkWidget *widget, const guint32 *counts) {
	CurveEditor *editor = get_editor(widget);
	editor->has_histogram = counts != NULL;
	if (counts)
		memcpy(editor->counts, counts, sizeof editor->counts);
	gtk_widget_queue_draw(widget);
}

const GArray *get_curve_editor_points(GtkWidget *widget) {
	return get_editor(widget)->points;
}
