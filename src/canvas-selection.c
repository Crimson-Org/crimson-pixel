#include <math.h>

#include "canvas-tools.h"
#include "pixel-selection.h"
#include "layer-commands.h"
#include "history.h"
#include "transform.h"
#include "../lang/en.h"

/* One horizontal or vertical run of boundary between selected and unselected pixels */
typedef struct {
	int x, y, length;
	gboolean vertical;
} SelectionEdge;

static GArray *build_selection_edges(const Document *document, const Record *selection) {
	Record *mask = get_record(selection, "C");
	RasterExtent extent = read_extent(get_record(selection, "V"));
	double half = channel_maximum(get_number(document->record, "depth")) / 2;
	int width = extent.width, height = extent.height, stride = width + 2;
	g_autofree guint8 *inside = g_malloc0((gsize)stride * (height + 2));
	gboolean bytes = mask->value.buffer.depth == 8 && mask->value.buffer.length >= (gsize)width * height;
	const guint8 *values = mask->value.buffer.data;
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++)
			inside[(gsize)(y + 1) * stride + x + 1] =
				bytes ? values[(gsize)y * width + x] > half : read_channel(mask, (gsize)y * width + x) > half;
	GArray *edges = g_array_new(FALSE, FALSE, sizeof(SelectionEdge));
	for (int y = 0; y <= height; y++) {
		const guint8 *above = inside + (gsize)y * stride + 1, *below = above + stride;
		for (int x = 0, start = -1; x <= width; x++) {
			gboolean edge = x < width && above[x] != below[x];
			if (edge && start < 0)
				start = x;
			if (!edge && start >= 0) {
				g_array_append_val(edges, ((SelectionEdge){start, y, x - start, FALSE}));
				start = -1;
			}
		}
	}
	for (int x = 0; x <= width; x++)
		for (int y = 0, start = -1; y <= height; y++) {
			gboolean edge =
				y < height && inside[(gsize)(y + 1) * stride + x] != inside[(gsize)(y + 1) * stride + x + 1];
			if (edge && start < 0)
				start = y;
			if (!edge && start >= 0) {
				g_array_append_val(edges, ((SelectionEdge){x, start, y - start, TRUE}));
				start = -1;
			}
		}
	return edges;
}

/* Each run is a one pixel line repeating a four pixel black and white dash, shifted to march */
static void draw_edge(GtkSnapshot *snapshot, graphene_rect_t bounds, gboolean vertical, double offset,
					  const graphene_rect_t *visible) {
	graphene_rect_t shown;
	if (!graphene_rect_intersection(&bounds, visible, &shown))
		return;
	const GdkRGBA black = {0, 0, 0, 1}, white = {1, 1, 1, 1};
	double shift = fmod(offset, 8);
	graphene_rect_t tile = vertical ? GRAPHENE_RECT_INIT(bounds.origin.x, bounds.origin.y + shift, 1, 8)
									: GRAPHENE_RECT_INIT(bounds.origin.x + shift, bounds.origin.y, 8, 1);
	gtk_snapshot_push_repeat(snapshot, &shown, &tile);
	gtk_snapshot_append_color(snapshot, &white, &tile);
	gtk_snapshot_append_color(snapshot, &black,
							  vertical ? &GRAPHENE_RECT_INIT(tile.origin.x, tile.origin.y, 1, 4)
									   : &GRAPHENE_RECT_INIT(tile.origin.x, tile.origin.y, 4, 1));
	gtk_snapshot_pop(snapshot);
}

static void draw_edges(GtkSnapshot *snapshot, GArray *edges, double x, double y, double zoom, double offset,
					   const graphene_rect_t *visible) {
	for (guint index = 0; index < edges->len; index++) {
		SelectionEdge *edge = &g_array_index(edges, SelectionEdge, index);
		double left = floor(x + edge->x * zoom), top = floor(y + edge->y * zoom);
		double length = round(edge->length * zoom);
		graphene_rect_t bounds = edge->vertical
									 ? GRAPHENE_RECT_INIT(MIN(left, x + edge->x * zoom - 1), top, 1, MAX(1, length))
									 : GRAPHENE_RECT_INIT(left, MIN(top, y + edge->y * zoom - 1), MAX(1, length), 1);
		draw_edge(snapshot, bounds, edge->vertical, offset, visible);
	}
}

