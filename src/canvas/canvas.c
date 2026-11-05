#include <math.h>

#include "canvas-tools.h"
#include "selection/pixel-selection.h"
#include "document/layer-commands.h"
#include "document/history.h"
#include "tools/transform.h"
#include "snapshot-area.h"
#include "lang/en.h"

/* The brush tip outline and crosshair: a light edge around a dark line, and how far the crosshair reaches */
static const GdkRGBA TIP_LIGHT = {1, 1, 1, .9}, TIP_DARK = {0, 0, 0, .9};
#define CROSSHAIR_ARM 4
/* A fitted view leaves this much room around the document, and stays between these zooms */
#define FIT_MARGIN 64.0
#define FIT_ZOOM_LOWEST 0.01
#define FIT_ZOOM_HIGHEST 64
/* Brush smoothing follows the zoom, so it feels the same at any zoom */
#define SMOOTHING_SCALE 50
/* A smooth wheel zooms this much per unit it turns */
#define ZOOM_PER_WHEEL_UNIT 8e-4
/* Adjusting the brush by dragging: each pixel sideways adds two to the diameter, and four pixels up or down change
 * the hardness by one */
#define DIAMETER_PER_PIXEL 2
#define PIXELS_PER_HARDNESS 4
/* Square tips show at least this many pixels wide, and rings at least this many from their middle */
#define SMALLEST_TIP_SQUARE 3
#define SMALLEST_TIP_RING 1.5
/* The document lifts off the pasteboard on a soft shadow inside a thin edge, like a sheet of paper */
static const GdkRGBA DOCUMENT_SHADOW = {0, 0, 0, .35}, DOCUMENT_EDGE = {0, 0, 0, .25};
#define DOCUMENT_SHADOW_DROP 3
#define DOCUMENT_SHADOW_BLUR 18

/* A paint stroke begins, moves and ends, and Shift begins it from the last point or moves it along one axis */
typedef enum { STROKE_BEGIN, STROKE_MOVE, STROKE_END, STROKE_BEGIN_FROM_LAST, STROKE_MOVE_ALONG_AXIS } StrokePhase;
/* While size and hardness change, a red fill shows the hard core fading to the edge */
#define FALLOFF_COLOR ((GdkRGBA){0.9, 0.2, 0.2, 0.5})
#define FALLOFF_EDGE ((GdkRGBA){0.9, 0.2, 0.2, 0})
#define FALLOFF_CORE_LIMIT 0.999f

static void draw_snap_lines(GtkSnapshot *snapshot, DocumentView *view, double x, double y);
static void draw_gradient_line(GtkSnapshot *snapshot, DocumentView *view, double x, double y);

/* The transparency checkerboard in the shades chosen in Preferences, shared by the canvas and the Navigator */
void snapshot_checkerboard(GtkSnapshot *snapshot, const graphene_rect_t *area, int colors, int cell) {
	static const GdkRGBA shades[3][2] = {{{0.9, 0.9, 0.9, 1}, {0.75, 0.75, 0.75, 1}},
										 {{0.6, 0.6, 0.6, 1}, {0.45, 0.45, 0.45, 1}},
										 {{0.3, 0.3, 0.3, 1}, {0.2, 0.2, 0.2, 1}}};
	const GdkRGBA *light = &shades[colors][0], *dark = &shades[colors][1];
	gtk_snapshot_push_repeat(snapshot, area, &GRAPHENE_RECT_INIT(0, 0, cell * 2, cell * 2));
	gtk_snapshot_append_color(snapshot, dark, &GRAPHENE_RECT_INIT(0, 0, cell * 2, cell * 2));
	gtk_snapshot_append_color(snapshot, light, &GRAPHENE_RECT_INIT(0, 0, cell, cell));
	gtk_snapshot_append_color(snapshot, light, &GRAPHENE_RECT_INIT(cell, cell, cell, cell));
	gtk_snapshot_pop(snapshot);
}

/* Views without an editor, such as previews, use the default settings */
static const Settings *get_view_settings(const DocumentView *view) {
	return view->editor ? &view->editor->settings : get_default_settings();
}

/* The pixel grid shows from the zoom chosen in Preferences */
static int get_view_grid_zoom(const DocumentView *view) {
	return get_grid_zoom(get_view_settings(view));
}

/* Zoomed out views blend pixels unless the Smooth zoomed out image preference is off */
gboolean is_view_smoothed(const DocumentView *view) {
	return view->zoom < 1 && (!view->editor || view->editor->settings.smooth_zoomed_out);
}

void notify_view_changed(DocumentView *view) {
	gtk_widget_queue_draw(view->canvas);
	if (view->changed)
		view->changed(view, view->changed_data);
}

/* Drawing must not change widgets, so the fitted zoom follows allocation changes instead */
static void update_fitted_zoom(DocumentView *view, int width, int height) {
	if (!view->fitted || width <= 0 || height <= 0)
		return;
	Document *document = view->presented->document;
	view->zoom =
		CLAMP(MIN((width - FIT_MARGIN) / document_width(document), (height - FIT_MARGIN) / document_height(document)),
			  FIT_ZOOM_LOWEST, FIT_ZOOM_HIGHEST);
}

void fit_document_view(DocumentView *view) {
	view->fitted = TRUE;
	view->pan_x = view->pan_y = 0;
	update_fitted_zoom(view, gtk_widget_get_width(view->canvas), gtk_widget_get_height(view->canvas));
	notify_view_changed(view);
}

