#include <math.h>
#include <string.h>

#include "window/editor.h"
#include "document/layer-commands.h"
#include "move-tool.h"
#include "selection/pixel-selection.h"
#include "transform.h"
#include "crop.h"
#include "lang/en.h"

/* Past the grid handles come the reference point, the inside of the box and the band around it */
enum { HANDLE_REFERENCE = HANDLE_GRID_COUNT, HANDLE_MOVE, HANDLE_ROTATE };
#define NO_HANDLE (-1)
/* Handles answer within this many pixels, and the rotation band is this many handle sizes wide */
#define HANDLE_RADIUS 8
#define ROTATION_BAND 3
/* Handles are squares this many pixels from the middle to a side, and the reference point a bit larger circle */
#define HANDLE_SIZE 4
#define REFERENCE_SIZE 5
/* Rotating with Shift turns in steps of fifteen degrees */
#define ROTATION_STEP (G_PI / 12)
/* Pressing inside the box twice this quickly applies the transform */
#define DOUBLE_PRESS_MICROSECONDS (G_USEC_PER_SEC / 4)
/* A crop box smaller than this on both sides is dropped when released */
#define SMALLEST_CROP 2
/* Resize cursors come in eight directions, counted from the right */
#define CURSOR_DIRECTIONS 8
/* The rotation cursor: an arc with an arrow head and a white outline, turned in eighths */
#define ROTATION_CURSOR_SIZE 32
#define ROTATION_CURSOR_RADIUS 9
#define ROTATION_CURSOR_START 20
#define ROTATION_CURSOR_END 290
#define ROTATION_CURSOR_HEAD_LENGTH 6
#define ROTATION_CURSOR_HEAD_HALF_WIDTH 4.5
#define ROTATION_CURSOR_LINE 2.2
#define ROTATION_CURSOR_OUTLINE 2.6
#define DEGREES_PER_TURN 360
#define CURSOR_TURN_STEP (DEGREES_PER_TURN / CURSOR_DIRECTIONS)
/* The box and handle outline colour, and the shade outside a crop box */
static const GdkRGBA BOX_COLOR = {0.1, 0.4, 0.9, 1};
static const GdkRGBA CROP_SHADE = {0, 0, 0, .5};
/* The straighten line is a white line over a darker wider one */
#define STRAIGHTEN_OUTLINE_WIDTH 3
static const GdkRGBA STRAIGHTEN_OUTLINE = {0, 0, 0, .6};

struct TransformSession {
	DocumentView *view;
	TransformMode mode;
	RasterExtent bounds;
	double quad[QUAD_VALUES], start[QUAD_VALUES], press_x, press_y, handle_x, handle_y, reference_x, reference_y;
	gboolean custom_reference;
	int handle;
	MovePreview preview;
	gboolean crop, replaceable;
	gboolean keeps_ratio;
	gint64 last_press;
};

TransformSession *get_transform(DocumentView *view) {
	TransformSession *session = view->presented->transform;
	return session && session->view->presented == view->presented ? session : NULL;
}

/* ============================== Geometry ============================== */

static void get_grid(const double quad[QUAD_VALUES], double points[HANDLE_GRID_COUNT][2]) {
	Projective matrix = get_quad_matrix(quad);
	double last = HANDLE_GRID_SIZE - 1;
	for (int row = 0; row < HANDLE_GRID_SIZE; row++)
		for (int column = 0; column < HANDLE_GRID_SIZE; column++) {
			double *point = points[row * HANDLE_GRID_SIZE + column];
			apply_projective(matrix, column / last, row / last, &point[0], &point[1]);
		}
}

static void get_reference(TransformSession *session, const double quad[QUAD_VALUES], double *x, double *y) {
	if (session->custom_reference) {
		*x = session->reference_x;
		*y = session->reference_y;
		return;
	}
	double points[HANDLE_GRID_COUNT][2];
	get_grid(quad, points);
	*x = points[HANDLE_CENTER][0];
	*y = points[HANDLE_CENTER][1];
}

static void to_unit(const double quad[QUAD_VALUES], double x, double y, double *u, double *v) {
	apply_projective(invert_projective(get_quad_matrix(quad)), x, y, u, v);
}

