#pragma once

#include "document.h"
#include "stroke.h"
#include "magic-wand.h"

#define PAINT_TOOL_BRUSH "23"
#define PAINT_TOOL_PENCIL "24"
#define PAINT_TOOL_ERASER "31"
#define PAINT_TOOL_MAGIC_ERASER "33"
#define PAINT_TOOL_BUCKET "35"
#define PAINT_TOOL_GRADIENT "34"
#define TOOL_EYEDROPPER "14"
#define TOOL_RECTANGLE_SELECT "1"
#define TOOL_ELLIPSE_SELECT "2"
#define TOOL_LASSO "5"
#define TOOL_POLYGONAL_LASSO "6"
#define TOOL_MAGIC_WAND "9"
#define TOOL_MOVE "0"
#define TOOL_CROP "10"

/* Native painting symmetry: a vertical axis mirrors left and right, a horizontal one top and bottom */
enum { SYMMETRY_VERTICAL = 1, SYMMETRY_HORIZONTAL = 2 };

typedef struct {
	BrushShape shape;
	double opacity, flow, smoothing;
	int eraser_mode;
	/* Native pixel art options: a square tip and pixel perfect one pixel lines for hard tips */
	gboolean smooth_edges, square, pixel_perfect;
} PaintOptions;

typedef struct {
	double opacity;
	WandSettings wand;
	gboolean all_layers;
} FillOptions;

typedef struct {
	const char *tool;
	Document *document;
	Stroke *stroke;
	Record *backup;
	RasterExtent backup_extent;
	/* The regions changed since the display last took them */
	GArray *dirty;
	double opacity, last_x, last_y, anchor_x, anchor_y;
	int axis;
	gboolean has_last, coverage_masked;
	/* Native wrap painting while Tile Preview shows: the stroke covers three by three copies of the image, offset so
	 * the press falls in the middle one, and maps back onto the image */
	gboolean wrap;
	double wrap_x, wrap_y;
	/* Native painting symmetry mirrors the stroke across the middle of the image, and the copies and mirror images
	 * merge into one image sized coverage */
	int symmetry;
	Record *merged;
	PaintOptions options;
	RandomSource random;
	gpointer random_data;
} PaintTool;

void reset_paint_tool(PaintTool *tool, const char *identifier);
gboolean begin_paint_stroke(PaintTool *tool, Document *document, const PaintOptions *options, guint32 color, double x,
							double y, gboolean line_from_last, GError **error);
void continue_paint_stroke(PaintTool *tool, double x, double y, gboolean constrain);
void end_paint_stroke(PaintTool *tool, gboolean extended_history);
gboolean fill_paint_area(PaintTool *tool, Document *document, const FillOptions *options, guint32 color, double x,
						 double y, GError **error);
gboolean draw_paint_bitmap(PaintTool *tool, Document *document, Record *pixels, RasterExtent extent, GError **error);
gboolean check_paint_target(Document *document, GError **error);
gboolean fill_paint_region(PaintTool *tool, Document *document, const Record *selection, guint32 color, int alpha,
						   gboolean preserve_transparency, int name_group, int name_item, GError **error);
void cancel_paint_tool(PaintTool *tool);
const GArray *get_paint_dirty_regions(PaintTool *tool);
void clear_paint_dirty_regions(PaintTool *tool);
guint32 sample_document_color(Document *document, double x, double y, int size, gboolean active_layer);
void prepare_document_for_save(Document *document);
Document *copy_document_for_save(const Document *document);
void trim_layer(Record *layer);