void show_actual_pixels(DocumentView *view) {
	view->zoom = 1;
	view->pan_x = view->pan_y = 0;
	view->fitted = FALSE;
	notify_view_changed(view);
}

/* The document's top left corner in canvas coordinates */
void get_view_origin(DocumentView *view, double *x, double *y) {
	Document *document = view->presented->document;
	*x = (gtk_widget_get_width(view->canvas) - document_width(document) * view->zoom) / 2 + view->pan_x;
	*y = (gtk_widget_get_height(view->canvas) - document_height(document) * view->zoom) / 2 + view->pan_y;
}

/* A two tone ring shows the brush footprint on any artwork while a paint tool is active */
/* A small dark cross with a light edge marks the exact pointer inside the brush tip */
static void draw_tip_crosshair(GtkSnapshot *snapshot, double center_x, double center_y) {
	double x = round(center_x), y = round(center_y);
	/* The light cross is one pixel larger on every side, so it edges the dark one */
	const int arm = CROSSHAIR_ARM, light = arm + 1;
	gtk_snapshot_append_color(snapshot, &TIP_LIGHT, &GRAPHENE_RECT_INIT(x - light, y - 1, 2 * light + 1, 3));
	gtk_snapshot_append_color(snapshot, &TIP_LIGHT, &GRAPHENE_RECT_INIT(x - 1, y - light, 3, 2 * light + 1));
	gtk_snapshot_append_color(snapshot, &TIP_DARK, &GRAPHENE_RECT_INIT(x - arm, y, 2 * arm + 1, 1));
	gtk_snapshot_append_color(snapshot, &TIP_DARK, &GRAPHENE_RECT_INIT(x, y - arm, 1, 2 * arm + 1));
}

/* A one pixel black line, over a wider white one when it must show on dark artwork too */
static void append_guide_line(GtkSnapshot *snapshot, double x, double y, double end_x, double end_y,
							  gboolean outlined) {
	g_autoptr(GskPathBuilder) builder = gsk_path_builder_new();
	gsk_path_builder_move_to(builder, x, y);
	gsk_path_builder_line_to(builder, end_x, end_y);
	g_autoptr(GskPath) path = gsk_path_builder_to_path(builder);
	if (outlined) {
		g_autoptr(GskStroke) under = gsk_stroke_new(3);
		gtk_snapshot_append_stroke(snapshot, path, under, &(GdkRGBA){1, 1, 1, .9});
	}
	g_autoptr(GskStroke) stroke = gsk_stroke_new(1);
	gtk_snapshot_append_stroke(snapshot, path, stroke, &(GdkRGBA){0, 0, 0, 1});
}

/* While Shift is held, a line shows where a click joins the last stroke with a straight line */
static void draw_shift_line(GtkSnapshot *snapshot, DocumentView *view) {
	PaintTool *tool = &view->editor->paint_tool;
	if (!view->editor->shift_held || view->painting || !tool->has_last || tool->document != view->presented->document)
		return;
	double origin_x, origin_y;
	get_view_origin(view, &origin_x, &origin_y);
	append_guide_line(snapshot, origin_x + tool->last_x * view->zoom, origin_y + tool->last_y * view->zoom,
					  view->pointer_x, view->pointer_y, TRUE);
}

/* Native painting symmetry shows its axes through the middle of the image while a painting tool is active */
static void draw_symmetry_axes(GtkSnapshot *snapshot, DocumentView *view, const graphene_rect_t *image) {
	Editor *editor = view->editor;
	if (!editor || !editor->symmetry || !is_paint_tool(editor->tool))
		return;
	double middle_x = round(image->origin.x + image->size.width / 2) + .5;
	double middle_y = round(image->origin.y + image->size.height / 2) + .5;
	if (editor->symmetry & SYMMETRY_VERTICAL)
		append_guide_line(snapshot, middle_x, image->origin.y, middle_x, image->origin.y + image->size.height, TRUE);
	if (editor->symmetry & SYMMETRY_HORIZONTAL)
		append_guide_line(snapshot, image->origin.x, middle_y, image->origin.x + image->size.width, middle_y, TRUE);
}

