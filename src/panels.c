#include <math.h>

#include <libpanel.h>

#include "editor.h"
#include "snapshot-area.h"
#include "../lang/en.h"

static const int NAVIGATOR_HEIGHT = 120;

/* The overview scale and offset that fit the whole document inside the navigator */
static double get_navigator_scale(GtkWidget *area, Document *document, double *x, double *y) {
	int width = gtk_widget_get_width(area), height = gtk_widget_get_height(area);
	double scale = MIN((width - 16.0) / document_width(document), (height - 16.0) / document_height(document));
	*x = (width - document_width(document) * scale) / 2;
	*y = (height - document_height(document) * scale) / 2;
	return scale;
}

static void draw_navigator(GtkWidget *area, GtkSnapshot *snapshot, int width, int height, gpointer data) {
	Editor *editor = data;
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || !presented->tiles)
		return;
	Document *document = presented->document;
	double x, y, scale = get_navigator_scale(area, document, &x, &y);
	if (scale <= 0)
		return;
	graphene_rect_t image =
		GRAPHENE_RECT_INIT(x, y, document_width(document) * scale, document_height(document) * scale);
	snapshot_checkerboard(snapshot, &image, editor->settings.checker_colors, 4);
	snapshot_tiled_image(presented->tiles, snapshot, &image, &image, scale, TRUE, FALSE);
	/* The frame marks the part of the document the primary view shows */
	DocumentView *view = get_page_view(page, "primary");
	double origin_x, origin_y;
	get_view_origin(view, &origin_x, &origin_y);
	double left = MAX(0, -origin_x / view->zoom), top = MAX(0, -origin_y / view->zoom);
	double right = MIN(document_width(document), (gtk_widget_get_width(view->canvas) - origin_x) / view->zoom);
	double bottom = MIN(document_height(document), (gtk_widget_get_height(view->canvas) - origin_y) / view->zoom);
	if (right <= left || bottom <= top)
		return;
	GskRoundedRect frame;
	gsk_rounded_rect_init_from_rect(
		&frame, &GRAPHENE_RECT_INIT(x + left * scale, y + top * scale, (right - left) * scale, (bottom - top) * scale),
		0);
	const GdkRGBA red = {0.85, 0.1, 0.1, 1};
	gtk_snapshot_append_border(snapshot, &frame, (const float[4]){2, 2, 2, 2}, (const GdkRGBA[4]){red, red, red, red});
}

static void center_primary_view(Editor *editor, double x, double y) {
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented)
		return;
	Document *document = presented->document;
	double offset_x, offset_y, scale = get_navigator_scale(editor->navigator, document, &offset_x, &offset_y);
	if (scale <= 0)
		return;
	DocumentView *view = get_page_view(page, "primary");
	double center_x = CLAMP((x - offset_x) / scale, 0, document_width(document));
	double center_y = CLAMP((y - offset_y) / scale, 0, document_height(document));
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
	double sector = fmod(hue, 1) * 6, fraction = sector - floor(sector);
	double p = value * (1 - saturation), q = value * (1 - saturation * fraction);
	double t = value * (1 - saturation * (1 - fraction));
	double rgb[6][3] = {{value, t, p}, {q, value, p}, {p, value, t}, {p, q, value}, {t, p, value}, {value, p, q}};
	double *channel = rgb[(int)floor(sector) % 6];
	return (guint32)lround(channel[0] * 255) << 16 | (guint32)lround(channel[1] * 255) << 8 |
		   (guint32)lround(channel[2] * 255);
}

/* Grey colors keep the previous hue so the square does not jump to red */
static void color_to_hsv(guint32 color, ColorPanel *panel) {
	double red = (color >> 16 & 255) / 255.0, green = (color >> 8 & 255) / 255.0, blue = (color & 255) / 255.0;
	double maximum = MAX(red, MAX(green, blue)), minimum = MIN(red, MIN(green, blue)), range = maximum - minimum;
	panel->value = maximum;
	panel->saturation = maximum > 0 ? range / maximum : 0;
	if (range <= 0)
		return;
	double hue = maximum == red		? (green - blue) / range
				 : maximum == green ? 2 + (blue - red) / range
									: 4 + (red - green) / range;
	panel->hue = fmod(hue / 6 + 1, 1);
}

static GdkRGBA color_to_rgba(guint32 color, double alpha) {
	return (GdkRGBA){(color >> 16 & 255) / 255.0, (color >> 8 & 255) / 255.0, (color & 255) / 255.0, alpha};
}

static void draw_ring(GtkSnapshot *snapshot, double x, double y, double radius) {
	const GdkRGBA rings[2] = {{1, 1, 1, 1}, {0, 0, 0, 0.8}};
	for (int ring = 0; ring < 2; ring++) {
		double size = radius + 1 - ring;
		GskRoundedRect outline;
		gsk_rounded_rect_init_from_rect(&outline, &GRAPHENE_RECT_INIT(x - size, y - size, size * 2, size * 2), size);
		GdkRGBA color = rings[ring];
		gtk_snapshot_append_border(snapshot, &outline, (const float[4]){1, 1, 1, 1},
								   (const GdkRGBA[4]){color, color, color, color});
	}
}

