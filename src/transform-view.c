#include <math.h>
#include <string.h>

#include "editor.h"
#include "layer-commands.h"
#include "move-tool.h"
#include "pixel-selection.h"
#include "transform.h"
#include "crop.h"
#include "../lang/en.h"

enum { HANDLE_REFERENCE = 9, HANDLE_MOVE = 10, HANDLE_ROTATE = 11 };

struct TransformSession {
	DocumentView *view;
	TransformMode mode;
	RasterExtent bounds;
	double quad[8], start[8], press_x, press_y, handle_x, handle_y, reference_x, reference_y;
	gboolean custom_reference;
	int handle;
	TiledImage *parts[3];
	RasterExtent below_extent, moving_extent;
	gboolean whole, crop, replaceable;
	gboolean keeps_ratio;
	gint64 last_press;
};

TransformSession *get_transform(DocumentView *view) {
	TransformSession *session = view->presented->transform;
	return session && session->view->presented == view->presented ? session : NULL;
}

/* ============================== Geometry ============================== */

static Projective get_quad_matrix(const double quad[8]) {
	Projective matrix = {{1, 0, 0, 0, 1, 0, 0, 0}};
	projective_from_rectangle(quad, 0, 0, 1, 1, &matrix);
	return matrix;
}

static void get_grid(const double quad[8], double points[9][2]) {
	Projective matrix = get_quad_matrix(quad);
	for (int row = 0; row < 3; row++)
		for (int column = 0; column < 3; column++)
			apply_projective(matrix, column / 2.0, row / 2.0, &points[row * 3 + column][0],
							 &points[row * 3 + column][1]);
}

static void get_reference(TransformSession *session, const double quad[8], double *x, double *y) {
	if (session->custom_reference) {
		*x = session->reference_x;
		*y = session->reference_y;
		return;
	}
	double points[9][2];
	get_grid(quad, points);
	*x = points[4][0];
	*y = points[4][1];
}

static void to_unit(const double quad[8], double x, double y, double *u, double *v) {
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
	double radius = 8, points[9][2], reference_x, reference_y, screen_x, screen_y;
	get_grid(session->quad, points);
	get_reference(session, session->quad, &reference_x, &reference_y);
	to_screen(view, reference_x, reference_y, &screen_x, &screen_y);
	if (hypot(screen_x - x, screen_y - y) < radius)
		return HANDLE_REFERENCE;
	const int handles[8] = {0, 2, 6, 8, 1, 3, 5, 7};
	for (int index = 0; index < 8; index++) {
		to_screen(view, points[handles[index]][0], points[handles[index]][1], &screen_x, &screen_y);
		if (hypot(screen_x - x, screen_y - y) < radius)
			return handles[index];
	}
	double origin_x, origin_y, u, v;
	get_view_origin(view, &origin_x, &origin_y);
	to_unit(session->quad, (x - origin_x) / view->zoom, (y - origin_y) / view->zoom, &u, &v);
	double width = hypot(points[2][0] - points[0][0], points[2][1] - points[0][1]) * view->zoom;
	double height = hypot(points[6][0] - points[0][0], points[6][1] - points[0][1]) * view->zoom;
	double across = u * width, down = v * height;
	gboolean near = across > -radius && across < width + radius && down > -radius && down < height + radius;
	if (near) {
		if (fabs(down) < radius)
			return 1;
		if (fabs(across) < radius)
			return 3;
		if (fabs(down - height) < radius)
			return 7;
		if (fabs(across - width) < radius)
			return 5;
	}
	if (u >= 0 && u <= 1 && v >= 0 && v <= 1)
		return HANDLE_MOVE;
	double band = 3 * radius;
	gboolean turn = across > -band && across < width + band && down > -band && down < height + band;
	return turn ? HANDLE_ROTATE : HANDLE_MOVE;
}