static void draw_brush_outline(GtkSnapshot *snapshot, DocumentView *view) {
	Editor *editor = view->editor;
	if (!editor || !is_paint_tool(editor->tool))
		return;
	/* While the right click brush picker is open the outline stays where it opened and follows its sliders */
	if (!view->brush_anchored && (!view->pointer_inside || view->panning || editor->space_held))
		return;
	double center_x = view->brush_anchored ? view->anchor_x : view->pointer_x;
	double center_y = view->brush_anchored ? view->anchor_y : view->pointer_y;
	if (editor->settings.brush_crosshair)
		draw_tip_crosshair(snapshot, center_x, center_y);
	draw_shift_line(snapshot, view);
	PaintOptions *options = get_tool_options(editor);
	gboolean hard = !g_strcmp0(editor->tool, PAINT_TOOL_PENCIL) ||
					(!g_strcmp0(editor->tool, PAINT_TOOL_ERASER) && options->eraser_mode == 1);
	if ((hard && options->square) || options->shape.diameter <= 1) {
		/* Square and one pixel tips show a square of the tip's size that follows the pointer like the brush ring */
		int size = MAX(1, (int)lround(options->shape.diameter));
		double side = MAX(SMALLEST_TIP_SQUARE, size * view->zoom);
		graphene_rect_t square = GRAPHENE_RECT_INIT(center_x - side / 2, center_y - side / 2, side, side);
		graphene_rect_t outer;
		graphene_rect_inset_r(&square, -1, -1, &outer);
		append_outline(snapshot, outer, 0, 1, &TIP_LIGHT);
		append_outline(snapshot, square, 0, 1, &TIP_DARK);
		return;
	}
	double radius = MAX(get_tool_options(editor)->shape.diameter * view->zoom / 2, SMALLEST_TIP_RING);
	if (view->adjusting || view->brush_anchored) {
		/* The falloff preview fades from the hard core to the edge while size and hardness change */
		float hardness = from_percent(get_tool_options(editor)->shape.hardness);
		const GskColorStop stops[] = {
			{0, FALLOFF_COLOR}, {MIN(hardness, FALLOFF_CORE_LIMIT), FALLOFF_COLOR}, {1, FALLOFF_EDGE}};
		graphene_rect_t area = square_around(center_x, center_y, radius);
		gtk_snapshot_append_radial_gradient(snapshot, &area, &GRAPHENE_POINT_INIT(center_x, center_y), radius, radius,
											0, 1, stops, G_N_ELEMENTS(stops));
	}

	const struct {
		double radius;
		GdkRGBA color;
	} rings[] = {{radius + 1, TIP_LIGHT}, {MAX(radius, 1), TIP_DARK}};

	for (guint ring = 0; ring < G_N_ELEMENTS(rings); ring++) {
		double size = rings[ring].radius;
		append_outline(snapshot, square_around(center_x, center_y, size), size, 1, &rings[ring].color);
	}
}

/* One line between each pair of visible pixels, snapped to whole screen pixels. The document's own edges get no
 * line, so all four sides look the same */
static void draw_pixel_grid(GtkSnapshot *snapshot, DocumentView *view, double x, double y,
							const graphene_rect_t *bounds) {
	Document *document = view->presented->document;
	int image_width = document_width(document), image_height = document_height(document);
	gboolean light = view->editor && view->editor->settings.grid_color == 1;
	const GdkRGBA color = {light, light, light, light ? 0.35 : 0.25};
	int left = MAX(0, (int)floor((bounds->origin.x - x) / view->zoom));
	int top = MAX(0, (int)floor((bounds->origin.y - y) / view->zoom));
	int right = MIN(image_width, (int)ceil((bounds->origin.x + bounds->size.width - x) / view->zoom));
	int bottom = MIN(image_height, (int)ceil((bounds->origin.y + bounds->size.height - y) / view->zoom));
	for (int column = MAX(left, 1); column <= MIN(right, image_width - 1); column++)
		gtk_snapshot_append_color(
			snapshot, &color,
			&GRAPHENE_RECT_INIT(floor(x + column * view->zoom), bounds->origin.y, 1, bounds->size.height));
	for (int row = MAX(top, 1); row <= MIN(bottom, image_height - 1); row++)
		gtk_snapshot_append_color(
			snapshot, &color,
			&GRAPHENE_RECT_INIT(bounds->origin.x, floor(y + row * view->zoom), bounds->size.width, 1));
}

/* For seamless textures and tiles, the eight neighbouring copies show how the edges meet, like the
 * tiled mode of pixel art editors, and painting on any copy wraps across the edges */
static void draw_tile_preview(GtkSnapshot *snapshot, DocumentView *view, const graphene_rect_t *image, int width,
							  int height) {
	const Settings *settings = get_view_settings(view);
	for (int row = -1; row <= 1; row++)
		for (int column = -1; column <= 1; column++) {
			if (!row && !column)
				continue;
			graphene_rect_t copy = *image, shown;
			graphene_rect_offset(&copy, column * image->size.width, row * image->size.height);
			if (!graphene_rect_intersection(&copy, &GRAPHENE_RECT_INIT(0, 0, width, height), &shown))
				continue;
			snapshot_checkerboard(snapshot, &shown, settings->checker_colors, get_checker_size(settings));
			gtk_snapshot_push_clip(snapshot, &shown);
			snapshot_tiled_image(view->presented->tiles, snapshot, &copy, &shown, view->zoom, is_view_smoothed(view),
								 FALSE);
			gtk_snapshot_pop(snapshot);
		}
}