/* Marching ants around the selection, and the rectangle being dragged */
void draw_selection(GtkSnapshot *snapshot, DocumentView *view, double x, double y, const graphene_rect_t *visible) {
	PresentedDocument *presented = view->presented;
	Record *selection = get_document_selection(presented->document);
	if (selection) {
		Record *mask = get_record(selection, "C");
		if (presented->selection_mask != mask || !presented->selection_edges) {
			g_clear_pointer(&presented->selection_edges, g_array_unref);
			presented->selection_edges = build_selection_edges(presented->document, selection);
			presented->selection_mask = mask;
		}
		RasterExtent extent = read_extent(get_record(selection, "V"));
		draw_edges(snapshot, presented->selection_edges, x + extent.x * view->zoom, y + extent.y * view->zoom,
				   view->zoom, view->ants_offset, visible);
	}
	if (view->editor)
		draw_selection_path_preview(snapshot, view, x, y);
	gboolean rectangle = view->selecting && view->editor && !g_strcmp0(view->editor->tool, TOOL_RECTANGLE_SELECT);
	if ((rectangle || view->marquee || view->cropping) && view->select_dragged && !empty_extent(view->select_preview)) {
		RasterExtent preview = view->select_preview;
		g_autoptr(GArray) edges = g_array_new(FALSE, FALSE, sizeof(SelectionEdge));
		g_array_append_val(edges, ((SelectionEdge){0, 0, preview.width, FALSE}));
		g_array_append_val(edges, ((SelectionEdge){0, preview.height, preview.width, FALSE}));
		g_array_append_val(edges, ((SelectionEdge){0, 0, preview.height, TRUE}));
		g_array_append_val(edges, ((SelectionEdge){preview.width, 0, preview.height, TRUE}));
		draw_edges(snapshot, edges, x + preview.x * view->zoom, y + preview.y * view->zoom, view->zoom,
				   view->ants_offset, visible);
	}
}

/* The ants march while a selection exists, at a pace that costs one small redraw every tenth of a second */
static gboolean march_ants(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
	DocumentView *view = data;
	gboolean active = get_document_selection(view->presented->document) || view->selecting || view->lasso_points;
	if (!active) {
		view->ants_tick = 0;
		return G_SOURCE_REMOVE;
	}
	double offset = floor(gdk_frame_clock_get_frame_time(clock) / 100000.0) * 2;
	if (offset != view->ants_offset) {
		view->ants_offset = offset;
		gtk_widget_queue_draw(widget);
	}
	return G_SOURCE_CONTINUE;
}

void refresh_selection_display(DocumentView *view) {
	if (!view->ants_tick &&
		(get_document_selection(view->presented->document) || view->selecting || view->lasso_points))
		view->ants_tick = gtk_widget_add_tick_callback(view->canvas, march_ants, view, NULL);
	gtk_widget_queue_draw(view->canvas);
}

/* ============================== Rectangle Select ============================== */

static void select_with_wand(DocumentView *view, int tolerance) {
	Editor *editor = view->editor;
	Document *document = view->presented->document;
	WandSettings settings = editor->wand_options.wand;
	settings.tolerance = tolerance;
	view->wand_tolerance = tolerance;
	if (view->wand_entry && get_latest_history_entry(document) == view->wand_entry)
		undo_document_command(document);
	g_autoptr(GError) error = NULL;
	gboolean changed = select_similar_area(document, view->wand_x, view->wand_y, settings,
										   editor->wand_options.all_layers, view->select_mode, FALSE, &error);
	if (error)
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
	view->wand_entry = changed ? get_latest_history_entry(document) : NULL;
	refresh_selection_display(view);
	refresh_document_page(editor, get_current_page(editor), FALSE);
}

