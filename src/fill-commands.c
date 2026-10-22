#include <math.h>

#include "fill-commands.h"
#include "paint.h"
#include "pixel-selection.h"
#include "selection-modify.h"

#define TOOL_STROKE_TARGET "31"

guint32 get_fill_content_color(FillContent content, guint32 foreground, guint32 background, guint32 custom) {
	switch (content) {
	case FILL_FOREGROUND:
		return foreground;
	case FILL_BACKGROUND:
		return background;
	case FILL_COLOR:
		return custom;
	case FILL_BLACK:
		return 0;
	case FILL_GRAY:
		return 0x808080;
	case FILL_WHITE:
		return 0xffffff;
	}
	return foreground;
}

gboolean fill_document(Document *document, const char *operation, guint32 color, int opacity_percent,
					   gboolean preserve_transparency, GError **error) {
	PaintTool tool = {.tool = operation};
	int alpha = round_like_source(255 * (opacity_percent / 100.0));
	gboolean filled = fill_paint_region(&tool, document, get_document_selection(document), color, alpha,
										preserve_transparency, 2, 3, error);
	cancel_paint_tool(&tool);
	return filled;
}

static Record *create_layer_alpha_selection(Document *document) {
	Record *layer = get_active_layer(document), *pixels = get_record(layer, "buffer");
	RasterExtent extent = read_extent(get_record(layer, "V"));
	gsize count = (gsize)MAX(0, extent.width) * MAX(0, extent.height);
	Record *mask = create_channels(count, 8);
	for (gsize index = 0; index < count; index++)
		write_channel(mask, index, read_channel(pixels, index * 4 + 3));
	return create_selection(extent, mask);
}

gboolean stroke_document(Document *document, guint32 color, int width, StrokeLocation location, int opacity_percent,
						 GError **error) {
	if (!check_paint_target(document, error))
		return FALSE;
	Record *selection = get_document_selection(document);
	g_autoptr(Record) alpha_selection = selection ? NULL : create_layer_alpha_selection(document);
	double inner = location == STROKE_INSIDE ? width : location == STROKE_CENTER ? width / 2.0 : 0;
	double outer = location == STROKE_OUTSIDE ? width : location == STROKE_CENTER ? width / 2.0 : 0;
	g_autoptr(Record) border =
		create_selection_border(selection ? selection : alpha_selection, ceil(inner), ceil(outer));
	/* A stroke that covers nothing changes nothing, so it records no empty history step */
	if (empty_extent(find_mask_bounds(get_record(border, "C"), read_extent(get_record(border, "V")))))
		return TRUE;
	PaintTool tool = {.tool = TOOL_STROKE_TARGET};
	int alpha = round_like_source(255.0 * opacity_percent / 100);
	gboolean stroked = fill_paint_region(&tool, document, border, color, alpha, FALSE, 14, 9, error);
	cancel_paint_tool(&tool);
	return stroked;
}