static void to_screen(DocumentView *view, double x, double y, double *screen_x, double *screen_y) {
	double origin_x, origin_y;
	get_view_origin(view, &origin_x, &origin_y);
	*screen_x = origin_x + x * view->zoom;
	*screen_y = origin_y + y * view->zoom;
}

static int find_handle(TransformSession *session, double x, double y) {
	DocumentView *view = session->view;
	double radius = HANDLE_RADIUS, points[HANDLE_GRID_COUNT][2], reference_x, reference_y, screen_x, screen_y;
	get_grid(session->quad, points);
	get_reference(session, session->quad, &reference_x, &reference_y);
	to_screen(view, reference_x, reference_y, &screen_x, &screen_y);
	if (hypot(screen_x - x, screen_y - y) < radius)
		return HANDLE_REFERENCE;
	/* Corners win over edges where they are close */
	const int handles[] = {HANDLE_TOP_LEFT, HANDLE_TOP_RIGHT, HANDLE_BOTTOM_LEFT, HANDLE_BOTTOM_RIGHT,
						   HANDLE_TOP,		HANDLE_LEFT,	  HANDLE_RIGHT,		  HANDLE_BOTTOM};
	for (guint index = 0; index < G_N_ELEMENTS(handles); index++) {
		to_screen(view, points[handles[index]][0], points[handles[index]][1], &screen_x, &screen_y);
		if (hypot(screen_x - x, screen_y - y) < radius)
			return handles[index];
	}
	double origin_x, origin_y, u, v;
	get_view_origin(view, &origin_x, &origin_y);
	to_unit(session->quad, (x - origin_x) / view->zoom, (y - origin_y) / view->zoom, &u, &v);
	const double *top_left = points[HANDLE_TOP_LEFT], *top_right = points[HANDLE_TOP_RIGHT];
	const double *bottom_left = points[HANDLE_BOTTOM_LEFT];
	double width = hypot(top_right[0] - top_left[0], top_right[1] - top_left[1]) * view->zoom;
	double height = hypot(bottom_left[0] - top_left[0], bottom_left[1] - top_left[1]) * view->zoom;
	double across = u * width, down = v * height;
	gboolean near = across > -radius && across < width + radius && down > -radius && down < height + radius;
	if (near) {
		if (fabs(down) < radius)
			return HANDLE_TOP;
		if (fabs(across) < radius)
			return HANDLE_LEFT;
		if (fabs(down - height) < radius)
			return HANDLE_BOTTOM;
		if (fabs(across - width) < radius)
			return HANDLE_RIGHT;
	}
	if (u >= 0 && u <= 1 && v >= 0 && v <= 1)
		return HANDLE_MOVE;
	double band = ROTATION_BAND * radius;
	gboolean turn = across > -band && across < width + band && down > -band && down < height + band;
	return turn ? HANDLE_ROTATE : HANDLE_MOVE;
}

static double to_radians(double degrees) {
	return degrees * G_PI / (DEGREES_PER_TURN / 2);
}

