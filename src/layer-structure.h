#pragma once

#include "document.h"

typedef enum { MERGE_SELECTED, MERGE_VISIBLE } MergeKind;

gboolean group_selected_layers(Document *document, gboolean extended_history, GError **error);
gboolean ungroup_selected_layer(Document *document, gboolean extended_history, GError **error);
gboolean merge_layers(Document *document, MergeKind kind, gboolean extended_history, GError **error);
gboolean flatten_image(Document *document, gboolean extended_history, GError **error);