static void start_wand_selection(DocumentView *view, double x, double y) {
	view->wanding = TRUE;
	view->wand_start = x;
	view->wand_entry = NULL;
	view->wand_base = view->editor->wand_options.wand.tolerance;
	get_document_point(view, x, y, &view->wand_x, &view->wand_y);
	select_with_wand(view, view->wand_base);
}

void drag_wand_selection(DocumentView *view, double x) {
	int tolerance = CLAMP((int)round_like_source(view->wand_base + (x - view->wand_start) / 4), 0, 255);
	if (tolerance == view->wand_tolerance)
		return;
	select_with_wand(view, tolerance);
	view->editor->wand_options.wand.tolerance = tolerance;
	refresh_tool_options(view->editor);
}

void finish_wand_selection(DocumentView *view) {
	if (!view->wanding)
		return;
	view->wanding = FALSE;
	view->editor->wand_options.wand.tolerance = view->wand_base;
	refresh_tool_options(view->editor);
}

void begin_selection_drag(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	Document *document = view->presented->document;
	if (is_drawing_selection_path(view)) {
		view->selecting = view->path_button = TRUE;
		return;
	}
	get_document_point(view, x, y, &view->select_x, &view->select_y);
	if (!selection_contains(view->presented->document, view->select_x, view->select_y)) {
		snap_view_point(view, &view->select_x, &view->select_y);
		view->snapping = FALSE;
	}
	view->select_shift = (modifiers & GDK_SHIFT_MASK) != 0;
	view->select_alt = (modifiers & GDK_ALT_MASK) != 0;
	view->select_mode = get_selection_mode(view->select_shift, view->select_alt);
	begin_modifier_tracking(&view->modifiers, view->select_shift, view->select_alt);
	view->select_dragged = view->start_held = FALSE;
	view->select_preview = (RasterExtent){0};
	if (!view->select_shift && !view->select_alt && selection_contains(document, view->select_x, view->select_y)) {
		view->moving_selection = TRUE;
		view->moved_from = get_selection_extent(document);
		return;
	}
	if (!g_strcmp0(view->editor->tool, TOOL_MAGIC_WAND)) {
		start_wand_selection(view, x, y);
		return;
	}
	if (is_selection_path_tool(view->editor->tool)) {
		start_selection_path(view);
		return;
	}
	view->selecting = TRUE;
	refresh_selection_display(view);
}

void update_selection_drag(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	Document *document = view->presented->document;
	double document_x, document_y;
	get_document_point(view, x, y, &document_x, &document_y);
	if (is_selection_path_tool(view->editor->tool) && !view->moving_selection) {
		drag_selection_path(view, document_x, document_y, modifiers);
		return;
	}
	update_modifier_tracking(&view->modifiers, (modifiers & GDK_SHIFT_MASK) != 0, (modifiers & GDK_ALT_MASK) != 0);
	if (hypot(document_x - view->select_x, document_y - view->select_y) * view->zoom > 5)
		view->select_dragged = view->modifiers.dragged = TRUE;
	if (view->editor->space_held) {
		if (!view->start_held) {
			view->start_offset_x = document_x - view->select_x;
			view->start_offset_y = document_y - view->select_y;
		}
		view->select_x = document_x - view->start_offset_x;
		view->select_y = document_y - view->start_offset_y;
	}
	view->start_held = view->editor->space_held;
	if (!view->select_dragged)
		return;
	if (view->moving_selection) {
		double box_x = view->moved_from.x + round(document_x - view->select_x);
		double box_y = view->moved_from.y + round(document_y - view->select_y);
		g_autoptr(GArray) excluded = g_array_new(FALSE, FALSE, sizeof(int));
		Record *selected = get_record(document->record, "j");
		for (guint index = 0; index < count_records(selected); index++) {
			int value = get_item(selected, index)->value.number;
			g_array_append_val(excluded, value);
		}
		snap_view_box(view, &box_x, &box_y, view->moved_from.width, view->moved_from.height, excluded, 0);
		move_selection(document, (int)box_x, (int)box_y, FALSE);
		gtk_widget_queue_draw(view->canvas);
		return;
	}
	snap_view_point(view, &document_x, &document_y);
	gboolean constrained = !get_document_selection(document) || was_shift_pressed_again(&view->modifiers) ||
						   was_alt_pressed_again(&view->modifiers);
	gboolean square = constrained && view->modifiers.shift, centered = constrained && view->modifiers.alt;
	view->select_preview =
		constrain_selection_drag(view->select_x, view->select_y, document_x, document_y, square, centered);
	RasterExtent canvas = {0, 0, document_width(document), document_height(document)};
	if (!g_strcmp0(view->editor->tool, TOOL_RECTANGLE_SELECT))
		view->select_preview = intersect_region(view->select_preview, canvas);
	gtk_widget_queue_draw(view->canvas);
}