/* A circular arrow with a white outline, turned in eighths, with the hotspot in the middle */
static GdkCursor *create_rotation_cursor(int direction) {
	const int size = ROTATION_CURSOR_SIZE;
	const double radius = ROTATION_CURSOR_RADIUS, start = to_radians(ROTATION_CURSOR_START);
	const double end = to_radians(ROTATION_CURSOR_END), head_length = ROTATION_CURSOR_HEAD_LENGTH;
	const double head_half_width = ROTATION_CURSOR_HEAD_HALF_WIDTH;
	cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *context = cairo_create(target);
	cairo_translate(context, size / 2.0, size / 2.0);
	cairo_rotate(context, to_radians(-CURSOR_TURN_STEP - direction * CURSOR_TURN_STEP));
	cairo_set_line_cap(context, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(context, CAIRO_LINE_JOIN_ROUND);
	double tip_x = radius * cos(end) - sin(end) * head_length, tip_y = radius * sin(end) + cos(end) * head_length;
	/* The white outline first, then the black arrow over it */
	for (int pass = 0; pass < 2; pass++) {
		double outline = pass ? 0 : ROTATION_CURSOR_OUTLINE;
		cairo_new_path(context);
		cairo_arc(context, 0, 0, radius, start, end);
		cairo_set_line_width(context, ROTATION_CURSOR_LINE + outline);
		cairo_set_source_rgba(context, pass, pass, pass, 1);
		cairo_stroke(context);
		cairo_move_to(context, tip_x, tip_y);
		cairo_line_to(context, (radius + head_half_width) * cos(end), (radius + head_half_width) * sin(end));
		cairo_line_to(context, (radius - head_half_width) * cos(end), (radius - head_half_width) * sin(end));
		cairo_close_path(context);
		cairo_set_line_width(context, outline);
		if (!pass)
			cairo_stroke_preserve(context);
		cairo_fill(context);
	}
	cairo_destroy(context);
	cairo_surface_flush(target);
	g_autoptr(GBytes) bytes =
		g_bytes_new(cairo_image_surface_get_data(target), (gsize)cairo_image_surface_get_stride(target) * size);
	g_autoptr(GdkTexture) texture =
		gdk_memory_texture_new(size, size, GDK_MEMORY_DEFAULT, bytes, cairo_image_surface_get_stride(target));
	cairo_surface_destroy(target);
	g_autoptr(GdkCursor) fallback = gdk_cursor_new_from_name("crosshair", NULL);
	return gdk_cursor_new_from_texture(texture, size / 2, size / 2, fallback);
}

static GdkCursor *get_rotation_cursor(TransformSession *session) {
	static GdkCursor *cursors[CURSOR_DIRECTIONS];
	double reference_x, reference_y, origin_x, origin_y;
	DocumentView *view = session->view;
	get_reference(session, session->quad, &reference_x, &reference_y);
	get_view_origin(view, &origin_x, &origin_y);
	double angle = atan2(reference_y - (view->pointer_y - origin_y) / view->zoom,
						 reference_x - (view->pointer_x - origin_x) / view->zoom);
	double degrees = DEGREES_PER_TURN / 4 - angle * (DEGREES_PER_TURN / 2) / G_PI;
	int direction = ((int)lround(degrees / CURSOR_TURN_STEP) % CURSOR_DIRECTIONS + CURSOR_DIRECTIONS) % CURSOR_DIRECTIONS;
	if (!cursors[direction])
		cursors[direction] = create_rotation_cursor(direction);
	return g_object_ref(cursors[direction]);
}

static const char *get_handle_cursor(TransformSession *session, int handle) {
	if (handle == HANDLE_MOVE)
		return "move";
	if (handle == HANDLE_REFERENCE)
		return "default";
	if (handle == HANDLE_ROTATE)
		return NULL;
	double points[HANDLE_GRID_COUNT][2];
	get_grid(session->quad, points);
	const double *center = points[HANDLE_CENTER];
	double angle = atan2(points[handle][1] - center[1], points[handle][0] - center[0]);
	const char *names[CURSOR_DIRECTIONS] = {"e-resize", "se-resize", "s-resize", "sw-resize",
											"w-resize", "nw-resize", "n-resize", "ne-resize"};
	int sector = ((int)lround(angle / to_radians(CURSOR_TURN_STEP)) % CURSOR_DIRECTIONS + CURSOR_DIRECTIONS) %
				 CURSOR_DIRECTIONS;
	return names[sector];
}

/* ============================== Dragging ============================== */

static void snap_dragged_point(gpointer view, double *x, double *y) {
	snap_view_point(view, x, y);
}

static void drag_handle(TransformSession *session, double x, double y, GdkModifierType modifiers) {
	gboolean shift = modifiers & GDK_SHIFT_MASK, alt = modifiers & GDK_ALT_MASK, control = modifiers & GDK_CONTROL_MASK;
	const double *start = session->start;
	double next[QUAD_VALUES];
	memcpy(next, start, sizeof next);
	int handle = session->handle;
	gboolean corner = is_corner_handle(handle);
	if (handle == HANDLE_REFERENCE) {
		session->custom_reference = TRUE;
		session->reference_x = x;
		session->reference_y = y;
		return;
	}
	if (handle == HANDLE_MOVE) {
		double moved_x = x - session->press_x, moved_y = y - session->press_y;
		if (is_axis_aligned_quad(start)) {
			moved_x = round(moved_x);
			moved_y = round(moved_y);
		}
		for (int point = 0; point < QUAD_CORNERS; point++) {
			next[point * 2] += moved_x;
			next[point * 2 + 1] += moved_y;
		}
	} else if (handle == HANDLE_ROTATE) {
		double center_x, center_y;
		get_reference(session, start, &center_x, &center_y);
		double angle =
			atan2(y - center_y, x - center_x) - atan2(session->press_y - center_y, session->press_x - center_x);
		if (shift)
			angle = round(angle / ROTATION_STEP) * ROTATION_STEP;
		double cosine = cos(angle), sine = sin(angle);
		for (int point = 0; point < QUAD_CORNERS; point++) {
			double px = start[point * 2] - center_x, py = start[point * 2 + 1] - center_y;
			next[point * 2] = center_x + px * cosine - py * sine;
			next[point * 2 + 1] = center_y + px * sine + py * cosine;
		}
	} else if (corner && control && !session->crop) {
		/* A free corner distorts the box, and Alt mirrors the opposite corner */
		int point = get_handle_corner(handle);
		next[point * 2] = x;
		next[point * 2 + 1] = y;
		if (alt) {
			int opposite = (point + QUAD_CORNERS / 2) % QUAD_CORNERS;
			double middle_x = (start[point * 2] + start[opposite * 2]) / 2;
			double middle_y = (start[point * 2 + 1] + start[opposite * 2 + 1]) / 2;
			next[opposite * 2] = 2 * middle_x - x;
			next[opposite * 2 + 1] = 2 * middle_y - y;
		}
	} else {
		BoxDrag drag = {.handle_x = session->handle_x,
						.handle_y = session->handle_y,
						.handle = handle,
						.keeps_ratio = session->keeps_ratio,
						.clamps_scale = session->crop,
						.skews = !session->crop,
						.snap = snap_dragged_point,
						.snap_data = session->view};
		memcpy(drag.start, start, sizeof drag.start);
		get_reference(session, start, &drag.reference_x, &drag.reference_y);
		scale_box(&drag, session->quad, x, y, shift, alt, control, next);
	}
	if (is_convex_quad(next))
		memcpy(session->quad, next, sizeof next);
}

/* ============================== Session ============================== */

void free_transform_session(TransformSession *session) {
	if (!session)
		return;
	clear_move_preview(&session->preview);
	g_free(session);
}

static void refresh_views(Editor *editor) {
	queue_canvas_redraw(editor);
	gtk_widget_set_visible(editor->transform_options, editor_transform(editor) != NULL);
}

TransformSession *editor_transform(Editor *editor) {
	PresentedDocument *presented = get_page_document(get_current_page(editor));
	return presented ? presented->transform : NULL;
}

void start_transform(Editor *editor, gboolean selection_only) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || presented->transform || presented->document->preview)
		return;
	Document *document = presented->document;
	if (selection_only && !get_document_selection(document))
		return;
	TransformMode mode = get_transform_mode(document, selection_only);
	RasterExtent bounds = get_transform_bounds(document, mode);
	if (empty_extent(bounds)) {
		gtk_label_set_text(GTK_LABEL(editor->status), TEXT_NOTHING_TO_TRANSFORM);
		return;
	}
	TransformSession *session = g_new0(TransformSession, 1);
	session->view = get_page_view(page, "primary");
	session->mode = mode;
	session->bounds = bounds;
	get_extent_quad(bounds, session->quad);
	session->handle = NO_HANDLE;
	if (mode != TRANSFORM_SELECTION) {
		g_autoptr(GArray) layers = collect_selected_subtrees(document);
		Record *floating = NULL;
		if (mode == TRANSFORM_PIXELS) {
			int index = get_item(get_record(document->record, "j"), 0)->value.number;
			floating = lift_pixels(document, get_item(get_record(document->record, "Z"), index), FALSE, NULL);
			g_array_set_size(layers, 0);
			g_array_append_val(layers, index);
		}
		fill_move_preview(&session->preview, document, layers, floating);
		if (floating)
			free_record(floating);
	}
	presented->transform = session;
	refresh_views(editor);
}

