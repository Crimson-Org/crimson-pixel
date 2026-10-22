#pragma once

#include "document.h"

typedef struct {
	int mode;
	GArray *layers;
	int moved_x, moved_y;
	Record *floating;
	/* While a preview draws the move, floating pixels are composed into their layer only on release */
	gboolean deferred;
	/* Where the floating pixels were when the document was last composed */
	RasterExtent composed;
} MoveDrag;

int find_layer_at(Document *document, double x, double y);
void pick_layer_at(Document *document, double x, double y, gboolean toggle, gboolean whole_group);
void select_layers_in_rectangle(Document *document, RasterExtent rectangle);
gboolean begin_move(MoveDrag *drag, Document *document, gboolean inside_selection, gboolean copy,
					gboolean extended_history, GError **error);
void update_move(MoveDrag *drag, Document *document, int x, int y);
gboolean finish_move(MoveDrag *drag, Document *document, gboolean extended_history);
void clear_move(MoveDrag *drag);
Record *lift_pixels(Document *document, Record *layer, gboolean copy, GError **error);
void compose_floating(Document *document, Record *data, const RasterExtent *previous);
void append_move(Document *document, Record *data, int group, int item, gboolean extended_history);