/* The GPU scales the document texture, so redraws cost the same at any image size */
static void draw_document(GtkWidget *widget, GtkSnapshot *snapshot, int width, int height, gpointer data) {
	DocumentView *view = data;
	Document *document = view->presented->document;
	int image_width = document_width(document), image_height = document_height(document);
	double x = (width - image_width * view->zoom) / 2 + view->pan_x;
	double y = (height - image_height * view->zoom) / 2 + view->pan_y;
	/* Whole pixel bounds keep the checkerboard from bleeding through partly covered edge pixels */
	double left = round(x), top = round(y);
	double right = round(x + image_width * view->zoom), bottom = round(y + image_height * view->zoom);
	graphene_rect_t image = GRAPHENE_RECT_INIT(left, top, MAX(1, right - left), MAX(1, bottom - top));
	graphene_rect_t visible;
	if (!graphene_rect_intersection(&image, &GRAPHENE_RECT_INIT(0, 0, width, height), &visible)) {
		draw_brush_outline(snapshot, view);
		return;
	}
	if (!view->tiled) {
		GskRoundedRect sheet;
		gsk_rounded_rect_init_from_rect(&sheet, &image, 0);
		gtk_snapshot_append_outset_shadow(snapshot, &sheet, &DOCUMENT_SHADOW, 0, DOCUMENT_SHADOW_DROP, 0,
										  DOCUMENT_SHADOW_BLUR);
		graphene_rect_t edge;
		graphene_rect_inset_r(&image, -1, -1, &edge);
		append_outline(snapshot, edge, 0, 1, &DOCUMENT_EDGE);
	}
	/* The checkerboard and grid are viewing aids outside the document record */
	const Settings *settings = get_view_settings(view);
	snapshot_checkerboard(snapshot, &visible, settings->checker_colors, get_checker_size(settings));
	PresentedDocument *presented = view->presented;
	if (view->tiled)
		draw_tile_preview(snapshot, view, &image, width, height);
	gtk_snapshot_push_clip(snapshot, &visible);
	if (get_transform(view)) {
		gtk_snapshot_pop(snapshot);
		gtk_snapshot_push_clip(snapshot, &visible);
		draw_transform(view, snapshot, left, top, &image, &visible);
		gtk_snapshot_pop(snapshot);
		if (view->grid && view->zoom >= get_view_grid_zoom(view))
			draw_pixel_grid(snapshot, view, left, top, &visible);
		draw_transform_box(view, snapshot);
		draw_snap_lines(snapshot, view, left, top);
		return;
	}
	const MovePreview *preview = &presented->move_preview;
	if (has_move_preview(preview)) {
		/* During a move the moving part is offset by the drag while the rest stays put */
		snapshot_patched_image(presented->tiles, preview->parts[MOVE_PART_BELOW], preview->below_extent,
							   document_width(presented->document), document_height(presented->document), snapshot,
							   &image, &visible, view->zoom, is_view_smoothed(view));
		for (int part = MOVE_PART_MOVING; part < MOVE_PARTS; part++) {
			gboolean moving = part == MOVE_PART_MOVING;
			TiledImage *tiles = moving && preview->whole ? presented->tiles : preview->parts[part];
			graphene_rect_t bounds = image;
			if (moving) {
				RasterExtent extent = preview->moving_extent;
				bounds = GRAPHENE_RECT_INIT(image.origin.x + round((extent.x + view->move.moved_x) * view->zoom),
											image.origin.y + round((extent.y + view->move.moved_y) * view->zoom),
											extent.width * view->zoom, extent.height * view->zoom);
			}
			snapshot_tiled_image(tiles, snapshot, &bounds, &visible, view->zoom, is_view_smoothed(view), FALSE);
		}
	} else
		snapshot_tiled_image(presented->tiles, snapshot, &image, &visible, view->zoom, is_view_smoothed(view), FALSE);
	gtk_snapshot_pop(snapshot);
	if (view->grid && view->zoom >= get_view_grid_zoom(view))
		draw_pixel_grid(snapshot, view, left, top, &visible);
	draw_selection(snapshot, view, left, top, &GRAPHENE_RECT_INIT(0, 0, width, height));
	draw_crop_line(view, snapshot);
	draw_snap_lines(snapshot, view, left, top);
	draw_gradient_line(snapshot, view, left, top);
	draw_symmetry_axes(snapshot, view, &image);
	draw_brush_outline(snapshot, view);
}

static void resize_view(GtkWidget *widget, int width, int height, gpointer data) {
	DocumentView *view = data;
	if (!view->fitted)
		return;
	update_fitted_zoom(view, width, height);
	if (view->changed)
		view->changed(view, view->changed_data);
}

static void refresh_canvas_cursor(DocumentView *view) {
	Editor *editor = view->editor;
	gboolean hand = view->panning || (editor && (editor->space_held || !g_strcmp0(editor->tool, TOOL_HAND)));
	g_autoptr(GdkCursor) transforming = hand ? NULL : get_transform_cursor(view, view->pointer_x, view->pointer_y);
	if (transforming) {
		gtk_widget_set_cursor(view->canvas, transforming);
		return;
	}
	if (editor && !hand && is_selection_tool(editor->tool)) {
		double document_x, document_y;
		get_document_point(view, view->pointer_x, view->pointer_y, &document_x, &document_y);
		gboolean over = view->moving_selection ||
						(!view->selecting && selection_contains(view->presented->document, document_x, document_y));
		gtk_widget_set_cursor_from_name(view->canvas, over ? "move" : "crosshair");
		return;
	}
	if (editor && !hand && !g_strcmp0(editor->tool, TOOL_CROP)) {
		gtk_widget_set_cursor_from_name(view->canvas, "crosshair");
		return;
	}
	gtk_widget_set_cursor_from_name(view->canvas, hand ? (view->panning ? "grabbing" : "grab")
												  : editor && is_paint_tool(editor->tool) ? "none"
												  : editor && (!g_strcmp0(editor->tool, TOOL_EYEDROPPER) ||
															   is_fill_tool(editor->tool) ||
															   !g_strcmp0(editor->tool, PAINT_TOOL_GRADIENT))
													  ? "crosshair"
													  : NULL);
}

void refresh_view_tool(DocumentView *view) {
	if (!view)
		return;
	refresh_canvas_cursor(view);
	gtk_widget_queue_draw(view->canvas);
}