static void start_crop_box(Editor *editor, const double quad[QUAD_VALUES], gboolean keeps_ratio) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || presented->transform)
		return;
	TransformSession *session = g_new0(TransformSession, 1);
	session->view = get_page_view(page, "primary");
	session->crop = TRUE;
	session->keeps_ratio = keeps_ratio;
	memcpy(session->quad, quad, sizeof session->quad);
	session->handle = NO_HANDLE;
	presented->transform = session;
	refresh_views(editor);
}

void start_crop(Editor *editor, RasterExtent rectangle) {
	if (empty_extent(rectangle))
		return;
	double quad[QUAD_VALUES];
	get_extent_quad(rectangle, quad);
	start_crop_box(editor, quad, editor->crop_options.mode != CROP_FREE);
}

void start_crop_tool(Editor *editor) {
	PresentedDocument *presented = get_page_document(get_current_page(editor));
	if (!presented || presented->transform)
		return;
	Document *document = presented->document;
	/* Native change: without a selection the crop area is dragged first rather than covering the canvas */
	if (!get_document_selection(document))
		return;
	start_crop(editor, get_selection_extent(document));
	if (presented->transform)
		presented->transform->replaceable = TRUE;
}

void configure_crop(Editor *editor) {
	PresentedDocument *presented = get_page_document(get_current_page(editor));
	if (g_strcmp0(editor->tool, TOOL_CROP) || !presented)
		return;
	TransformSession *session = presented->transform;
	if (session && !session->crop)
		return;
	Document *document = presented->document;
	RasterExtent box = get_crop_start_box(document_width(document), document_height(document), &editor->crop_options);
	if (empty_extent(box))
		return;
	presented->transform = NULL;
	free_transform_session(session);
	start_crop(editor, box);
}

