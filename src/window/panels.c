#include <math.h>

#include <adwaita.h>
#include <libpanel.h>

#include "editor.h"
#include "color/color-spaces.h"
#include "canvas/snapshot-area.h"
#include "lang/en.h"

static const int NAVIGATOR_HEIGHT = 104;
#define NAVIGATOR_FRAME_WIDTH 2
/* The navigator leaves this much room around the document, and shows its checkerboard in cells this size */
#define NAVIGATOR_MARGIN 12.0
#define NAVIGATOR_WELL_ALPHA 0.06
#define NAVIGATOR_WELL_RADIUS 6
#define SMALL_CHECKER 4
/* The square and the hue strip mark their colour with the same ring, which keeps this much colour between itself
 * and the field's edge, and the hue strip's height limit just under a full turn */
#define RING_RADIUS 5
#define RING_ROOM 3
#define LAST_HUE 0.9999
#define COLOR_PANEL_HEIGHT 100
/* The colour square and hue strip round their corners like the controls around them, with a faint edge */
#define COLOR_CORNER_RADIUS 6
#define COLOR_EDGE_ALPHA 0.15

/* Where the whole document fits inside the navigator, on whole pixels so the overview and its frame stay sharp */
static graphene_rect_t get_navigator_image(GtkWidget *area, Document *document) {
	int width = gtk_widget_get_width(area), height = gtk_widget_get_height(area);
	double scale = MIN((width - NAVIGATOR_MARGIN) / document_width(document),
					   (height - NAVIGATOR_MARGIN) / document_height(document));
	double image_width = MAX(0, round(document_width(document) * scale));
	double image_height = MAX(0, round(document_height(document) * scale));
	return GRAPHENE_RECT_INIT(floor((width - image_width) / 2), floor((height - image_height) / 2), image_width,
							  image_height);
}

/* The overview sits in a soft rounded well that fills the panel, so the picture is framed whatever its shape */
static void draw_navigator_well(GtkWidget *area, GtkSnapshot *snapshot, int width, int height) {
	GdkRGBA well;
	gtk_widget_get_color(area, &well);
	well.alpha *= NAVIGATOR_WELL_ALPHA;
	GskRoundedRect box;
	gsk_rounded_rect_init_from_rect(&box, &GRAPHENE_RECT_INIT(0, 0, width, height), NAVIGATOR_WELL_RADIUS);
	gtk_snapshot_push_rounded_clip(snapshot, &box);
	gtk_snapshot_append_color(snapshot, &well, &box.bounds);
	gtk_snapshot_pop(snapshot);
}

static void draw_navigator(GtkWidget *area, GtkSnapshot *snapshot, int width, int height, gpointer data) {
	Editor *editor = data;
	draw_navigator_well(area, snapshot, width, height);
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || !presented->tiles)
		return;
	Document *document = presented->document;
	graphene_rect_t image = get_navigator_image(area, document);
	if (image.size.width <= 0 || image.size.height <= 0)
		return;
	double scale_x = image.size.width / document_width(document);
	double scale_y = image.size.height / document_height(document);
	snapshot_checkerboard(snapshot, &image, editor->settings.checker_colors, SMALL_CHECKER);
	snapshot_tiled_image(presented->tiles, snapshot, &image, &image, scale_x, TRUE, FALSE);
	/* The frame marks the part of the document the primary view shows */
	DocumentView *view = get_page_view(page, "primary");
	double origin_x, origin_y;
	get_view_origin(view, &origin_x, &origin_y);
	double left = MAX(0, -origin_x / view->zoom), top = MAX(0, -origin_y / view->zoom);
	double right = MIN(document_width(document), (gtk_widget_get_width(view->canvas) - origin_x) / view->zoom);
	double bottom = MIN(document_height(document), (gtk_widget_get_height(view->canvas) - origin_y) / view->zoom);
	if (right <= left || bottom <= top)
		return;
	double frame_left = round(image.origin.x + left * scale_x), frame_top = round(image.origin.y + top * scale_y);
	double frame_right = round(image.origin.x + right * scale_x);
	double frame_bottom = round(image.origin.y + bottom * scale_y);
	/* The frame takes the accent colour, like every other mark of what is current */
	g_autoptr(GdkRGBA) accent = adw_style_manager_get_accent_color_rgba(adw_style_manager_get_default());
	append_outline(snapshot,
				   GRAPHENE_RECT_INIT(frame_left, frame_top, frame_right - frame_left, frame_bottom - frame_top), 0,
				   NAVIGATOR_FRAME_WIDTH, accent);
}

