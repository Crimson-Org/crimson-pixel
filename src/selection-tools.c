#include <math.h>

#include "editor.h"
#include "history.h"
#include "pixel-selection.h"

#define DOUBLE_CLICK_MILLISECONDS 250
#define CLOSE_DISTANCE 8

gboolean is_selection_path_tool(const char *identifier) {
	return !g_strcmp0(identifier, TOOL_LASSO) || !g_strcmp0(identifier, TOOL_POLYGONAL_LASSO);
}

SelectionMode get_selection_mode(gboolean shift, gboolean alt) {
	return shift && alt ? SELECTION_INTERSECT : shift ? SELECTION_ADD : alt ? SELECTION_SUBTRACT : SELECTION_REPLACE;
}

gboolean is_drawing_selection_path(const DocumentView *view) {
	return view->lasso_points != NULL;
}

static gboolean is_polygon_view(const DocumentView *view) {
	return !g_strcmp0(view->editor->tool, TOOL_POLYGONAL_LASSO);
}

static void add_path_point(GArray *points, double x, double y) {
	g_array_append_vals(points, (double[]){x, y}, 2);
}

static void clear_selection_path(DocumentView *view) {
	g_clear_pointer(&view->lasso_points, g_array_unref);
	clear_view_snap(view);
	gtk_widget_queue_draw(view->canvas);
}

static void finish_selection_path(DocumentView *view, gboolean empty) {
	Editor *editor = view->editor;
	Document *document = view->presented->document;
	SelectionMode mode = get_selection_mode(is_shift_held(&view->modifiers), is_alt_held(&view->modifiers));
	GArray *points = view->lasso_points;
	SelectionPath *path = create_polygon_path((const double *)points->data, points->len / 2);
	gboolean *anti_alias = get_selection_anti_alias(editor);
	gboolean changed = empty ? deselect(document, FALSE)
							 : select_path_area(document, path, mode, !anti_alias || *anti_alias, 10, 9, FALSE);
	free_selection_path(path);
	if (is_polygon_view(view)) {
		g_clear_pointer(&view->finished_polygon, g_array_unref);
		view->finished_polygon = g_array_copy(points);
		view->polygon_entry = changed ? get_latest_history_entry(document) : NULL;
	}
	clear_selection_path(view);
	refresh_selection_display(view);
	refresh_document_page(editor, get_current_page(editor), FALSE);
}

void start_selection_path(DocumentView *view) {
	view->selecting = view->path_button = TRUE;
	if (is_polygon_view(view))
		return;
	view->lasso_points = g_array_new(FALSE, FALSE, sizeof(double));
	add_path_point(view->lasso_points, view->select_x, view->select_y);
	refresh_selection_display(view);
}

static void move_polygon_point(DocumentView *view, double x, double y, gboolean button) {
	GArray *points = view->lasso_points;
	if (!points)
		return;
	gboolean freehand = was_alt_pressed_again(&view->modifiers) && view->modifiers.alt && button;
	if (freehand)
		add_path_point(points, x, y);
	else if (view->modifiers.shift && points->len >= 4)
		constrain_polygon_angle(g_array_index(points, double, points->len - 4),
								g_array_index(points, double, points->len - 3), &x, &y);
	g_array_index(points, double, points->len - 2) = x;
	g_array_index(points, double, points->len - 1) = y;
	gtk_widget_queue_draw(view->canvas);
}

static void move_lasso_point(DocumentView *view, double x, double y, gboolean button) {
	GArray *points = view->lasso_points;
	if (!points)
		return;
	if (button)
		add_path_point(points, x, y);
	else if (was_alt_pressed_again(&view->modifiers) && view->modifiers.alt) {
		g_array_index(points, double, points->len - 2) = x;
		g_array_index(points, double, points->len - 1) = y;
	} else
		return;
	gtk_widget_queue_draw(view->canvas);
}

static void follow_selection_pointer(DocumentView *view, double *x, double *y) {
	if (hypot(*x - view->select_x, *y - view->select_y) * view->zoom > 5) {
		view->select_dragged = TRUE;
		view->modifiers.dragged = TRUE;
	}
	if (is_polygon_view(view))
		snap_view_point(view, x, y);
	view->path_x = *x;
	view->path_y = *y;
}

void drag_selection_path(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	update_modifier_tracking(&view->modifiers, (modifiers & GDK_SHIFT_MASK) != 0, (modifiers & GDK_ALT_MASK) != 0);
	follow_selection_pointer(view, &x, &y);
	view->path_button = TRUE;
	if (is_polygon_view(view))
		move_polygon_point(view, x, y, TRUE);
	else
		move_lasso_point(view, x, y, TRUE);
}