void start_straighten(Editor *editor) {
	PresentedDocument *presented = get_page_document(get_current_page(editor));
	if (!presented)
		return;
	if (presented->transform && presented->transform->crop)
		finish_transform(editor, FALSE);
	editor->straightening = TRUE;
	gtk_label_set_text(GTK_LABEL(editor->status), TEXT_CROP_DRAW_LINE);
}

void update_crop_drag(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	double origin_x, origin_y;
	get_view_origin(view, &origin_x, &origin_y);
	double document_x = (x - origin_x) / view->zoom, document_y = (y - origin_y) / view->zoom;
	view->select_dragged = TRUE;
	if (view->editor->straightening) {
		view->crop_end_x = document_x;
		view->crop_end_y = document_y;
		gtk_widget_queue_draw(view->canvas);
		return;
	}
	snap_view_point(view, &document_x, &document_y);
	view->select_preview =
		constrain_crop_drag(view->select_x, view->select_y, document_x, document_y, &view->editor->crop_options,
							(modifiers & GDK_SHIFT_MASK) != 0, (modifiers & GDK_ALT_MASK) != 0);
	gtk_widget_queue_draw(view->canvas);
}

void finish_crop_drag(DocumentView *view) {
	Editor *editor = view->editor;
	if (!view->select_dragged)
		return;
	if (!editor->straightening) {
		start_crop(editor, view->select_preview);
		return;
	}
	editor->straightening = FALSE;
	Document *document = view->presented->document;
	double quad[QUAD_VALUES];
	get_straighten_box(document_width(document), document_height(document), view->select_x, view->select_y,
					   view->crop_end_x, view->crop_end_y, quad);
	start_crop_box(editor, quad, FALSE);
}

void finish_transform_for_tool_change(Editor *editor) {
	TransformSession *session = editor_transform(editor);
	if (session)
		finish_transform(editor, !(session->crop && session->replaceable));
}

gboolean press_replaces_crop(DocumentView *view, double x, double y) {
	TransformSession *session = get_transform(view);
	if (!session || !session->crop || !session->replaceable)
		return FALSE;
	double origin_x, origin_y, u, v;
	get_view_origin(view, &origin_x, &origin_y);
	to_unit(session->quad, (x - origin_x) / view->zoom, (y - origin_y) / view->zoom, &u, &v);
	gboolean outside = u < 0 || u > 1 || v < 0 || v > 1;
	if (find_handle(session, x, y) != HANDLE_MOVE || !outside)
		return FALSE;
	view->presented->transform = NULL;
	free_transform_session(session);
	return TRUE;
}

void crop_to_selection(Editor *editor) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || !get_document_selection(presented->document))
		return;
	finish_transform(editor, FALSE);
	RasterExtent rectangle = get_selection_extent(presented->document);
	double quad[QUAD_VALUES];
	get_extent_quad(rectangle, quad);
	if (!crop_document(presented->document, quad, NULL, FALSE, FALSE))
		return;
	refresh_selection_display(get_page_view(page, "primary"));
	refresh_selection_display(get_page_view(page, "secondary"));
	refresh_document_page(editor, page, TRUE);
	fit_document_view(get_page_view(page, "primary"));
}

