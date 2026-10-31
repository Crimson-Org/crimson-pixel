#pragma once

#include "document/document.h"
#include "document/tiled-image.h"

/* While layers move, the parts below, moving and above them are drawn once, so the drag only moves textures */
enum { MOVE_PART_BELOW, MOVE_PART_MOVING, MOVE_PART_ABOVE, MOVE_PARTS };

typedef struct {
	TiledImage *parts[MOVE_PARTS];
	/* Where the below and moving parts sit in the document; outside the below part the current display shows */
	RasterExtent below_extent, moving_extent;
	/* The current display is the moving part, when nothing else shows */
	gboolean whole;
} MovePreview;

gboolean fill_move_preview(MovePreview *preview, Document *document, const GArray *layers, Record *floating);
void clear_move_preview(MovePreview *preview);
gboolean has_move_preview(const MovePreview *preview);
