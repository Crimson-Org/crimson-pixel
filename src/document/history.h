#pragma once

#include "document.h"

typedef struct {
	const char *id;
	void (*undo)(Record *data, Document *document);
	void (*redo)(Record *data, Document *document);
	double (*memory_size)(const Record *data);
} HistoryOperation;

/* A history step is named by a text group, an item in it and sometimes a detail, as saved documents name it */
typedef struct {
	int group, item, detail;
} HistoryStep;

#define NO_DETAIL (-1)
#define HISTORY_STEP(group, item) ((HistoryStep){group, item, NO_DETAIL})
#define HISTORY_DETAILED_STEP(group, item, detail) ((HistoryStep){group, item, detail})

#define HISTORY_OPEN HISTORY_STEP(1, 0)
#define HISTORY_CLEAR HISTORY_STEP(2, 2)
#define HISTORY_FILL HISTORY_STEP(2, 3)
#define HISTORY_PASTE HISTORY_STEP(5, 2)
#define HISTORY_DUPLICATE_LAYERS HISTORY_STEP(6, 0)
#define HISTORY_ADD_ADJUSTMENT_LAYER HISTORY_STEP(6, 1)
#define HISTORY_GROUP_LAYERS HISTORY_STEP(6, 9)
#define HISTORY_MERGE_DOWN HISTORY_DETAILED_STEP(6, 12, 0)
#define HISTORY_FLATTEN HISTORY_DETAILED_STEP(6, 12, 1)
#define HISTORY_NEW_LAYER HISTORY_STEP(6, 13)
#define HISTORY_CREATE_CLIPPING_MASK HISTORY_STEP(6, 18)
#define HISTORY_RELEASE_CLIPPING_MASK HISTORY_STEP(6, 19)
#define HISTORY_DELETE_LAYERS HISTORY_STEP(6, 31)
#define HISTORY_MOVE_LAYERS HISTORY_STEP(6, 35)
#define HISTORY_RENAME_LAYER HISTORY_STEP(6, 37)
#define HISTORY_LAYER_COLOR HISTORY_STEP(6, 38)
#define HISTORY_EDIT_ADJUSTMENT HISTORY_STEP(6, 39)
#define HISTORY_LAYER_VIA_COPY HISTORY_DETAILED_STEP(6, 49, 0)
#define HISTORY_LAYER_VIA_CUT HISTORY_DETAILED_STEP(6, 49, 1)
#define HISTORY_SELECT_ALL HISTORY_STEP(7, 0)
#define HISTORY_DESELECT HISTORY_STEP(7, 1)
#define HISTORY_INVERT_SELECTION HISTORY_STEP(7, 2)
#define HISTORY_EXPAND_SELECTION HISTORY_STEP(7, 4)
#define HISTORY_CONTRACT_SELECTION HISTORY_STEP(7, 5)
#define HISTORY_FEATHER_SELECTION HISTORY_STEP(7, 6)
#define HISTORY_MOVE_SELECTION HISTORY_STEP(7, 7)
#define HISTORY_BORDER_SELECTION HISTORY_STEP(7, 9)
#define HISTORY_TRANSFORM_SELECTION HISTORY_STEP(7, 10)
#define HISTORY_CROP HISTORY_STEP(10, 2)
#define HISTORY_ELLIPSE_SELECTION HISTORY_STEP(10, 4)
#define HISTORY_LASSO_SELECTION HISTORY_STEP(10, 9)
#define HISTORY_MAGIC_WAND HISTORY_STEP(10, 12)
#define HISTORY_RECTANGLE_SELECTION HISTORY_STEP(10, 15)
#define HISTORY_TRANSFORM HISTORY_STEP(10, 16)
#define HISTORY_CANVAS_SIZE HISTORY_STEP(11, 3)
#define HISTORY_IMAGE_SIZE HISTORY_STEP(11, 10)
#define HISTORY_TRIM HISTORY_DETAILED_STEP(11, 12, 0)
#define HISTORY_REVEAL_ALL HISTORY_DETAILED_STEP(11, 12, 2)
#define HISTORY_MOVE_COPY HISTORY_STEP(12, 53)
#define HISTORY_MOVE HISTORY_STEP(12, 54)
#define HISTORY_STROKE HISTORY_STEP(14, 9)
#define HISTORY_SELECT_LAYER_PIXELS HISTORY_STEP(14, 11)
#define HISTORY_SMOOTH_SELECTION HISTORY_DETAILED_STEP(19, 3, 0)
/* Painting tools name their steps in the tools group */
#define HISTORY_TOOLS_GROUP 10
enum { TOOL_STEP_BRUSH = 0, TOOL_STEP_ERASER = 3, TOOL_STEP_GRADIENT = 6, TOOL_STEP_BUCKET = 13, TOOL_STEP_PENCIL = 38,
	   TOOL_STEP_MAGIC_ERASER = 52 };

Record *create_history_name(HistoryStep step);
Record *create_history_entry(Record *name, const char *operation_id, gboolean transient, double time_ms);
void append_history_entry(Document *document, Record *entry, const HistoryOperation *operation, gboolean extended);
Record *get_latest_history_entry(const Document *document);
gboolean undo_document_command(Document *document);
gboolean redo_document_command(Document *document);
double get_history_memory_size(const Document *document, const Record *entry);
/* The history step a document is at, counting steps that were trimmed away, so a step taken later stays apart */
gint64 get_document_step(const Document *document);
/* Marks the document saved as it was at the step that was written */
void mark_document_saved(Document *document, gint64 step, gboolean reset_time, double time_ms);
void mark_document_unsaved(Document *document);
gboolean is_document_modified(const Document *document, double time_ms);
void clear_document_history(Document *document, double time_ms);
