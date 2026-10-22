#pragma once

#include "document.h"

typedef enum { FILL_FOREGROUND, FILL_BACKGROUND, FILL_COLOR, FILL_BLACK, FILL_GRAY, FILL_WHITE } FillContent;

typedef enum { STROKE_INSIDE, STROKE_CENTER, STROKE_OUTSIDE } StrokeLocation;

guint32 get_fill_content_color(FillContent content, guint32 foreground, guint32 background, guint32 custom);
gboolean fill_document(Document *document, const char *operation, guint32 color, int opacity_percent,
					   gboolean preserve_transparency, GError **error);
gboolean stroke_document(Document *document, guint32 color, int width, StrokeLocation location, int opacity_percent,
						 GError **error);