void hover_selection_path(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	if (!view->lasso_points || !is_selection_path_tool(view->editor->tool))
		return;
	double document_x, document_y;
	get_document_point(view, x, y, &document_x, &document_y);
	update_modifier_tracking(&view->modifiers, (modifiers & GDK_SHIFT_MASK) != 0, (modifiers & GDK_ALT_MASK) != 0);
	follow_selection_pointer(view, &document_x, &document_y);
	view->path_button = FALSE;
	if (is_polygon_view(view))
		move_polygon_point(view, document_x, document_y, FALSE);
	else
		move_lasso_point(view, document_x, document_y, FALSE);
}

static void release_polygon(DocumentView *view, double x, double y) {
	gint64 now = g_get_monotonic_time() / 1000;
	if (!view->lasso_points) {
		view->lasso_points = g_array_new(FALSE, FALSE, sizeof(double));
		add_path_point(view->lasso_points, x, y);
		add_path_point(view->lasso_points, x, y);
	} else {
		double previous_x = g_array_index(view->lasso_points, double, view->lasso_points->len - 4);
		double previous_y = g_array_index(view->lasso_points, double, view->lasso_points->len - 3);
		move_polygon_point(view, x, y, FALSE);
		double start_x = g_array_index(view->lasso_points, double, 0);
		double start_y = g_array_index(view->lasso_points, double, 1);
		int distance = 5;
		g_object_get(gtk_widget_get_settings(view->canvas), "gtk-double-click-distance", &distance, NULL);
		gboolean double_click = now - view->polygon_release_time < DOUBLE_CLICK_MILLISECONDS &&
								hypot(x - previous_x, y - previous_y) * view->zoom <= distance;
		if (double_click || hypot(x - start_x, y - start_y) < CLOSE_DISTANCE / view->zoom) {
			finish_selection_path(view, FALSE);
			return;
		}
		add_path_point(view->lasso_points, x, y);
	}
	view->polygon_release_time = now;
	refresh_selection_display(view);
}

void release_selection_path(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	view->selecting = view->path_button = FALSE;
	update_modifier_tracking(&view->modifiers, (modifiers & GDK_SHIFT_MASK) != 0, (modifiers & GDK_ALT_MASK) != 0);
	snap_view_point(view, &x, &y);
	view->path_x = x;
	view->path_y = y;
	if (is_polygon_view(view))
		release_polygon(view, x, y);
	else if (view->lasso_points && was_alt_pressed_again(&view->modifiers) && view->modifiers.alt)
		add_path_point(view->lasso_points, x, y);
	else if (view->lasso_points)
		finish_selection_path(view, !view->select_dragged || (x == view->select_x && y == view->select_y));
	view->modifiers.pressed = FALSE;
	clear_view_snap(view);
}

static gboolean resume_polygon(DocumentView *view) {
	Document *document = view->presented->document;
	if (!view->finished_polygon || !view->polygon_entry || !get_document_selection(document) ||
		get_latest_history_entry(document) != view->polygon_entry)
		return FALSE;
	undo_document_command(document);
	view->polygon_entry = NULL;
	view->lasso_points = g_array_copy(view->finished_polygon);
	refresh_selection_display(view);
	refresh_document_page(view->editor, get_current_page(view->editor), FALSE);
	return TRUE;
}

static gboolean press_polygon_key(DocumentView *view, guint key) {
	if (key == GDK_KEY_Escape && !view->lasso_points)
		return resume_polygon(view);
	GArray *points = view->lasso_points;
	if (!points)
		return FALSE;
	if (key == GDK_KEY_BackSpace || key == GDK_KEY_Delete || key == GDK_KEY_KP_Delete) {
		if (points->len <= 4)
			clear_selection_path(view);
		else {
			g_array_set_size(points, points->len - 2);
			move_polygon_point(view, view->path_x, view->path_y, view->path_button);
		}
		return TRUE;
	}
	if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) {
		if (points->len > 4)
			finish_selection_path(view, FALSE);
		else
			clear_selection_path(view);
		return TRUE;
	}
	if (key == GDK_KEY_Escape) {
		clear_selection_path(view);
		return TRUE;
	}
	return FALSE;
}

static gboolean is_modifier_down(guint key, gboolean pressed, GdkModifierType modifiers, GdkModifierType mask,
								 guint left, guint right) {
	return key == left || key == right ? pressed : (modifiers & mask) != 0;
}