static void center_primary_view(Editor *editor, double x, double y) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented)
		return;
	Document *document = presented->document;
	graphene_rect_t image = get_navigator_image(editor->navigator, document);
	if (image.size.width <= 0 || image.size.height <= 0)
		return;
	DocumentView *view = get_page_view(page, "primary");
	double center_x = CLAMP((x - image.origin.x) / image.size.width, 0, 1) * document_width(document);
	double center_y = CLAMP((y - image.origin.y) / image.size.height, 0, 1) * document_height(document);
	view->fitted = FALSE;
	view->pan_x = (document_width(document) / 2.0 - center_x) * view->zoom;
	view->pan_y = (document_height(document) / 2.0 - center_y) * view->zoom;
	notify_view_changed(view);
}

static void press_navigator(GtkGestureDrag *gesture, double x, double y, Editor *editor) {
	center_primary_view(editor, x, y);
}

static void drag_navigator(GtkGestureDrag *gesture, double x, double y, Editor *editor) {
	double start_x, start_y;
	gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
	center_primary_view(editor, start_x + x, start_y + y);
}

/* ============================== Color panel ============================== */

typedef struct {
	Editor *editor;
	double hue, saturation, value;
	gboolean picking_hue, writing;
} ColorPanel;

enum { HUE_STRIP = 18, HUE_GAP = 10 };

static guint32 hsv_to_color(double hue, double saturation, double value) {
	double rgb[RGB_CHANNELS];
	convert_hsb(hue, saturation, value, rgb);
	return rgba_to_color(&(GdkRGBA){rgb[0], rgb[1], rgb[2], 1});
}

/* Grey colors keep the previous hue so the square does not jump to red */
static void color_to_hsv(guint32 color, ColorPanel *panel) {
	GdkRGBA rgba = color_to_rgba(color, 1);
	double hsv[RGB_CHANNELS];
	convert_rgb_to_hsv(rgba.red, rgba.green, rgba.blue, hsv);
	panel->value = hsv[2];
	panel->saturation = hsv[1];
	if (hsv[1] > 0)
		panel->hue = hsv[0];
}

guint32 rgba_to_color(const GdkRGBA *rgba) {
	return (guint32)round(rgba->red * G_MAXUINT8) << WORD_RED_SHIFT |
		   (guint32)round(rgba->green * G_MAXUINT8) << WORD_GREEN_SHIFT | (guint32)round(rgba->blue * G_MAXUINT8);
}

GdkRGBA color_to_rgba(guint32 color, double alpha) {
	return (GdkRGBA){(color >> WORD_RED_SHIFT & G_MAXUINT8) / 255.0, (color >> WORD_GREEN_SHIFT & G_MAXUINT8) / 255.0,
					 (color & G_MAXUINT8) / 255.0, alpha};
}

static void draw_ring(GtkSnapshot *snapshot, double x, double y) {
	const GdkRGBA rings[] = {{1, 1, 1, 1}, {0, 0, 0, 0.8}};
	for (guint ring = 0; ring < G_N_ELEMENTS(rings); ring++) {
		double size = RING_RADIUS + 1 - ring;
		append_outline(snapshot, square_around(x, y, size), size, 1, &rings[ring]);
	}
}

/* The ring's centre travels along a field of this length inset by the ring and its room, so the whole ring always
 * stays inside the field */
static const double RING_MARGIN = RING_RADIUS + 1 + RING_ROOM;

static double place_ring(double fraction, double length) {
	return RING_MARGIN + fraction * MAX(0, length - 2 * RING_MARGIN);
}

static double read_ring(double position, double length) {
	double travel = length - 2 * RING_MARGIN;
	return travel > 0 ? CLAMP((position - RING_MARGIN) / travel, 0, 1) : 0;
}

/* Draws inside a rounded box until the matching pop, so gradients get the same corners as other controls. The box
 * masks everything drawn inside as one picture, so a colour under the gradients never shows at the smoothed corners */
static void push_rounded_box(GtkSnapshot *snapshot, const graphene_rect_t *box) {
	const GdkRGBA opaque = {0, 0, 0, 1};
	GskRoundedRect rounded;
	gsk_rounded_rect_init_from_rect(&rounded, box, COLOR_CORNER_RADIUS);
	gtk_snapshot_push_mask(snapshot, GSK_MASK_MODE_ALPHA);
	gtk_snapshot_push_rounded_clip(snapshot, &rounded);
	gtk_snapshot_append_color(snapshot, &opaque, box);
	gtk_snapshot_pop(snapshot);
	gtk_snapshot_pop(snapshot);
}