static void move_pointer(GtkEventControllerMotion *controller, double x, double y, DocumentView *view) {
	view->pointer_x = x;
	view->pointer_y = y;
	view->pointer_inside = TRUE;
	refresh_canvas_cursor(view);
	GdkModifierType modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
	if (view->editor)
		view->editor->shift_held = (modifiers & GDK_SHIFT_MASK) != 0;
	if (view->editor && is_paint_tool(view->editor->tool))
		gtk_widget_queue_draw(view->canvas);
	if (view->editor && !view->selecting)
		hover_selection_path(view, x, y, modifiers);
}

static void leave_canvas(GtkEventControllerMotion *controller, DocumentView *view) {
	view->pointer_inside = FALSE;
	gtk_widget_queue_draw(view->canvas);
}

void get_document_point(DocumentView *view, double x, double y, double *document_x, double *document_y) {
	double origin_x, origin_y;
	get_view_origin(view, &origin_x, &origin_y);
	*document_x = (x - origin_x) / view->zoom;
	*document_y = (y - origin_y) / view->zoom;
}

/* Each changed band redraws on its own, so a large tip redraws only the edge it moved */
static void update_paint_regions(Editor *editor, PresentedDocument *presented, PaintTool *tool) {
	const GArray *regions = get_paint_dirty_regions(tool);
	for (guint index = 0; index < regions->len; index++)
		update_document_region(editor, presented, g_array_index(regions, RasterExtent, index));
	clear_paint_dirty_regions(tool);
}

static gboolean paint_canvas_stroke(DocumentView *view, double x, double y, StrokePhase phase) {
	Editor *editor = view->editor;
	PresentedDocument *presented = view->presented;
	PaintTool *tool = &editor->paint_tool;
	double document_x, document_y;
	get_document_point(view, x, y, &document_x, &document_y);
	if (phase == STROKE_BEGIN || phase == STROKE_BEGIN_FROM_LAST) {
		g_autoptr(GError) error = NULL;
		PaintOptions options = *get_tool_options(editor);
		options.smoothing = options.smoothing * SMOOTHING_SCALE / view->zoom;
		guint32 color = get_foreground_color(editor);
		tool->wrap = view->tiled;
		tool->symmetry = editor->symmetry;
		if (!begin_paint_stroke(tool, presented->document, &options, color, document_x, document_y,
								phase == STROKE_BEGIN_FROM_LAST,
								&error)) {
			gtk_label_set_text(GTK_LABEL(editor->status), error->message);
			return FALSE;
		}
	} else if (phase == STROKE_MOVE || phase == STROKE_MOVE_ALONG_AXIS)
		continue_paint_stroke(tool, document_x, document_y, phase == STROKE_MOVE_ALONG_AXIS);
	else
		end_paint_stroke(tool, FALSE);
	update_paint_regions(editor, presented, tool);
	if (phase == STROKE_END)
		refresh_document_page(editor, get_current_page(editor), FALSE);
	return TRUE;
}

static void fill_canvas_area(DocumentView *view, double x, double y) {
	Editor *editor = view->editor;
	PaintTool *tool = &editor->paint_tool;
	double document_x, document_y;
	get_document_point(view, x, y, &document_x, &document_y);
	g_autoptr(GError) error = NULL;
	if (!fill_paint_area(tool, view->presented->document, get_fill_options(editor), get_foreground_color(editor),
						 document_x, document_y, &error)) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	update_paint_regions(editor, view->presented, tool);
	refresh_document_page(editor, get_current_page(editor), FALSE);
}

/* ============================== Gradient ============================== */

static void begin_gradient_drag(DocumentView *view, double x, double y) {
	g_autoptr(GError) error = NULL;
	if (!check_paint_target(view->presented->document, &error)) {
		gtk_label_set_text(GTK_LABEL(view->editor->status), error->message);
		return;
	}
	get_document_point(view, x, y, &view->gradient_x, &view->gradient_y);
	snap_view_point(view, &view->gradient_x, &view->gradient_y);
	view->snapping = FALSE;
	view->gradient_end_x = view->gradient_x;
	view->gradient_end_y = view->gradient_y;
	view->drawing_gradient = TRUE;
}

static void update_gradient_drag(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	get_document_point(view, x, y, &view->gradient_end_x, &view->gradient_end_y);
	snap_view_point(view, &view->gradient_end_x, &view->gradient_end_y);
	if (modifiers & GDK_SHIFT_MASK)
		constrain_to_eighth_turns(view->gradient_x, view->gradient_y, &view->gradient_end_x, &view->gradient_end_y);
	gtk_widget_queue_draw(view->canvas);
}