/* A circular arrow with a white outline, turned in eighths, with the hotspot in the middle */
static GdkCursor *create_rotation_cursor(int direction) {
	const int size = 32;
	const double radius = 9, start = 20 * G_PI / 180, end = 290 * G_PI / 180, head_length = 6, head_half_width = 4.5;
	cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *context = cairo_create(target);
	cairo_translate(context, size / 2.0, size / 2.0);
	cairo_rotate(context, (-45 - direction * 45) * G_PI / 180);
	cairo_set_line_cap(context, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(context, CAIRO_LINE_JOIN_ROUND);
	double tip_x = radius * cos(end) - sin(end) * head_length, tip_y = radius * sin(end) + cos(end) * head_length;
	for (int pass = 0; pass < 2; pass++) {
		double outline = pass ? 0 : 2.6;
		cairo_new_path(context);
		cairo_arc(context, 0, 0, radius, start, end);
		cairo_set_line_width(context, 2.2 + outline);
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
	static GdkCursor *cursors[8];
	double reference_x, reference_y, origin_x, origin_y;
	DocumentView *view = session->view;
	get_reference(session, session->quad, &reference_x, &reference_y);
	get_view_origin(view, &origin_x, &origin_y);
	double angle = atan2(reference_y - (view->pointer_y - origin_y) / view->zoom,
						 reference_x - (view->pointer_x - origin_x) / view->zoom);
	int direction = (((int)lround(8 * (90 - angle * 180 / G_PI) / 360)) % 8 + 8) % 8;
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
	double points[9][2];
	get_grid(session->quad, points);
	double angle = atan2(points[handle][1] - points[4][1], points[handle][0] - points[4][0]);
	const char *names[8] = {"e-resize", "se-resize", "s-resize", "sw-resize",
							"w-resize", "nw-resize", "n-resize", "ne-resize"};
	int sector = ((int)lround(angle / (G_PI / 4)) % 8 + 8) % 8;
	return names[sector];
}

/* ============================== Dragging ============================== */

static void snap_dragged_point(gpointer view, double *x, double *y) {
	snap_view_point(view, x, y);
}

static void drag_handle(TransformSession *session, double x, double y, GdkModifierType modifiers) {
	gboolean shift = modifiers & GDK_SHIFT_MASK, alt = modifiers & GDK_ALT_MASK, control = modifiers & GDK_CONTROL_MASK;
	const double *start = session->start;
	double next[8];
	memcpy(next, start, sizeof next);
	int handle = session->handle;
	gboolean corner = handle == 0 || handle == 2 || handle == 6 || handle == 8;
	if (handle == HANDLE_REFERENCE) {
		session->custom_reference = TRUE;
		session->reference_x = x;
		session->reference_y = y;
		return;
	}
	if (handle == HANDLE_MOVE) {
		double moved_x = x - session->press_x, moved_y = y - session->press_y;
		gboolean aligned = (fabs(start[1] - start[3]) < 1e-6 && fabs(start[2] - start[4]) < 1e-6) ||
						   (fabs(start[0] - start[2]) < 1e-6 && fabs(start[3] - start[5]) < 1e-6);
		if (aligned) {
			moved_x = round(moved_x);
			moved_y = round(moved_y);
		}
		for (int point = 0; point < 4; point++) {
			next[point * 2] += moved_x;
			next[point * 2 + 1] += moved_y;
		}
	} else if (handle == HANDLE_ROTATE) {
		double center_x, center_y;
		get_reference(session, start, &center_x, &center_y);
		double angle =
			atan2(y - center_y, x - center_x) - atan2(session->press_y - center_y, session->press_x - center_x);
		if (shift)
			angle = round(angle / (G_PI / 12)) * (G_PI / 12);
		double cosine = cos(angle), sine = sin(angle);
		for (int point = 0; point < 4; point++) {
			double px = start[point * 2] - center_x, py = start[point * 2 + 1] - center_y;
			next[point * 2] = center_x + px * cosine - py * sine;
			next[point * 2 + 1] = center_y + px * sine + py * cosine;
		}
	} else if (corner && control && !session->crop) {
		/* A free corner distorts the box, and Alt mirrors the opposite corner */
		int point = (const int[9]){0, 0, 1, 0, 0, 0, 3, 0, 2}[handle];
		next[point * 2] = x;
		next[point * 2 + 1] = y;
		if (alt) {
			int opposite = (point + 2) % 4;
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

static void free_parts(TransformSession *session) {
	for (int part = 0; part < 3; part++)
		g_clear_pointer(&session->parts[part], free_tiled_image);
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
	const double quad[8] = {bounds.x,
							bounds.y,
							bounds.x + bounds.width,
							bounds.y,
							bounds.x + bounds.width,
							bounds.y + bounds.height,
							bounds.x,
							bounds.y + bounds.height};
	memcpy(session->quad, quad, sizeof quad);
	session->handle = -1;
	if (mode != TRANSFORM_SELECTION) {
		g_autoptr(GArray) layers = collect_selected_subtrees(document);
		Record *floating = NULL;
		if (mode == TRANSFORM_PIXELS) {
			int index = get_item(get_record(document->record, "j"), 0)->value.number;
			floating = lift_pixels(document, get_item(get_record(document->record, "Z"), index), FALSE, NULL);
			g_array_set_size(layers, 0);
			g_array_append_val(layers, index);
		}
		cairo_surface_t *parts[3];
		if (render_move_preview(document, layers, floating, &parts[0], &parts[1], &parts[2], &session->below_extent,
								&session->moving_extent)) {
			session->whole = !parts[1];
			for (int part = 0; part < 3; part++) {
				session->parts[part] = parts[part] ? create_tiled_image(parts[part]) : NULL;
				if (parts[part])
					cairo_surface_destroy(parts[part]);
			}
		}
		if (floating)
			free_record(floating);
	}
	presented->transform = session;
	refresh_views(editor);
}

static void start_crop_box(Editor *editor, const double quad[8], gboolean keeps_ratio) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || presented->transform)
		return;
	TransformSession *session = g_new0(TransformSession, 1);
	session->view = get_page_view(page, "primary");
	session->crop = TRUE;
	session->keeps_ratio = keeps_ratio;
	memcpy(session->quad, quad, sizeof session->quad);
	session->handle = -1;
	presented->transform = session;
	refresh_views(editor);
}

void start_crop(Editor *editor, RasterExtent rectangle) {
	if (empty_extent(rectangle))
		return;
	const double quad[8] = {rectangle.x,
							rectangle.y,
							rectangle.x + rectangle.width,
							rectangle.y,
							rectangle.x + rectangle.width,
							rectangle.y + rectangle.height,
							rectangle.x,
							rectangle.y + rectangle.height};
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
	g_free(session);
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
	double quad[8];
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
	g_free(session);
	return TRUE;
}

void crop_to_selection(Editor *editor) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || !get_document_selection(presented->document))
		return;
	finish_transform(editor, FALSE);
	RasterExtent rectangle = get_selection_extent(presented->document);
	const double quad[8] = {rectangle.x,
							rectangle.y,
							rectangle.x + rectangle.width,
							rectangle.y,
							rectangle.x + rectangle.width,
							rectangle.y + rectangle.height,
							rectangle.x,
							rectangle.y + rectangle.height};
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
		g_free(session);
		refresh_views(editor);
		return;
	}
	double unchanged[8] = {session->bounds.x,
						   session->bounds.y,
						   session->bounds.x + session->bounds.width,
						   session->bounds.y,
						   session->bounds.x + session->bounds.width,
						   session->bounds.y + session->bounds.height,
						   session->bounds.x,
						   session->bounds.y + session->bounds.height};
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
	free_parts(session);
	g_free(session);
	refresh_views(editor);
}

