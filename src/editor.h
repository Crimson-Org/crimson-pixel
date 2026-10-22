#pragma once

#include <gtk/gtk.h>

#include "crop.h"
#include "document.h"
#include "paint.h"
#include "gradient.h"
#include "move-tool.h"
#include "snap.h"
#include "settings.h"
#include "selection-path.h"
#include "pixel-selection.h"

typedef struct TransformSession TransformSession;
#include "tiled-image.h"

/* A file request carries a decoded model and its derived presentation together */
typedef struct {
	Document *document;
	cairo_surface_t *surface;
	GFile *file;
	/* The format Save writes to the file, from find_file_format */
	const char *format;
	TiledImage *tiles;
	/* The selection outline as edge runs in mask coordinates, rebuilt when the mask changes */
	GArray *selection_edges;
	/* Below, moving and above parts drawn by the GPU while layers move */
	TiledImage *move_preview[3];
	/* Where the below and moving parts sit in the document; outside the below part the current display shows */
	RasterExtent below_extent, move_extent;
	TransformSession *transform;
	gboolean move_whole;
	gconstpointer selection_mask;
	/* Where the crash recovery copy of this document is kept, once it has needed one */
	char *recovery_path;
	guint recovered_revision;
	/* The tab position a closed document returns to when reopened */
	int closed_position;
} PresentedDocument;

typedef struct Editor Editor;

/* Docked panels in the order workspaces store them */
enum { PANEL_COLOR, PANEL_NAVIGATOR, PANEL_LAYERS, PANEL_SWATCHES, PANEL_COUNT };

/* Each native viewport owns independent zoom and pan around one shared document */
typedef struct DocumentView DocumentView;

struct DocumentView {
	GtkWidget *canvas;
	PresentedDocument *presented;
	Editor *editor;
	double zoom, pan_x, pan_y, pointer_x, pointer_y, drag_x, drag_y;
	/* Where the right click brush picker opened, which holds the brush outline while it is open */
	gboolean brush_anchored;
	double anchor_x, anchor_y;
	/* Wheel movement not yet worth a whole zoom step */
	double wheel_zoom;
	gboolean fitted, grid, tiled, pointer_inside, panning, painting, picking, filling, adjusting, selecting;
	gboolean moving_selection;
	double select_x, select_y, ants_offset;
	gboolean start_held;
	double start_offset_x, start_offset_y;
	int select_mode;
	gboolean select_shift, select_alt, select_dragged;
	/* The Magic Wand press point, the tolerance its drag started from and the history step it last made */
	gboolean wanding;
	double wand_x, wand_y, wand_start;
	int wand_base, wand_tolerance;
	Record *wand_entry;
	RasterExtent select_preview, moved_from;
	GArray *lasso_points, *finished_polygon;
	double path_x, path_y;
	gboolean path_button;
	Record *polygon_entry;
	gint64 polygon_release_time;
	ModifierTracker modifiers;
	guint ants_tick;
	MoveDrag move;
	/* The box being dragged and what it last snapped to, for the snap lines */
	RasterExtent move_box;
	SnapResult snapped;
	RasterExtent snapped_box;
	gboolean snapping;
	gboolean moving, marquee, transforming, cropping;
	gboolean drawing_gradient;
	double gradient_x, gradient_y, gradient_end_x, gradient_end_y;
	/* Where the Straighten line being drawn ends */
	double crop_end_x, crop_end_y;
	int move_axis;
	double adjust_diameter, adjust_hardness, adjust_x, adjust_y;
	gboolean adjust_horizontal;
	void (*changed)(DocumentView *view, gpointer data);
	gpointer changed_data;
};

/* Tabs own models while the shell owns pending cancellable loading requests and tool state */
struct Editor {
	/* File Open Recent and the start view list, refreshed when the recent files store changes */
	GMenu *recent_menu;
	GtkWidget *recent_list;
	GtkApplication *application;
	GtkWidget *window, *notebook, *status, *zoom_status;
	GtkWidget *navigator, *layer_list, *opacity, *fill, *dock;
	/* The Layers blend mode list, which starts with Pass Through while a group is active */
	GtkWidget *blend_mode;
	gboolean blend_mode_lists_pass, updating_blend_mode;
	gpointer panels[PANEL_COUNT];
	/* The swatch library of Swatch entries, kept in its ACO file */
	GArray *swatches;
	GtkWidget *swatch_grid, *swatch_empty;
	GMenu *workspace_menu;
	SnapSettings snap;
	Settings settings;
	GtkWidget *preferences;
	/* The last copy made here, used instead of the system clipboard while that still holds it */
	Record *clip_pixels;
	RasterExtent clip_extent;
	gconstpointer clip_document;
	char *workspace;
	GtkWidget *documents, *tool_column, *options_bar, *foreground, *background;
	GtkWidget *tool_size, *tool_hardness, *tool_opacity, *tool_flow;
	GtkWidget *hardness_row, *flow_row, *eraser_mode, *eyedropper_options, *sample_size, *sample_source;
	GtkWidget *transform_options, *rename_entry, *tip_shape;
	GtkWidget *pixel_perfect;
	/* Native painting symmetry shared by Brush, Pencil and Eraser, from the SYMMETRY bits */
	int symmetry;
	GtkWidget *fill_options, *fill_opacity, *fill_tolerance, *fill_smooth, *fill_contiguous, *fill_all_layers;
	char *layer_rows;
	int eye_layer;
	GPtrArray *requests, *tool_buttons;
	/* Closed documents with their history, newest last, that Reopen Closed Document brings back */
	GPtrArray *closed_documents;
	const char *tool;
	int remembered_tools[32];
	PaintTool paint_tool;
	PaintOptions brush_options, pencil_options, eraser_options;
	FillOptions bucket_options, magic_eraser_options, wand_options;
	GradientOptions gradient_options;
	GtkWidget *gradient_bar, *gradient_type, *gradient_opacity;
	gboolean move_auto_select, move_select_groups;
	GtkWidget *move_options;
	gboolean ellipse_anti_alias, lasso_anti_alias, polygon_anti_alias;
	GtkWidget *selection_options, *selection_anti_alias;
	CropOptions crop_options;
	gboolean straightening;
	GtkWidget *crop_bar;
	gboolean closing, close_prompt, space_held, shift_held;
};