static gboolean press_view_path_key(DocumentView *view, guint key, GdkModifierType modifiers, gboolean pressed) {
	if (!view)
		return FALSE;
	gboolean shift = is_modifier_down(key, pressed, modifiers, GDK_SHIFT_MASK, GDK_KEY_Shift_L, GDK_KEY_Shift_R);
	gboolean alt = is_modifier_down(key, pressed, modifiers, GDK_ALT_MASK, GDK_KEY_Alt_L, GDK_KEY_Alt_R);
	update_modifier_tracking(&view->modifiers, shift, alt);
	if (is_polygon_view(view))
		return pressed && press_polygon_key(view, key);
	if (view->lasso_points && !view->path_button && was_alt_pressed_again(&view->modifiers) && !alt)
		finish_selection_path(view, FALSE);
	return FALSE;
}

gboolean press_selection_path_key(Editor *editor, guint key, GdkModifierType modifiers, gboolean pressed) {
	GtkWidget *page = get_current_page(editor);
	if (!page || !is_selection_path_tool(editor->tool))
		return FALSE;
	DocumentView *primary = get_page_view(page, "primary"), *secondary = get_page_view(page, "secondary");
	DocumentView *view = secondary && (secondary->lasso_points || secondary->finished_polygon) && !primary->lasso_points
							 ? secondary
							 : primary;
	return press_view_path_key(view, key, modifiers, pressed);
}

void cancel_selection_paths(Editor *editor) {
	GtkWidget *page = get_current_page(editor);
	if (!page)
		return;
	DocumentView *views[] = {get_page_view(page, "primary"), get_page_view(page, "secondary")};
	for (guint index = 0; index < G_N_ELEMENTS(views); index++)
		if (views[index] && views[index]->lasso_points)
			clear_selection_path(views[index]);
}

void finish_ellipse_selection(DocumentView *view) {
	Document *document = view->presented->document;
	RasterExtent box = view->select_preview;
	int width = document_width(document), height = document_height(document);
	if (box.y + box.height < 0 || box.x > width || box.y > height || box.x + box.width < 0) {
		deselect(document, FALSE);
		return;
	}
	SelectionPath *path = create_ellipse_path(box);
	gboolean *anti_alias = get_selection_anti_alias(view->editor);
	select_path_area(document, path, view->select_mode, !anti_alias || *anti_alias, 10, 4, FALSE);
	free_selection_path(path);
}

/* ============================== Drawing ============================== */

static GskPath *build_screen_path(const SelectionPath *path, double x, double y, double zoom) {
	GskPathBuilder *builder = gsk_path_builder_new();
	const double *points = (const double *)path->points->data;
	guint position = 0;
	for (gsize index = 0; index < path->commands->len; index++) {
		const double *at = points + position;
		char command = path->commands->str[index];
		if (command == 'M')
			gsk_path_builder_move_to(builder, x + at[0] * zoom, y + at[1] * zoom);
		if (command == 'L')
			gsk_path_builder_line_to(builder, x + at[0] * zoom, y + at[1] * zoom);
		if (command == 'C')
			gsk_path_builder_cubic_to(builder, x + at[0] * zoom, y + at[1] * zoom, x + at[2] * zoom, y + at[3] * zoom,
									  x + at[4] * zoom, y + at[5] * zoom);
		position += command == 'C' ? 6 : 2;
	}
	return gsk_path_builder_free_to_path(builder);
}

/* The outline being drawn is a white line under black dashes that march like the selection ants */
static void stroke_preview_path(GtkSnapshot *snapshot, GskPath *path, double offset) {
	const GdkRGBA black = {0, 0, 0, 1}, white = {1, 1, 1, 1};
	GskStroke *stroke = gsk_stroke_new(1);
	gtk_snapshot_append_stroke(snapshot, path, stroke, &white);
	gsk_stroke_set_dash(stroke, (const float[]){4, 4}, 2);
	gsk_stroke_set_dash_offset(stroke, -offset);
	gtk_snapshot_append_stroke(snapshot, path, stroke, &black);
	gsk_stroke_free(stroke);
}

void draw_selection_path_preview(GtkSnapshot *snapshot, DocumentView *view, double x, double y) {
	SelectionPath *path = NULL;
	if (view->lasso_points && view->lasso_points->len >= 4)
		path = create_polygon_path((const double *)view->lasso_points->data, view->lasso_points->len / 2);
	else if (view->selecting && view->select_dragged && !g_strcmp0(view->editor->tool, TOOL_ELLIPSE_SELECT))
		path = create_ellipse_path(view->select_preview);
	if (!path)
		return;
	GskPath *screen = build_screen_path(path, x + 0.5, y + 0.5, view->zoom);
	stroke_preview_path(snapshot, screen, view->ants_offset);
	gsk_path_unref(screen);
	free_selection_path(path);
}