static void finish_gradient_drag(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	if (!view->drawing_gradient)
		return;
	update_gradient_drag(view, x, y, modifiers);
	view->drawing_gradient = FALSE;
	clear_view_snap(view);
	Editor *editor = view->editor;
	PaintTool *tool = &editor->paint_tool;
	g_autoptr(GError) error = NULL;
	if (!draw_gradient(tool, view->presented->document, &editor->gradient_options, get_foreground_color(editor),
					   get_background_color(editor), view->gradient_x, view->gradient_y, view->gradient_end_x,
					   view->gradient_end_y, &error)) {
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	update_paint_regions(editor, view->presented, tool);
	refresh_document_page(editor, get_current_page(editor), FALSE);
}

static void draw_gradient_line(GtkSnapshot *snapshot, DocumentView *view, double x, double y) {
	if (!view->drawing_gradient ||
		(view->gradient_x == view->gradient_end_x && view->gradient_y == view->gradient_end_y))
		return;
	double points[4] = {x + view->gradient_x * view->zoom, y + view->gradient_y * view->zoom,
						x + view->gradient_end_x * view->zoom, y + view->gradient_end_y * view->zoom};
	append_guide_line(snapshot, points[0], points[1], points[2], points[3], FALSE);
	const GdkRGBA white = {1, 1, 1, 1}, blue = {.3, .5, 1, 1};
	for (int point = 0; point < 4; point += 2) {
		gtk_snapshot_append_color(snapshot, &white,
								  &GRAPHENE_RECT_INIT(points[point] - 3, points[point + 1] - 3, 6, 6));
		gtk_snapshot_append_color(snapshot, &blue, &GRAPHENE_RECT_INIT(points[point] - 2, points[point + 1] - 2, 4, 4));
	}
}

static void finish_canvas_stroke(DocumentView *view) {
	if (!view->painting)
		return;
	view->painting = FALSE;
	paint_canvas_stroke(view, 0, 0, STROKE_END);
}

static void zoom_view_around(DocumentView *view, double x, double y, double zoom) {
	if (zoom == view->zoom)
		return;
	Document *document = view->presented->document;
	double document_x, document_y, origin_x, origin_y;
	get_document_point(view, x, y, &document_x, &document_y);
	get_view_origin(view, &origin_x, &origin_y);
	double anchor_x = origin_x + CLAMP(document_x, 0, document_width(document)) * view->zoom;
	double anchor_y = origin_y + CLAMP(document_y, 0, document_height(document)) * view->zoom;
	double center_x = gtk_widget_get_width(view->canvas) / 2.0, center_y = gtk_widget_get_height(view->canvas) / 2.0;
	double ratio = zoom / view->zoom;
	view->zoom = zoom;
	view->pan_x = (view->pan_x + center_x - anchor_x) * ratio + anchor_x - center_x;
	view->pan_y = (view->pan_y + center_y - anchor_y) * ratio + anchor_y - center_y;
	view->fitted = FALSE;
	notify_view_changed(view);
}

void step_view_zoom(DocumentView *view, gboolean in) {
	zoom_view_around(view, round(gtk_widget_get_width(view->canvas) / 2.0),
					 round(gtk_widget_get_height(view->canvas) / 2.0), step_zoom_level(view->zoom, in));
}

static gboolean zoom_document(GtkEventControllerScroll *controller, double dx, double dy, DocumentView *view) {
	/* Alt with the wheel sizes the brush, and with Shift changes its hardness */
	GdkModifierType modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
	if ((modifiers & GDK_ALT_MASK) && view->editor && is_paint_tool(view->editor->tool)) {
		double amount = dy != 0 ? dy : dx;
		if (amount != 0)
			step_brush(view->editor, amount < 0, (modifiers & GDK_SHIFT_MASK) != 0);
		return TRUE;
	}
	double zoom = view->zoom;
	if (gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_WHEEL) {
		if (view->wheel_zoom * dy < 0)
			view->wheel_zoom = 0;
		view->wheel_zoom += dy;
		if (fabs(view->wheel_zoom) < 1)
			return TRUE;
		zoom = step_zoom_level(zoom, view->wheel_zoom < 0);
		view->wheel_zoom -= view->wheel_zoom < 0 ? -1 : 1;
	} else {
		if (dy == 0)
			return TRUE;
		double factor = 1 + fabs(dy) * ZOOM_PER_WHEEL_UNIT;
		if (dy > 0)
			factor = 1 / factor;
		zoom = (modifiers & GDK_SHIFT_MASK) ? step_zoom_level(zoom, factor > 1) : zoom * factor;
		zoom = clamp_zoom_level(zoom);
	}
	zoom_view_around(view, view->pointer_x, view->pointer_y, zoom);
	return TRUE;
}

static void pick_canvas_color(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	double document_x, document_y;
	get_document_point(view, x, y, &document_x, &document_y);
	gboolean background = !g_strcmp0(view->editor->tool, TOOL_EYEDROPPER) && (modifiers & GDK_ALT_MASK);
	pick_editor_color(view->editor, view->presented->document, document_x, document_y, background);
}

/* ============================== Snapping ============================== */

void snap_view_box(DocumentView *view, double *x, double *y, double width, double height, const GArray *excluded,
				   LockedAxis locked_axis) {
	Editor *editor = view->editor;
	SnapResult result =
		snap_rectangle(view->presented->document, &editor->snap, *x, *y, width, height, view->zoom, excluded, NULL, 0);
	if (locked_axis == AXIS_VERTICAL) {
		result.offset_x = 0;
		result.line_x = NAN;
	}
	if (locked_axis == AXIS_HORIZONTAL) {
		result.offset_y = 0;
		result.line_y = NAN;
	}
	*x += round(result.offset_x);
	*y += round(result.offset_y);
	view->snapped = result;
	view->snapped_box = (RasterExtent){*x, *y, width, height};
	view->snapping = TRUE;
}

void snap_view_point(DocumentView *view, double *x, double *y) {
	SnapResult result = snap_points(view->presented->document, &view->editor->snap, x, y, 1, view->zoom, NULL, NULL, 0);
	*x += result.offset_x;
	*y += result.offset_y;
	view->snapped = result;
	view->snapped_box = (RasterExtent){*x, *y, 0, 0};
	view->snapping = TRUE;
}

void clear_view_snap(DocumentView *view) {
	view->snapping = FALSE;
	gtk_widget_queue_draw(view->canvas);
}

static void draw_snap_lines(GtkSnapshot *snapshot, DocumentView *view, double x, double y) {
	if (!view->snapping)
		return;
	const GdkRGBA color = {1, 0, 1, 1};
	SnapResult *snapped = &view->snapped;
	RasterExtent box = view->snapped_box;
	if (!isnan(snapped->line_x)) {
		double top = box.y, bottom = box.y + box.height;
		if (snapped->has_source_x) {
			top = MIN(top, snapped->source_y);
			bottom = MAX(bottom, snapped->source_y);
		}
		gtk_snapshot_append_color(snapshot, &color,
								  &GRAPHENE_RECT_INIT(floor(x + snapped->line_x * view->zoom), y + top * view->zoom, 1,
													  MAX(1, (bottom - top) * view->zoom)));
	}
	if (!isnan(snapped->line_y)) {
		double left = box.x, right = box.x + box.width;
		if (snapped->has_source_y) {
			left = MIN(left, snapped->source_x);
			right = MAX(right, snapped->source_x);
		}
		gtk_snapshot_append_color(snapshot, &color,
								  &GRAPHENE_RECT_INIT(x + left * view->zoom, floor(y + snapped->line_y * view->zoom),
													  MAX(1, (right - left) * view->zoom), 1));
	}
}

/* Painting tools draw with the primary button while Hand, Space and the middle button pan */
static void begin_drag(GtkGestureDrag *gesture, double x, double y, DocumentView *view) {
	gtk_widget_grab_focus(view->canvas);
	Editor *editor = view->editor;
	guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
	view->drag_x = view->pan_x;
	view->drag_y = view->pan_y;
	GdkModifierType modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
	gboolean primary = editor && button == GDK_BUTTON_PRIMARY && !editor->space_held;
	view->adjusting =
		editor && button == GDK_BUTTON_SECONDARY && (modifiers & GDK_ALT_MASK) && is_paint_tool(editor->tool);
	if (view->adjusting) {
		view->adjust_diameter = get_tool_options(editor)->shape.diameter;
		view->adjust_hardness = get_tool_options(editor)->shape.hardness;
		view->adjust_x = x;
		view->adjust_y = y;
		view->adjust_horizontal = TRUE;
		return;
	}
	if (editor && button == GDK_BUTTON_SECONDARY) {
		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
		show_canvas_menu(view, x, y);
		return;
	}
	view->picking =
		primary && (!g_strcmp0(editor->tool, TOOL_EYEDROPPER) ||
					((is_paint_tool(editor->tool) || is_fill_tool(editor->tool)) && (modifiers & GDK_ALT_MASK)));
	view->painting = primary && !view->picking && is_paint_tool(editor->tool);
	view->filling = primary && !view->picking && is_fill_tool(editor->tool);
	if (primary && press_replaces_crop(view, x, y))
		gtk_widget_queue_draw(view->canvas);
	if (primary && get_transform(view)) {
		view->transforming = TRUE;
		press_transform(view, x, y);
		return;
	}
	if (primary && is_selection_tool(editor->tool)) {
		begin_selection_drag(view, x, y, modifiers);
		return;
	}
	if (primary && !g_strcmp0(editor->tool, TOOL_CROP)) {
		get_document_point(view, x, y, &view->select_x, &view->select_y);
		snap_view_point(view, &view->select_x, &view->select_y);
		view->snapping = FALSE;
		view->cropping = TRUE;
		view->select_dragged = FALSE;
		view->select_preview = (RasterExtent){0};
		return;
	}
	if (primary && !g_strcmp0(editor->tool, TOOL_MOVE)) {
		begin_move_drag(view, x, y, modifiers);
		return;
	}
	if (primary && !g_strcmp0(editor->tool, PAINT_TOOL_GRADIENT)) {
		begin_gradient_drag(view, x, y);
		return;
	}
	view->panning = !view->painting && !view->picking && !view->filling;
	refresh_canvas_cursor(view);
	if (view->picking)
		pick_canvas_color(view, x, y, modifiers);
	if (!view->painting)
		return;
	view->painting =
		paint_canvas_stroke(view, x, y, (modifiers & GDK_SHIFT_MASK) ? STROKE_BEGIN_FROM_LAST : STROKE_BEGIN);
}

static void update_drag(GtkGestureDrag *gesture, double x, double y, DocumentView *view) {
	double start_x, start_y;
	gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
	if (view->adjusting) {
		double pointer_x = start_x + x, pointer_y = start_y + y;
		double across = pointer_x - view->adjust_x, down = pointer_y - view->adjust_y;
		gboolean horizontal = fabs(across) > fabs(down);
		if (horizontal && view->adjust_horizontal) {
			double change = round(DIAMETER_PER_PIXEL * across / view->zoom);
			view->adjust_diameter += change;
			pointer_x -= across - change / DIAMETER_PER_PIXEL * view->zoom;
		}
		if (!horizontal && !view->adjust_horizontal) {
			double change = round(down / PIXELS_PER_HARDNESS);
			view->adjust_hardness += change;
			pointer_y -= down - PIXELS_PER_HARDNESS * change;
		}
		view->adjust_diameter = MAX(1, view->adjust_diameter);
		view->adjust_hardness = CLAMP(view->adjust_hardness, 0, PERCENT);
		view->adjust_horizontal = horizontal;
		view->adjust_x = pointer_x;
		view->adjust_y = pointer_y;
		set_brush_shape(view->editor, view->adjust_diameter, view->adjust_hardness);
		PaintOptions *options = get_tool_options(view->editor);
		g_autofree char *values =
			g_strdup_printf(TEXT_BRUSH_ADJUST_STATUS, options->shape.diameter, options->shape.hardness);
		gtk_label_set_text(GTK_LABEL(view->editor->status), values);
		gtk_widget_queue_draw(view->canvas);
		return;
	}
	view->pointer_x = start_x + x;
	view->pointer_y = start_y + y;
	if (view->transforming) {
		view->pointer_x = start_x + x;
		view->pointer_y = start_y + y;
		drag_transform(view, start_x + x, start_y + y,
					   gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)));
		return;
	}
	if (view->cropping) {
		update_crop_drag(view, start_x + x, start_y + y,
						 gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)));
		return;
	}
	if (view->moving || view->marquee) {
		update_move_drag(view, start_x + x, start_y + y,
						 gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)));
		return;
	}
	if (view->wanding) {
		drag_wand_selection(view, start_x + x);
		return;
	}
	if (view->selecting || view->moving_selection) {
		update_selection_drag(view, start_x + x, start_y + y,
							  gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)));
		return;
	}
	if (view->drawing_gradient) {
		update_gradient_drag(view, start_x + x, start_y + y,
							 gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)));
		return;
	}
	if (view->picking) {
		pick_canvas_color(view, start_x + x, start_y + y,
						  gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)));
		return;
	}
	if (view->painting) {
		GdkModifierType modifiers = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
		paint_canvas_stroke(view, start_x + x, start_y + y,
							(modifiers & GDK_SHIFT_MASK) ? STROKE_MOVE_ALONG_AXIS : STROKE_MOVE);
		return;
	}
	if (!view->panning)
		return;
	view->pan_x = view->drag_x + x;
	view->pan_y = view->drag_y + y;
	view->fitted = FALSE;
	notify_view_changed(view);
}