/* ============================== Drawing ============================== */

static void draw_handle(GtkSnapshot *snapshot, double x, double y, gboolean round_marker) {
	double size = round_marker ? 5 : 4;
	GskRoundedRect outline;
	gsk_rounded_rect_init_from_rect(&outline, &GRAPHENE_RECT_INIT(x - size, y - size, size * 2, size * 2),
									round_marker ? size : 0);
	gtk_snapshot_push_rounded_clip(snapshot, &outline);
	gtk_snapshot_append_color(snapshot, &(GdkRGBA){1, 1, 1, 1}, &outline.bounds);
	gtk_snapshot_pop(snapshot);
	const GdkRGBA border = {0.1, 0.4, 0.9, 1};
	gtk_snapshot_append_border(snapshot, &outline, (const float[4]){1, 1, 1, 1},
							   (const GdkRGBA[4]){border, border, border, border});
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
		for (int point = 0; point < 4; point++) {
			to_screen(view, session->quad[point * 2], session->quad[point * 2 + 1], &screen_x, &screen_y);
			if (point)
				gsk_path_builder_line_to(builder, screen_x, screen_y);
			else
				gsk_path_builder_move_to(builder, screen_x, screen_y);
		}
		gsk_path_builder_close(builder);
		g_autoptr(GskPath) path = gsk_path_builder_to_path(builder);
		gtk_snapshot_push_fill(snapshot, path, GSK_FILL_RULE_EVEN_ODD);
		gtk_snapshot_append_color(snapshot, &(GdkRGBA){0, 0, 0, .5}, visible);
		gtk_snapshot_pop(snapshot);
		return;
	}
	if (session->mode == TRANSFORM_SELECTION)
		snapshot_tiled_image(presented->tiles, snapshot, image, visible, view->zoom, is_view_smoothed(view), FALSE);
	else {
		snapshot_patched_image(presented->tiles, session->parts[0], session->below_extent,
							   document_width(presented->document), document_height(presented->document), snapshot,
							   image, visible, view->zoom, is_view_smoothed(view));
		Projective matrix;
		RasterExtent bounds = session->bounds;
		TiledImage *moving = session->whole ? presented->tiles : session->parts[1];
		const double unchanged[8] = {bounds.x,
									 bounds.y,
									 bounds.x + bounds.width,
									 bounds.y,
									 bounds.x + bounds.width,
									 bounds.y + bounds.height,
									 bounds.x,
									 bounds.y + bounds.height};
		/* An untouched box draws the moving part exactly like the rest, so shared edges line up to the pixel */
		RasterExtent extent = session->moving_extent;
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
		snapshot_tiled_image(session->parts[2], snapshot, image, visible, view->zoom, is_view_smoothed(view), FALSE);
	}
}