static void draw_color_edge(GtkSnapshot *snapshot, graphene_rect_t box) {
	const GdkRGBA edge = {0, 0, 0, COLOR_EDGE_ALPHA};
	append_outline(snapshot, box, COLOR_CORNER_RADIUS, 1, &edge);
}

static void draw_color_panel(GtkWidget *widget, GtkSnapshot *snapshot, int width, int height, gpointer data) {
	ColorPanel *panel = data;
	double square = width - HUE_STRIP - HUE_GAP;
	if (square <= 0 || height <= 0)
		return;
	graphene_rect_t area = GRAPHENE_RECT_INIT(0, 0, square, height);
	push_rounded_box(snapshot, &area);
	GdkRGBA hue = color_to_rgba(hsv_to_color(panel->hue, 1, 1), 1);
	gtk_snapshot_append_color(snapshot, &hue, &area);
	/* The gradients run along the rings' travel and keep their end colours beyond it, so a ring always sits on
	 * the colour it marks */
	const GskColorStop whites[] = {{0, {1, 1, 1, 1}}, {1, {1, 1, 1, 0}}};
	const GskColorStop blacks[] = {{0, {0, 0, 0, 0}}, {1, {0, 0, 0, 1}}};
	gtk_snapshot_append_linear_gradient(snapshot, &area, &GRAPHENE_POINT_INIT(place_ring(0, square), 0),
										&GRAPHENE_POINT_INIT(place_ring(1, square), 0), whites, G_N_ELEMENTS(whites));
	gtk_snapshot_append_linear_gradient(snapshot, &area, &GRAPHENE_POINT_INIT(0, place_ring(0, height)),
										&GRAPHENE_POINT_INIT(0, place_ring(1, height)), blacks, G_N_ELEMENTS(blacks));
	gtk_snapshot_pop(snapshot);
	draw_color_edge(snapshot, area);
	/* The strip runs once around the hues, with a stop at each sector's start and the first hue again at the end */
	GskColorStop spectrum[HUE_SECTORS + 1];
	for (int stop = 0; stop <= HUE_SECTORS; stop++)
		spectrum[stop] = (GskColorStop){(float)stop / HUE_SECTORS,
										color_to_rgba(hsv_to_color((double)stop / HUE_SECTORS, 1, 1), 1)};
	graphene_rect_t strip = GRAPHENE_RECT_INIT(width - HUE_STRIP, 0, HUE_STRIP, height);
	push_rounded_box(snapshot, &strip);
	gtk_snapshot_append_linear_gradient(snapshot, &strip, &GRAPHENE_POINT_INIT(0, place_ring(0, height)),
										&GRAPHENE_POINT_INIT(0, place_ring(1, height)), spectrum,
										G_N_ELEMENTS(spectrum));
	gtk_snapshot_pop(snapshot);
	draw_color_edge(snapshot, strip);
	draw_ring(snapshot, place_ring(panel->saturation, square), place_ring(1 - panel->value, height));
	draw_ring(snapshot, width - HUE_STRIP / 2.0, place_ring(panel->hue / LAST_HUE, height));
}

static void pick_panel_color(GtkWidget *area, ColorPanel *panel, double x, double y) {
	int width = gtk_widget_get_width(area), height = gtk_widget_get_height(area);
	double square = width - HUE_STRIP - HUE_GAP;
	if (square <= 0 || height <= 0)
		return;
	if (panel->picking_hue)
		panel->hue = read_ring(y, height) * LAST_HUE;
	else {
		panel->saturation = read_ring(x, square);
		panel->value = 1 - read_ring(y, height);
	}
	panel->writing = TRUE;
	set_editor_color(panel->editor, hsv_to_color(panel->hue, panel->saturation, panel->value), FALSE);
	panel->writing = FALSE;
	gtk_widget_queue_draw(area);
}

static void press_color_panel(GtkGestureDrag *gesture, double x, double y, ColorPanel *panel) {
	GtkWidget *area = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	panel->picking_hue = x >= gtk_widget_get_width(area) - HUE_STRIP - HUE_GAP / 2.0;
	pick_panel_color(area, panel, x, y);
}

static void drag_color_panel(GtkGestureDrag *gesture, double x, double y, ColorPanel *panel) {
	double start_x, start_y;
	gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
	pick_panel_color(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)), panel, start_x + x, start_y + y);
}

/* Colors chosen elsewhere, such as the Eyedropper, move the markers too */
static void follow_foreground(GObject *button, GParamSpec *property, GtkWidget *area) {
	ColorPanel *panel = g_object_get_data(G_OBJECT(area), "color-panel");
	if (panel->writing)
		return;
	color_to_hsv(get_foreground_color(panel->editor), panel);
	gtk_widget_queue_draw(area);
}

