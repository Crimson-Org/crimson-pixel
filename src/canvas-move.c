#include <math.h>

#include "canvas-tools.h"
#include "pixel-selection.h"
#include "layer-commands.h"
#include "history.h"
#include "transform.h"
#include "../lang/en.h"

/* ============================== Move ============================== */

/* Renders the parts below, moving and above once, so the drag itself only moves a GPU texture */
static void start_move_preview(DocumentView *view) {
	PresentedDocument *presented = view->presented;
	Document *document = presented->document;
	cairo_surface_t *parts[3];
	if (!render_move_preview(document, view->move.layers, view->move.mode == 1 ? view->move.floating : NULL, &parts[0],
							 &parts[1], &parts[2], &presented->below_extent, &presented->move_extent)) {
		for (int part = 0; part < 3; part++)
			if (parts[part])
				cairo_surface_destroy(parts[part]);
		return;
	}
	presented->move_whole = !parts[1];
	for (int part = 0; part < 3; part++) {
		presented->move_preview[part] = parts[part] ? create_tiled_image(parts[part]) : NULL;
		if (parts[part])
			cairo_surface_destroy(parts[part]);
	}
	view->move.deferred = TRUE;
	document->changed = (RasterExtent){0};
}

/* The real composite is made once for everything the move touched */
static void stop_move_preview(DocumentView *view) {
	PresentedDocument *presented = view->presented;
	Document *document = presented->document;
	gboolean previewing = presented->move_preview[1] != NULL || presented->move_whole;
	presented->move_whole = FALSE;
	for (int part = 0; part < 3; part++)
		g_clear_pointer(&presented->move_preview[part], free_tiled_image);
	if (previewing && !empty_extent(document->changed))
		update_document_region(view->editor, presented, document->changed);
	document->changed = (RasterExtent){0};
	gtk_widget_queue_draw(view->canvas);
}

void begin_move_drag(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	Editor *editor = view->editor;
	GtkWidget *page = get_current_page(editor);
	Document *document = view->presented->document;
	get_document_point(view, x, y, &view->select_x, &view->select_y);
	gboolean inside = selection_contains(document, view->select_x, view->select_y);
	gboolean shift = (modifiers & GDK_SHIFT_MASK) != 0, alt = (modifiers & GDK_ALT_MASK) != 0;
	view->move_axis = 0;
	view->select_dragged = FALSE;
	view->select_preview = (RasterExtent){0};
	g_autoptr(GError) error = NULL;
	gboolean auto_select = editor->move_auto_select || (modifiers & GDK_CONTROL_MASK);
	if (!inside && auto_select) {
		if (find_layer_at(document, view->select_x, view->select_y) < 0 && !shift) {
			view->marquee = TRUE;
			return;
		}
		pick_layer_at(document, view->select_x, view->select_y, shift, editor->move_select_groups);
	}
	if (!inside && alt) {
		if (!duplicate_selected_layers(document, FALSE, &error) && error) {
			gtk_label_set_text(GTK_LABEL(editor->status), error->message);
			return;
		}
		refresh_document_page(editor, page, TRUE);
	}
	document->changed = (RasterExtent){0};
	if (!begin_move(&view->move, document, inside, alt, FALSE, &error)) {
		if (error)
			gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		refresh_document_page(editor, page, FALSE);
		return;
	}
	view->moving = TRUE;
	view->move_box = get_transform_bounds(document, view->move.mode == 1 ? TRANSFORM_PIXELS : TRANSFORM_LAYERS);
	if (!empty_extent(document->changed))
		update_document_region(editor, view->presented, document->changed);
	document->changed = (RasterExtent){0};
	start_move_preview(view);
	refresh_selection_display(view);
	refresh_document_page(editor, page, FALSE);
}

void update_move_drag(DocumentView *view, double x, double y, GdkModifierType modifiers) {
	Document *document = view->presented->document;
	double document_x, document_y;
	get_document_point(view, x, y, &document_x, &document_y);
	double across = document_x - view->select_x, down = document_y - view->select_y;
	if (view->marquee) {
		view->select_dragged = TRUE;
		view->select_preview =
			constrain_selection_drag(view->select_x, view->select_y, document_x, document_y, FALSE, FALSE);
		gtk_widget_queue_draw(view->canvas);
		return;
	}
	if (!view->move_axis && (across || down))
		view->move_axis = fabs(across) < fabs(down) ? 1 : 2;
	if (modifiers & GDK_SHIFT_MASK) {
		if (view->move_axis == 1)
			across = 0;
		if (view->move_axis == 2)
			down = 0;
	}
	double box_x = view->move_box.x + round(across), box_y = view->move_box.y + round(down);
	snap_view_box(view, &box_x, &box_y, view->move_box.width, view->move_box.height, view->move.layers,
				  (modifiers & GDK_SHIFT_MASK) ? view->move_axis : 0);
	across = box_x - view->move_box.x;
	down = box_y - view->move_box.y;
	gboolean previewing = view->presented->move_preview[1] || view->presented->move_whole;
	if (!previewing)
		document->changed = (RasterExtent){0};
	update_move(&view->move, document, (int)round(across), (int)round(down));
	/* With a preview the GPU shows the move, and the document is composed once on release */
	if (previewing)
		gtk_widget_queue_draw(view->canvas);
	else {
		if (!empty_extent(document->changed))
			update_document_region(view->editor, view->presented, document->changed);
		document->changed = (RasterExtent){0};
	}
	if (view->move.mode == 1)
		refresh_selection_display(view);
}

void finish_move_drag(DocumentView *view) {
	Document *document = view->presented->document;
	if (view->marquee) {
		view->marquee = FALSE;
		if (view->select_dragged)
			select_layers_in_rectangle(document, view->select_preview);
		view->select_dragged = FALSE;
		gtk_widget_queue_draw(view->canvas);
	} else if (view->moving) {
		view->moving = FALSE;
		finish_move(&view->move, document, FALSE);
		stop_move_preview(view);
	}
	clear_view_snap(view);
	refresh_document_page(view->editor, get_current_page(view->editor), FALSE);
}

void nudge_with_move_tool(Editor *editor, int x, int y, gboolean copy) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = page ? get_page_document(page) : NULL;
	DocumentView *primary = page ? get_page_view(page, "primary") : NULL;
	if (!presented || presented->transform || primary->moving || primary->marquee)
		return;
	Document *document = presented->document;
	gboolean selection = get_document_selection(document) != NULL;
	g_autoptr(GError) error = NULL;
	if (copy && !selection && !duplicate_selected_layers(document, FALSE, &error)) {
		if (error)
			gtk_label_set_text(GTK_LABEL(editor->status), error->message);
		return;
	}
	document->changed = (RasterExtent){0};
	MoveDrag drag = {0};
	if (begin_move(&drag, document, selection, copy, FALSE, &error)) {
		update_move(&drag, document, x, y);
		finish_move(&drag, document, FALSE);
	} else if (error)
		gtk_label_set_text(GTK_LABEL(editor->status), error->message);
	if (!empty_extent(document->changed))
		update_document_region(editor, presented, document->changed);
	document->changed = (RasterExtent){0};
	refresh_selection_display(primary);
	refresh_selection_display(get_page_view(page, "secondary"));
	refresh_document_page(editor, page, copy && !selection);
}