static void end_drag(GtkGestureDrag *gesture, double x, double y, DocumentView *view) {
	if (view->transforming) {
		view->transforming = FALSE;
		release_transform(view);
	}
	if (view->cropping) {
		view->cropping = FALSE;
		clear_view_snap(view);
		finish_crop_drag(view);
		view->select_dragged = FALSE;
	}
	if (view->selecting || view->moving_selection) {
		double start_x, start_y;
		gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
		finish_selection_drag(view, start_x + x, start_y + y,
							  gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)));
	}
	finish_wand_selection(view);
	if (view->moving || view->marquee)
		finish_move_drag(view);
	double start_x, start_y;
	gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
	if (view->filling)
		fill_canvas_area(view, start_x + x, start_y + y);
	finish_gradient_drag(view, start_x + x, start_y + y,
						 gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)));
	view->picking = view->adjusting = view->filling = FALSE;
	finish_canvas_stroke(view);
	view->panning = FALSE;
	refresh_canvas_cursor(view);
}

static void cancel_drag(GtkGesture *gesture, GdkEventSequence *sequence, DocumentView *view) {
	if (view->moving || view->marquee)
		finish_move_drag(view);
	finish_wand_selection(view);
	view->adjusting = view->selecting = view->moving_selection = view->filling = view->drawing_gradient = FALSE;
	clear_view_snap(view);
	finish_canvas_stroke(view);
	view->panning = FALSE;
}