void finish_selection_drag(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	Document *document = view->presented->document;
	clear_view_snap(view);
	gboolean moving = view->moving_selection;
	if (!moving && is_selection_path_tool(view->editor->tool)) {
		double document_x, document_y;
		get_document_point(view, x, y, &document_x, &document_y);
		release_selection_path(view, document_x, document_y, modifiers);
		return;
	}
	view->moving_selection = view->selecting = FALSE;
	if (moving && view->select_dragged) {
		refresh_document_page(view->editor, get_current_page(view->editor), FALSE);
		return;
	}
	if (!view->select_dragged)
		deselect(document, FALSE);
	else if (!g_strcmp0(view->editor->tool, TOOL_ELLIPSE_SELECT))
		finish_ellipse_selection(view);
	else
		select_rectangle(document, view->select_preview, view->select_mode, FALSE);
	refresh_selection_display(view);
	refresh_document_page(view->editor, get_current_page(view->editor), FALSE);
}

void show_canvas_menu(DocumentView *view, double x, double y) {
	Editor *editor = view->editor;
	if (is_paint_tool(editor->tool)) {
		show_brush_popup(view, x, y);
		return;
	}
	if (!is_selection_tool(editor->tool))
		return;
	GMenu *menu = g_menu_new(), *selection = g_menu_new(), *layers = g_menu_new();
	g_menu_append(selection, TEXT_DESELECT, "win.deselect");
	g_menu_append(selection, TEXT_INVERSE, "win.inverse");
	GMenu *modify = create_modify_menu();
	g_menu_append_submenu(selection, TEXT_MENU_MODIFY, G_MENU_MODEL(modify));
	g_object_unref(modify);
	g_menu_append(layers, TEXT_LAYER_VIA_COPY, "win.layer-via-copy");
	g_menu_append(layers, TEXT_LAYER_VIA_CUT, "win.layer-via-cut");
	g_menu_append(layers, TEXT_NEW_LAYER, "win.new-layer");
	GMenu *transforms = g_menu_new();
	g_menu_append(transforms, TEXT_FREE_TRANSFORM, "win.free-transform");
	g_menu_append(transforms, TEXT_TRANSFORM_SELECTION, "win.transform-selection");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(selection));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(layers));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(transforms));
	g_object_unref(transforms);
	GMenu *painting = g_menu_new();
	g_menu_append(painting, TEXT_FILL_ITEM, "win.fill");
	g_menu_append(painting, TEXT_STROKE_ITEM, "win.stroke");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(painting));
	g_object_unref(painting);
	g_object_unref(selection);
	g_object_unref(layers);
	refresh_editor_actions(editor);
	show_context_menu(view->canvas, G_MENU_MODEL(menu), x, y);
	g_object_unref(menu);
}

void nudge_selection(Editor *editor, int x, int y) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = page ? get_page_document(page) : NULL;
	DocumentView *primary = page ? get_page_view(page, "primary") : NULL;
	if (!presented || presented->transform || primary->selecting || primary->moving_selection)
		return;
	Record *selection = get_document_selection(presented->document);
	if (!selection)
		return;
	RasterExtent extent = read_extent(get_record(selection, "V"));
	move_selection(presented->document, extent.x + x, extent.y + y, FALSE);
	refresh_selection_display(primary);
	refresh_selection_display(get_page_view(page, "secondary"));
	refresh_document_page(editor, page, FALSE);
}