static void draw_color_panel(GtkWidget *widget, GtkSnapshot *snapshot, int width, int height, gpointer data) {
	ColorPanel *panel = data;
	double square = width - HUE_STRIP - HUE_GAP;
	if (square <= 0 || height <= 0)
		return;
	graphene_rect_t area = GRAPHENE_RECT_INIT(0, 0, square, height);
	GdkRGBA hue = color_to_rgba(hsv_to_color(panel->hue, 1, 1), 1);
	gtk_snapshot_append_color(snapshot, &hue, &area);
	const GskColorStop whites[2] = {{0, {1, 1, 1, 1}}, {1, {1, 1, 1, 0}}};
	const GskColorStop blacks[2] = {{0, {0, 0, 0, 0}}, {1, {0, 0, 0, 1}}};
	gtk_snapshot_append_linear_gradient(snapshot, &area, &GRAPHENE_POINT_INIT(0, 0), &GRAPHENE_POINT_INIT(square, 0),
										whites, 2);
	gtk_snapshot_append_linear_gradient(snapshot, &area, &GRAPHENE_POINT_INIT(0, 0), &GRAPHENE_POINT_INIT(0, height),
										blacks, 2);
	GskColorStop spectrum[7];
	for (int stop = 0; stop < 7; stop++)
		spectrum[stop] = (GskColorStop){stop / 6.0f, color_to_rgba(hsv_to_color(stop / 6.0, 1, 1), 1)};
	graphene_rect_t strip = GRAPHENE_RECT_INIT(width - HUE_STRIP, 0, HUE_STRIP, height);
	gtk_snapshot_append_linear_gradient(snapshot, &strip, &GRAPHENE_POINT_INIT(0, 0), &GRAPHENE_POINT_INIT(0, height),
										spectrum, 7);
	draw_ring(snapshot, panel->saturation * square, (1 - panel->value) * height, 5);
	double marker = panel->hue * height;
	const GdkRGBA white = {1, 1, 1, 1}, black = {0, 0, 0, 0.8};
	gtk_snapshot_append_color(snapshot, &black,
							  &GRAPHENE_RECT_INIT(width - HUE_STRIP - 2, marker - 2, HUE_STRIP + 4, 4));
	gtk_snapshot_append_color(snapshot, &white,
							  &GRAPHENE_RECT_INIT(width - HUE_STRIP - 1, marker - 1, HUE_STRIP + 2, 2));
}

static void pick_panel_color(GtkWidget *area, ColorPanel *panel, double x, double y) {
	int width = gtk_widget_get_width(area), height = gtk_widget_get_height(area);
	double square = width - HUE_STRIP - HUE_GAP;
	if (square <= 0 || height <= 0)
		return;
	if (panel->picking_hue)
		panel->hue = CLAMP(y / height, 0, 0.9999);
	else {
		panel->saturation = CLAMP(x / square, 0, 1);
		panel->value = 1 - CLAMP(y / height, 0, 1);
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
	gtk_widget_set_size_request(area, -1, 110);
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

/* Panels outlive any one dock, so workspaces can rebuild the dock and closed panels can return */
static PanelWidget *create_panel(const char *id, const char *title, const char *icon, GtkWidget *content) {
	PanelWidget *panel = PANEL_WIDGET(g_object_ref_sink(panel_widget_new()));
	panel_widget_set_id(panel, id);
	panel_widget_set_title(panel, title);
	panel_widget_set_icon_name(panel, icon);
	gtk_widget_set_margin_start(content, 8);
	gtk_widget_set_margin_end(content, 8);
	gtk_widget_set_margin_top(content, 8);
	gtk_widget_set_margin_bottom(content, 8);
	panel_widget_set_child(panel, content);
	/* The dock gives spare height only to panels whose content grows, so fixed panels stay at their natural size */
	gtk_widget_set_vexpand(GTK_WIDGET(panel), gtk_widget_compute_expand(content, GTK_ORIENTATION_VERTICAL));
	return panel;
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
	editor->panels[PANEL_COLOR] =
		create_panel("color", TEXT_COLOR, "color-select-symbolic", create_color_panel(editor));
	editor->panels[PANEL_NAVIGATOR] =
		create_panel("navigator", TEXT_NAVIGATOR, "find-location-symbolic", editor->navigator);
	editor->panels[PANEL_LAYERS] = create_panel("layers", TEXT_LAYERS, "view-paged-symbolic", layers);
	editor->panels[PANEL_SWATCHES] =
		create_panel("swatches", TEXT_SWATCHES, "view-app-grid-symbolic", create_swatches_panel(editor));
	create_workspace(editor, center);
	refresh_editor_dock(editor);
	return editor->dock;
}

void refresh_editor_dock(Editor *editor) {
	gtk_widget_queue_draw(editor->navigator);
	refresh_layers_panel(editor);
}
