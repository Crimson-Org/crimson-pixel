#pragma once

#include "document.h"
#include "magic-wand.h"
#include "selection-path.h"

typedef enum { SELECTION_REPLACE, SELECTION_ADD, SELECTION_SUBTRACT, SELECTION_INTERSECT } SelectionMode;

Record *get_document_selection(const Document *document);
RasterExtent get_selection_extent(const Document *document);
gboolean selection_contains(const Document *document, double x, double y);
gboolean select_rectangle(Document *document, RasterExtent rectangle, SelectionMode mode, gboolean extended_history);
gboolean select_path_area(Document *document, const SelectionPath *path, SelectionMode mode, gboolean anti_alias,
						  int group, int item, gboolean extended_history);
gboolean select_all(Document *document, gboolean extended_history);
gboolean deselect(Document *document, gboolean extended_history);
gboolean invert_selection(Document *document, gboolean extended_history);
void move_selection(Document *document, int x, int y, gboolean extended_history);
RasterExtent constrain_rectangle_drag(double start_x, double start_y, double end_x, double end_y, double ratio,
									  gboolean centered);
RasterExtent constrain_selection_drag(double start_x, double start_y, double end_x, double end_y, gboolean square,
									  gboolean centered);
RasterExtent find_mask_bounds(const Record *mask, RasterExtent extent);
gboolean select_similar_area(Document *document, double x, double y, WandSettings settings, gboolean all_layers,
							 SelectionMode mode, gboolean extended_history, GError **error);
gboolean select_layer_pixels(Document *document, int layer_index, gboolean extended_history);
gboolean record_selection(Document *document, Record *selection, int group, int item, gboolean extended);
gboolean record_named_selection(Document *document, Record *selection, Record *name, gboolean extended);
Record *create_selection(RasterExtent extent, Record *mask);
Record *create_selection_mask(const Document *document, RasterExtent extent);
void copy_selection_mask(const Record *source, RasterExtent from, Record *target, RasterExtent to);
Record *trim_selection(const Document *document, Record *selection);