void free_presented_document(PresentedDocument *presented);
GtkWidget *create_document_view(PresentedDocument *presented);
void fit_document_view(DocumentView *view);
void notify_view_changed(DocumentView *view);
void get_view_origin(DocumentView *view, double *x, double *y);
void show_actual_pixels(DocumentView *view);
void step_view_zoom(DocumentView *view, gboolean in);
void create_editor_document(Editor *editor, int width, int height, unsigned depth, DocumentMode mode, const char *name);
void open_editor_file(Editor *editor, GFile *file);
void open_recovered_file(Editor *editor, GFile *file);
void start_document_recovery(Editor *editor);
void stop_document_recovery(void);
void adopt_recovered_document(Editor *editor, GFile *file, PresentedDocument *presented);
void forget_recovery_file(PresentedDocument *presented);
void install_document(Editor *editor, PresentedDocument *presented);
void reopen_closed_document(Editor *editor);
GtkWidget *add_loading_page(Editor *editor, const char *name, GCancellable *loading);
void finish_loading_page(Editor *editor, GtkWidget *page, PresentedDocument *presented);
void show_open_dialog(Editor *editor);
void show_new_dialog(Editor *editor);
void show_preferences(Editor *editor);
void show_image_size_dialog(Editor *editor);
void show_canvas_size_dialog(Editor *editor);
void show_trim_dialog(Editor *editor);
void trim_editor_document(Editor *editor, TrimColor color, const gboolean sides[4]);
void reveal_editor_document(Editor *editor);
void apply_settings(Editor *editor);
gboolean is_view_smoothed(const DocumentView *view);
void snapshot_checkerboard(GtkSnapshot *snapshot, const graphene_rect_t *area, int colors, int cell);
void show_export_dialog(Editor *editor);
const char *find_file_format(GFile *file, gboolean saved_in_place);
void save_editor_document(Editor *editor, gboolean choose_file);
void export_editor_document(Editor *editor, GFile *file);
void build_editor_window(Editor *editor);
GtkWidget *create_start_view(Editor *editor);
void focus_start_view(GtkWidget *window, Editor *editor);
void remember_recent_file(GFile *file);
void forget_recent_file(const char *uri);
void clear_recent_files(void);
void open_recent_uri(Editor *editor, const char *uri);
GMenuModel *create_recent_menu(Editor *editor);
GtkWidget *create_recent_list(Editor *editor);
GtkWidget *create_tool_column(Editor *editor);
GtkWidget *create_options_bar(Editor *editor);
void initialize_editor_tools(Editor *editor);
void select_editor_tool(Editor *editor, const char *identifier);
gboolean press_tool_key(Editor *editor, guint key, GdkModifierType modifiers);
gboolean is_paint_tool(const char *identifier);
PaintOptions *get_tool_options(Editor *editor);
gboolean is_fill_tool(const char *identifier);
gboolean is_selection_tool(const char *identifier);
gboolean *get_selection_anti_alias(Editor *editor);
gboolean is_selection_path_tool(const char *identifier);
SelectionMode get_selection_mode(gboolean shift, gboolean alt);
gboolean is_drawing_selection_path(const DocumentView *view);
void start_selection_path(DocumentView *view);
void drag_selection_path(DocumentView *view, double x, double y, GdkModifierType modifiers);
void hover_selection_path(DocumentView *view, double x, double y, GdkModifierType modifiers);
void release_selection_path(DocumentView *view, double x, double y, GdkModifierType modifiers);
gboolean press_selection_path_key(Editor *editor, guint key, GdkModifierType modifiers, gboolean pressed);
void cancel_selection_paths(Editor *editor);
void finish_ellipse_selection(DocumentView *view);
void draw_selection_path_preview(GtkSnapshot *snapshot, DocumentView *view, double x, double y);
void get_document_point(DocumentView *view, double x, double y, double *document_x, double *document_y);
void refresh_tool_options(Editor *editor);
FillOptions *get_fill_options(Editor *editor);
void set_brush_shape(Editor *editor, double diameter, double hardness);
gboolean uses_soft_tip(Editor *editor);
void show_brush_popup(DocumentView *view, double x, double y);
void step_brush(Editor *editor, gboolean larger, gboolean hardness);
int type_percent_digit(int current, int digit);
void type_layer_opacity_digit(Editor *editor, int digit);
void step_layer_blend_mode(Editor *editor, gboolean forward);
void queue_canvas_redraw(Editor *editor);
void refresh_selection_display(DocumentView *view);
void nudge_with_move_tool(Editor *editor, int x, int y, gboolean copy);
void nudge_selection(Editor *editor, int x, int y);
void refresh_view_tool(DocumentView *view);
void snap_view_box(DocumentView *view, double *x, double *y, double width, double height, const GArray *excluded,
				   int locked_axis);
