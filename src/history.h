#pragma once

#include "document.h"

typedef struct {
	const char *id;
	void (*undo)(Record *data, Document *document);
	void (*redo)(Record *data, Document *document);
	double (*memory_size)(const Record *data);
} HistoryOperation;

Record *create_history_name(int group, int item);
Record *create_history_entry(Record *name, const char *operation_id, gboolean transient, double time_ms);
void append_history_entry(Document *document, Record *entry, const HistoryOperation *operation, gboolean extended);
Record *get_latest_history_entry(const Document *document);
gboolean undo_document_command(Document *document);
gboolean redo_document_command(Document *document);
double get_history_memory_size(const Document *document, const Record *entry);
void mark_document_saved(Document *document, gboolean reset_time, double time_ms);
void mark_document_unsaved(Document *document);
gboolean is_document_modified(const Document *document, double time_ms);
void clear_document_history(Document *document, double time_ms);