void finish_transform(Editor *editor, gboolean apply) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	TransformSession *session = presented ? presented->transform : NULL;
	if (!session)
		return;
	presented->transform = NULL;
	if (session->crop) {
		if (apply && crop_document(presented->document, session->quad, &editor->crop_options,
								   editor->settings.clear_selection_on_crop, FALSE)) {
			refresh_selection_display(get_page_view(page, "primary"));
			refresh_selection_display(get_page_view(page, "secondary"));
			refresh_document_page(editor, page, TRUE);
			fit_document_view(get_page_view(page, "primary"));
		}
		free_transform_session(session);
		refresh_views(editor);
		return;
	}
	double unchanged[QUAD_VALUES];
	get_extent_quad(session->bounds, unchanged);
	gboolean moved = memcmp(unchanged, session->quad, sizeof unchanged);
	if (apply && moved) {
		g_autoptr(GError) error = NULL;
		Document *document = presented->document;
		document->changed = (RasterExtent){0};
		if (!apply_transform(document, session->mode, session->quad, FALSE, &error) && error)
			gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		RasterExtent region = document->changed;
		document->changed = (RasterExtent){0};
		if (!empty_extent(region))
			update_document_region(editor, presented, region);
		refresh_selection_display(get_page_view(page, "primary"));
		refresh_selection_display(get_page_view(page, "secondary"));
		refresh_document_page(editor, page, empty_extent(region) && session->mode != TRANSFORM_SELECTION);
	}
	free_transform_session(session);
	refresh_views(editor);
}

/* ============================== Drawing ============================== */

static void draw_handle(GtkSnapshot *snapshot, double x, double y, gboolean round_marker) {
	double size = round_marker ? REFERENCE_SIZE : HANDLE_SIZE;
	GskRoundedRect outline;
	gsk_rounded_rect_init_from_rect(&outline, &GRAPHENE_RECT_INIT(x - size, y - size, size * 2, size * 2),
									round_marker ? size : 0);
	gtk_snapshot_push_rounded_clip(snapshot, &outline);
	gtk_snapshot_append_color(snapshot, &(GdkRGBA){1, 1, 1, 1}, &outline.bounds);
	gtk_snapshot_pop(snapshot);
	gtk_snapshot_append_border(snapshot, &outline, (const float[]){1, 1, 1, 1},
							   (const GdkRGBA[]){BOX_COLOR, BOX_COLOR, BOX_COLOR, BOX_COLOR});
}