static GtkWidget *create_color_panel(Editor *editor) {
	ColorPanel *panel = g_new0(ColorPanel, 1);
	panel->editor = editor;
	GtkWidget *area = create_snapshot_area(draw_color_panel, NULL, panel);
	g_object_set_data_full(G_OBJECT(area), "color-panel", panel, g_free);
	color_to_hsv(get_foreground_color(editor), panel);
	gtk_widget_set_size_request(area, -1, COLOR_PANEL_HEIGHT);
	gtk_widget_set_vexpand(area, FALSE);
	gtk_widget_set_tooltip_text(area, TEXT_COLOR_HELP);
	gtk_widget_set_cursor_from_name(area, "crosshair");
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(press_color_panel), panel);
	g_signal_connect(drag, "drag-update", G_CALLBACK(drag_color_panel), panel);
	gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));
	g_signal_connect_object(editor->foreground, "notify::rgba", G_CALLBACK(follow_foreground), area, 0);
	return area;
}

/* ============================== Dock ============================== */

/* Panels outlive any one dock, so workspaces can rebuild the dock and closed panels can return. A panel has no
 * icon, so its tab in the frame header shows its name */
static PanelWidget *create_panel(const char *id, const char *title, GtkWidget *content) {
	PanelWidget *panel = PANEL_WIDGET(g_object_ref_sink(panel_widget_new()));
	panel_widget_set_id(panel, id);
	panel_widget_set_title(panel, title);
	gtk_widget_add_css_class(content, "panel-content");
	panel_widget_set_child(panel, content);
	/* The dock gives spare height only to panels whose content grows, so fixed panels stay at their natural size */
	gtk_widget_set_vexpand(GTK_WIDGET(panel), gtk_widget_compute_expand(content, GTK_ORIENTATION_VERTICAL));
	return panel;
}

/* A quiet line that tells an empty panel has nothing to show yet, centred in the space the content would fill */
GtkWidget *create_panel_note(const char *text) {
	GtkWidget *note = gtk_label_new(text);
	gtk_widget_add_css_class(note, "panel-note");
	gtk_widget_set_halign(note, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(note, GTK_ALIGN_CENTER);
	gtk_widget_set_can_target(note, FALSE);
	return note;
}

/* The overview's well keeps its shape with no document open, and says so */
static GtkWidget *create_navigator_panel(Editor *editor) {
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), editor->navigator);
	editor->navigator_empty = create_panel_note(TEXT_NO_DOCUMENT);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), editor->navigator_empty);
	return overlay;
}

/* libpanel's dock lets panels be dragged into tabs, split into new frames and moved between dock edges */
GtkWidget *create_editor_dock(Editor *editor, GtkWidget *center) {
	editor->navigator = create_snapshot_area(draw_navigator, NULL, editor);
	gtk_widget_set_size_request(editor->navigator, -1, NAVIGATOR_HEIGHT);
	gtk_widget_set_vexpand(editor->navigator, FALSE);
	gtk_widget_set_tooltip_text(editor->navigator, TEXT_NAVIGATOR_HELP);
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(press_navigator), editor);
	g_signal_connect(drag, "drag-update", G_CALLBACK(drag_navigator), editor);
	gtk_widget_add_controller(editor->navigator, GTK_EVENT_CONTROLLER(drag));
	GtkWidget *layers = create_layers_panel(editor);
	editor->panels[PANEL_COLOR] = create_panel("color", TEXT_COLOR, create_color_panel(editor));
	editor->panels[PANEL_NAVIGATOR] = create_panel("navigator", TEXT_NAVIGATOR, create_navigator_panel(editor));
	editor->panels[PANEL_LAYERS] = create_panel("layers", TEXT_LAYERS, layers);
	editor->panels[PANEL_SWATCHES] = create_panel("swatches", TEXT_SWATCHES, create_swatches_panel(editor));
	create_workspace(editor, center);
	refresh_editor_dock(editor);
	return editor->dock;
}

void refresh_editor_dock(Editor *editor) {
	gtk_widget_queue_draw(editor->navigator);
	gboolean open = get_page_document(get_current_page(editor)) != NULL;
	gtk_revealer_set_reveal_child(GTK_REVEALER(editor->layer_controls), open);
	gtk_revealer_set_reveal_child(GTK_REVEALER(editor->layer_actions), open);
	gtk_widget_set_visible(editor->navigator_empty, !open);
	gtk_widget_set_visible(editor->layers_empty, !open);
	refresh_layers_panel(editor);
}
