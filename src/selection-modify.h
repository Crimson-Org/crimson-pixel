#pragma once

#include "document.h"

typedef enum { MODIFY_BORDER, MODIFY_SMOOTH, MODIFY_EXPAND, MODIFY_CONTRACT, MODIFY_FEATHER } SelectionModification;

Record *create_selection_border(const Record *selection, double inner, double outer);
Record *create_modified_selection(const Record *selection, RasterExtent canvas, SelectionModification modification,
								  int amount, gboolean at_canvas_bounds);
gboolean modify_selection(Document *document, SelectionModification modification, int amount, gboolean at_canvas_bounds,
						  gboolean extended_history);