/* The moving part is drawn by the GPU through the box's perspective, between the parts below and above it */
void draw_transform(DocumentView *view, GtkSnapshot *snapshot, double x, double y, const graphene_rect_t *image,
					const graphene_rect_t *visible) {
	TransformSession *session = get_transform(view);
	PresentedDocument *presented = view->presented;
	if (session->crop) {
		/* The document shows as it is, darkened outside the crop box */
		snapshot_tiled_image(presented->tiles, snapshot, image, visible, view->zoom, is_view_smoothed(view), FALSE);
		g_autoptr(GskPathBuilder) builder = gsk_path_builder_new();
		gsk_path_builder_add_rect(builder, visible);
		double screen_x, screen_y;
		for (int point = 0; point < QUAD_CORNERS; point++) {
			to_screen(view, session->quad[point * 2], session->quad[point * 2 + 1], &screen_x, &screen_y);
			if (point)
				gsk_path_builder_line_to(builder, screen_x, screen_y);
			else
				gsk_path_builder_move_to(builder, screen_x, screen_y);
		}
		gsk_path_builder_close(builder);
		g_autoptr(GskPath) path = gsk_path_builder_to_path(builder);
		gtk_snapshot_push_fill(snapshot, path, GSK_FILL_RULE_EVEN_ODD);
		gtk_snapshot_append_color(snapshot, &CROP_SHADE, visible);
		gtk_snapshot_pop(snapshot);
		return;
	}
	if (session->mode == TRANSFORM_SELECTION)
		snapshot_tiled_image(presented->tiles, snapshot, image, visible, view->zoom, is_view_smoothed(view), FALSE);
	else {
		const MovePreview *preview = &session->preview;
		snapshot_patched_image(presented->tiles, preview->parts[MOVE_PART_BELOW], preview->below_extent,
							   document_width(presented->document), document_height(presented->document), snapshot,
							   image, visible, view->zoom, is_view_smoothed(view));
		Projective matrix;
		RasterExtent bounds = session->bounds;
		TiledImage *moving = preview->whole ? presented->tiles : preview->parts[MOVE_PART_MOVING];
		double unchanged[QUAD_VALUES];
		get_extent_quad(bounds, unchanged);
		/* An untouched box draws the moving part exactly like the rest, so shared edges line up to the pixel */
		RasterExtent extent = preview->moving_extent;
		if (!memcmp(unchanged, session->quad, sizeof unchanged))
			snapshot_tiled_image(moving, snapshot,
								 &GRAPHENE_RECT_INIT(image->origin.x + extent.x * view->zoom,
													 image->origin.y + extent.y * view->zoom, extent.width * view->zoom,
													 extent.height * view->zoom),
								 visible, view->zoom, is_view_smoothed(view), FALSE);
		else if (projective_from_rectangle(session->quad, bounds.x, bounds.y, bounds.width, bounds.height, &matrix)) {
			const double *m = matrix.m;
			graphene_matrix_t perspective;
			graphene_matrix_init_from_float(&perspective, (const float[16]){m[0], m[3], 0, m[6], m[1], m[4], 0, m[7], 0,
																			0, 1, 0, m[2], m[5], 0, 1});
			gtk_snapshot_save(snapshot);
			gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(x, y));
			gtk_snapshot_scale(snapshot, view->zoom, view->zoom);
			gtk_snapshot_transform(snapshot, gsk_transform_matrix(NULL, &perspective));
			graphene_rect_t whole = GRAPHENE_RECT_INIT(extent.x, extent.y, extent.width, extent.height);
			snapshot_tiled_image(moving, snapshot, &whole, &whole, view->zoom, is_view_smoothed(view), TRUE);
			gtk_snapshot_restore(snapshot);
		}
		snapshot_tiled_image(preview->parts[MOVE_PART_ABOVE], snapshot, image, visible, view->zoom, is_view_smoothed(view), FALSE);
	}
}

/* The box outline, its eight handles and the reference point stay on top of the grid */
void draw_transform_box(DocumentView *view, GtkSnapshot *snapshot) {
	TransformSession *session = get_transform(view);
	double points[HANDLE_GRID_COUNT][2], screen[HANDLE_GRID_COUNT][2], reference_x, reference_y;
	get_grid(session->quad, points);
	for (int index = 0; index < HANDLE_GRID_COUNT; index++)
		to_screen(view, points[index][0], points[index][1], &screen[index][0], &screen[index][1]);
	g_autoptr(GskPathBuilder) builder = gsk_path_builder_new();
	const int corners[] = {HANDLE_TOP_LEFT, HANDLE_TOP_RIGHT, HANDLE_BOTTOM_RIGHT, HANDLE_BOTTOM_LEFT};
	gsk_path_builder_move_to(builder, screen[HANDLE_TOP_LEFT][0], screen[HANDLE_TOP_LEFT][1]);
	for (int index = 1; index <= QUAD_CORNERS; index++)
		gsk_path_builder_line_to(builder, screen[corners[index % QUAD_CORNERS]][0],
								 screen[corners[index % QUAD_CORNERS]][1]);
	g_autoptr(GskPath) path = gsk_path_builder_to_path(builder);
	g_autoptr(GskStroke) stroke = gsk_stroke_new(1);
	gtk_snapshot_append_stroke(snapshot, path, stroke, &BOX_COLOR);
	for (int index = 0; index < HANDLE_GRID_COUNT; index++)
		if (index != HANDLE_CENTER)
			draw_handle(snapshot, screen[index][0], screen[index][1], FALSE);
	get_reference(session, session->quad, &reference_x, &reference_y);
	double screen_x, screen_y;
	to_screen(view, reference_x, reference_y, &screen_x, &screen_y);
	draw_handle(snapshot, screen_x, screen_y, TRUE);
}

/* ============================== Input ============================== */