/* The box outline, its eight handles and the reference point stay on top of the grid */
void draw_transform_box(DocumentView *view, GtkSnapshot *snapshot) {
	TransformSession *session = get_transform(view);
	double points[9][2], screen[9][2], reference_x, reference_y;
	get_grid(session->quad, points);
	for (int index = 0; index < 9; index++)
		to_screen(view, points[index][0], points[index][1], &screen[index][0], &screen[index][1]);
	g_autoptr(GskPathBuilder) builder = gsk_path_builder_new();
	const int corners[4] = {0, 2, 8, 6};
	gsk_path_builder_move_to(builder, screen[0][0], screen[0][1]);
	for (int index = 1; index <= 4; index++)
		gsk_path_builder_line_to(builder, screen[corners[index % 4]][0], screen[corners[index % 4]][1]);
	g_autoptr(GskPath) path = gsk_path_builder_to_path(builder);
	g_autoptr(GskStroke) stroke = gsk_stroke_new(1);
	gtk_snapshot_append_stroke(snapshot, path, stroke, &(GdkRGBA){0.1, 0.4, 0.9, 1});
	for (int index = 0; index < 9; index++)
		if (index != 4)
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
	if (session->handle == HANDLE_MOVE && now - session->last_press < 250000) {
		session->handle = -1;
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
	if (session->handle >= 0 && session->handle <= 8) {
		double points[9][2];
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
	if (session->handle <= 8) {
		document_x += session->handle_x - session->press_x;
		document_y += session->handle_y - session->press_y;
		snap_view_point(view, &document_x, &document_y);
	} else if (session->handle == HANDLE_MOVE) {
		double left = G_MAXDOUBLE, top = G_MAXDOUBLE, right = -G_MAXDOUBLE, bottom = -G_MAXDOUBLE;
		for (int point = 0; point < 4; point++) {
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
	session->handle = -1;
	const double *quad = session->quad;
	if (!session->crop || fabs(quad[0] - quad[4]) >= 2 || fabs(quad[1] - quad[5]) >= 2)
		return;
	view->presented->transform = NULL;
	g_free(session);
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
	g_autoptr(GskStroke) outline = gsk_stroke_new(3), line = gsk_stroke_new(1);
	gtk_snapshot_append_stroke(snapshot, path, outline, &(GdkRGBA){0, 0, 0, .6});
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