static void free_document_view(gpointer data) {
	DocumentView *view = data;
	g_clear_pointer(&view->lasso_points, g_array_unref);
	g_clear_pointer(&view->finished_polygon, g_array_unref);
	g_free(view);
}

GtkWidget *create_document_view(PresentedDocument *presented) {
	DocumentView *view = g_new0(DocumentView, 1);
	view->presented = presented;
	view->canvas = create_snapshot_area(draw_document, resize_view, view);
	gtk_widget_add_css_class(view->canvas, "canvas");
	view->zoom = 1;
	view->fitted = view->grid = TRUE;
	gtk_widget_set_hexpand(view->canvas, TRUE);
	gtk_widget_set_vexpand(view->canvas, TRUE);
	gtk_widget_set_focusable(view->canvas, TRUE);
	g_object_set_data_full(G_OBJECT(view->canvas), "document-view", view, free_document_view);
	GtkEventController *motion = gtk_event_controller_motion_new();
	g_signal_connect(motion, "motion", G_CALLBACK(move_pointer), view);
	g_signal_connect(motion, "leave", G_CALLBACK(leave_canvas), view);
	gtk_widget_add_controller(view->canvas, motion);
	GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
	g_signal_connect(scroll, "scroll", G_CALLBACK(zoom_document), view);
	gtk_widget_add_controller(view->canvas, scroll);
	GtkGesture *drag = gtk_gesture_drag_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);
	g_signal_connect(drag, "drag-begin", G_CALLBACK(begin_drag), view);
	g_signal_connect(drag, "drag-update", G_CALLBACK(update_drag), view);
	g_signal_connect(drag, "drag-end", G_CALLBACK(end_drag), view);
	g_signal_connect(drag, "cancel", G_CALLBACK(cancel_drag), view);
	gtk_widget_add_controller(view->canvas, GTK_EVENT_CONTROLLER(drag));
	return view->canvas;
}