void press_transform(DocumentView *view, double x, double y) {
	TransformSession *session = get_transform(view);
	session->view = view;
	gint64 now = g_get_monotonic_time();
	session->handle = find_handle(session, x, y);
	if (session->handle == HANDLE_MOVE && now - session->last_press < DOUBLE_PRESS_MICROSECONDS) {
		session->handle = NO_HANDLE;
		finish_transform(view->editor, TRUE);
		return;
	}
	session->last_press = now;
	session->replaceable = FALSE;
	double origin_x, origin_y;
	get_view_origin(view, &origin_x, &origin_y);
	session->press_x = (x - origin_x) / view->zoom;
	session->press_y = (y - origin_y) / view->zoom;
	memcpy(session->start, session->quad, sizeof session->start);
	if (session->handle >= 0 && session->handle < HANDLE_GRID_COUNT) {
		double points[HANDLE_GRID_COUNT][2];
		get_grid(session->quad, points);
		session->handle_x = points[session->handle][0];
		session->handle_y = points[session->handle][1];
	}
}

void drag_transform(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	TransformSession *session = get_transform(view);
	if (!session || session->handle < 0)
		return;
	double origin_x, origin_y;
	get_view_origin(view, &origin_x, &origin_y);
	double document_x = (x - origin_x) / view->zoom, document_y = (y - origin_y) / view->zoom;
	if (session->handle < HANDLE_GRID_COUNT) {
		document_x += session->handle_x - session->press_x;
		document_y += session->handle_y - session->press_y;
		snap_view_point(view, &document_x, &document_y);
	} else if (session->handle == HANDLE_MOVE) {
		double left = G_MAXDOUBLE, top = G_MAXDOUBLE, right = -G_MAXDOUBLE, bottom = -G_MAXDOUBLE;
		for (int point = 0; point < QUAD_CORNERS; point++) {
			left = MIN(left, session->start[point * 2]);
			right = MAX(right, session->start[point * 2]);
			top = MIN(top, session->start[point * 2 + 1]);
			bottom = MAX(bottom, session->start[point * 2 + 1]);
		}
		double box_x = left + document_x - session->press_x, box_y = top + document_y - session->press_y;
		g_autoptr(GArray) excluded = collect_selected_subtrees(view->presented->document);
		snap_view_box(view, &box_x, &box_y, right - left, bottom - top, excluded, 0);
		document_x = session->press_x + box_x - left;
		document_y = session->press_y + box_y - top;
	}
	drag_handle(session, document_x, document_y, modifiers);
	queue_canvas_redraw(view->editor);
}

void release_transform(DocumentView *view) {
	TransformSession *session = get_transform(view);
	clear_view_snap(view);
	if (!session)
		return;
	session->handle = NO_HANDLE;
	const double *quad = session->quad;
	if (!session->crop || fabs(quad[0] - quad[4]) >= SMALLEST_CROP || fabs(quad[1] - quad[5]) >= SMALLEST_CROP)
		return;
	view->presented->transform = NULL;
	free_transform_session(session);
	refresh_views(view->editor);
}

/* The Straighten line while it is drawn */
void draw_crop_line(DocumentView *view, GtkSnapshot *snapshot) {
	if (!view->cropping || !view->editor->straightening || !view->select_dragged)
		return;
	double start_x, start_y, end_x, end_y;
	to_screen(view, view->select_x, view->select_y, &start_x, &start_y);
	to_screen(view, view->crop_end_x, view->crop_end_y, &end_x, &end_y);
	g_autoptr(GskPathBuilder) builder = gsk_path_builder_new();
	gsk_path_builder_move_to(builder, start_x, start_y);
	gsk_path_builder_line_to(builder, end_x, end_y);
	g_autoptr(GskPath) path = gsk_path_builder_to_path(builder);
	g_autoptr(GskStroke) outline = gsk_stroke_new(STRAIGHTEN_OUTLINE_WIDTH), line = gsk_stroke_new(1);
	gtk_snapshot_append_stroke(snapshot, path, outline, &STRAIGHTEN_OUTLINE);
	gtk_snapshot_append_stroke(snapshot, path, line, &(GdkRGBA){1, 1, 1, 1});
}

GdkCursor *get_transform_cursor(DocumentView *view, double x, double y) {
	TransformSession *session = get_transform(view);
	if (!session)
		return NULL;
	int handle = session->handle >= 0 ? session->handle : find_handle(session, x, y);
	if (handle == HANDLE_ROTATE)
		return get_rotation_cursor(session);
	return gdk_cursor_new_from_name(get_handle_cursor(session, handle), NULL);
}