void snap_view_point(DocumentView *view, double *x, double *y);
void clear_view_snap(DocumentView *view);
TransformSession *get_transform(DocumentView *view);
TransformSession *editor_transform(Editor *editor);
void start_transform(Editor *editor, gboolean selection_only);
void start_crop(Editor *editor, RasterExtent rectangle);
void start_crop_tool(Editor *editor);
void finish_transform_for_tool_change(Editor *editor);
gboolean press_replaces_crop(DocumentView *view, double x, double y);
void crop_to_selection(Editor *editor);
void configure_crop(Editor *editor);
void start_straighten(Editor *editor);
void update_crop_drag(DocumentView *view, double x, double y, GdkModifierType modifiers);
void finish_crop_drag(DocumentView *view);
void draw_crop_line(DocumentView *view, GtkSnapshot *snapshot);
GtkWidget *create_crop_options(Editor *editor);
void finish_transform(Editor *editor, gboolean apply);
void draw_transform(DocumentView *view, GtkSnapshot *snapshot, double x, double y, const graphene_rect_t *image,
					const graphene_rect_t *visible);
void draw_transform_box(DocumentView *view, GtkSnapshot *snapshot);
void press_transform(DocumentView *view, double x, double y);
void drag_transform(DocumentView *view, double x, double y, GdkModifierType modifiers);
void release_transform(DocumentView *view);
GdkCursor *get_transform_cursor(DocumentView *view, double x, double y);
guint32 get_foreground_color(Editor *editor);
guint32 get_background_color(Editor *editor);
guint32 read_color_button(GtkWidget *button);
void write_color_button(GtkWidget *button, guint32 color);
void add_dialog_input(GtkWidget *grid, const char *name, GtkWidget *input, int row);
void show_modify_dialog(Editor *editor, int modification);
void show_fill_dialog(Editor *editor);
void show_stroke_dialog(Editor *editor);
void show_adjustment_panel(Editor *editor, int layer_index);
gboolean get_layer_row_bounds(Editor *editor, int layer_index, GtkWidget *relative, GdkRectangle *bounds);
GMenuModel *create_adjustment_menu(void);
void fill_with_editor_color(Editor *editor, gboolean background);
void set_editor_color(Editor *editor, guint32 color, gboolean background);
void pick_editor_color(Editor *editor, Document *document, double x, double y, gboolean background);
void update_document_region(Editor *editor, PresentedDocument *presented, RasterExtent extent);
gboolean update_surface_region(Document *document, cairo_surface_t *surface, RasterExtent extent, GError **error);

GtkWidget *get_current_page(Editor *editor);
PresentedDocument *get_page_document(GtkWidget *page);
DocumentView *get_page_view(GtkWidget *page, const char *name);
void refresh_document_page(Editor *editor, GtkWidget *page, gboolean render);
void refresh_loading_state(Editor *editor);
void refresh_document_status(Editor *editor, Document *document);
GtkWidget *create_editor_dock(Editor *editor, GtkWidget *center);
void create_workspace(Editor *editor, GtkWidget *center);
void add_workspace_actions(Editor *editor);
GMenuModel *create_window_menu(Editor *editor);
GMenu *create_modify_menu(void);
GMenuModel *create_menu_model(Editor *editor);
void set_menu_shortcuts(Editor *editor);
void show_context_menu(GtkWidget *parent, GMenuModel *model, double x, double y);
void remember_workspace(Editor *editor);
void refresh_editor_dock(Editor *editor);
GtkWidget *create_layers_panel(Editor *editor);
GtkWidget *create_swatches_panel(Editor *editor);
gboolean open_swatch_file(Editor *editor, GFile *file);
void refresh_layers_panel(Editor *editor);
void refresh_history_actions(Editor *editor, GtkWidget *page);
void refresh_editor_actions(Editor *editor);
void refresh_layer_menu_actions(Editor *editor);
void watch_layer_rename(Editor *editor);
void copy_to_clipboard(Editor *editor, gboolean cut, gboolean merged);
void paste_from_clipboard(Editor *editor);
void clear_selected_area(Editor *editor);
void edit_document_history(Editor *editor, gboolean redo);
gboolean confirm_editor_close(Editor *editor, GtkWidget *page);
void close_editor_page(Editor *editor, GtkWidget *page);
