#pragma once

#include "document.h"

/* Marks the canvas for redrawing after a layer change */
void invalidate_document(Document *document);
gboolean change_layer_fill_opacity(Document *document, int layer_index, double opacity, gboolean extended_history,
								   GError **error);
gboolean change_layer_opacity(Document *document, int layer_index, double opacity, gboolean extended_history,
							  GError **error);
gboolean change_layer_blend_mode(Document *document, int layer_index, const char *mode, gboolean extended_history,
								 GError **error);
gboolean toggle_layer_visible(Document *document, int layer_index, gboolean extended_history, GError **error);
gboolean solo_layer_visible(Document *document, int layer_index, gboolean extended_history, GError **error);
gboolean select_layer(Document *document, int layer_index, gboolean add, gboolean range, GError **error);
void toggle_group_open(Document *document, int layer_index, gboolean all_siblings);
gboolean add_new_layer(Document *document, gboolean below, gboolean extended_history, GError **error);
gboolean delete_selected_layers(Document *document, gboolean extended_history, GError **error);
gboolean duplicate_selected_layers(Document *document, gboolean extended_history, GError **error);
gboolean rename_layer(Document *document, int layer_index, const char *name, gboolean extended_history, GError **error);
gboolean copy_selection_to_layer(Document *document, gboolean cut, gboolean extended_history, GError **error);
GArray *collect_selected_subtrees(Document *document);
GArray *collect_subtrees(Document *document, const Record *chosen);
Record *copy_selection_pixels(Document *document, gboolean merged, RasterExtent *extent, GError **error);
gboolean clear_selection_pixels(Document *document, gboolean extended_history, GError **error);
gboolean paste_pixels(Document *document, Record *pixels, RasterExtent extent, gboolean same_document,
					  gboolean extended_history, GError **error);

typedef enum { ARRANGE_FRONT, ARRANGE_FORWARD, ARRANGE_BACKWARD, ARRANGE_BACK } LayerArrangement;

gboolean move_layers_to(Document *document, int source, int target, gboolean below, gboolean copy,
						gboolean extended_history, GError **error);
gboolean arrange_layers(Document *document, LayerArrangement arrangement, gboolean extended_history, GError **error);

/* The shared et.yd layer list step and its helpers, for commands that rebuild the layer list */
Record *create_layer_list_data(Document *document, Record *next, Record *previous_selection, Record *next_selection);
void commit_layer_list(Document *document, Record *data, Record *name, gboolean extended_history);
void record_layer_list(Document *document, Record *name, Record *next, Record *previous_selection,
					   Record *next_selection, gboolean extended_history);
Record *create_index_list(int first);
int find_highest_suffix(const Record *layers, const char *prefix);
gboolean contains_index(const GArray *indices, int value);
gint compare_indices(gconstpointer first, gconstpointer second);
gboolean can_clip_layer(Document *document, int layer_index);
gboolean toggle_clipping_mask(Document *document, gboolean extended_history, GError **error);
gboolean set_layer_color(Document *document, int color, gboolean extended_history, GError **error);
