#include <string.h>

#include "history.h"

Record *create_history_name(int group, int item) {
	Record *name = create_array();
	append_record(name, create_number(group));
	append_record(name, create_number(item));
	return name;
}

Record *create_history_entry(Record *name, const char *operation_id, gboolean transient, double time_ms) {
	Record *entry = create_object(RECORD_HISTORY);
	set_record(entry, "name", name);
	Record *operation = operation_id ? create_object(RECORD_PLAIN) : create_null();
	if (operation_id)
		set_string(operation, "id", operation_id);
	set_record(entry, "U", operation);
	set_boolean(entry, "anB", transient);
	set_record(entry, "data", create_null());
	set_number(entry, "pE", time_ms);
	return entry;
}

void append_history_entry(Document *document, Record *entry, const HistoryOperation *operation, gboolean extended) {
	Record *record = document->record, *history = get_record(record, "history");
	guint cursor = get_number(record, "NP");
	g_return_if_fail(operation && operation->undo && operation->redo && operation->memory_size);
	g_return_if_fail(g_strcmp0(get_string(get_record(entry, "U"), "id"), operation->id) == 0);
	if (!document->history_operations)
		document->history_operations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	char *operation_id = g_strdup(operation->id);
	HistoryOperation *handler = g_memdup2(operation, sizeof(*operation));
	handler->id = operation_id;
	g_hash_table_replace(document->history_operations, operation_id, handler);
	g_ptr_array_set_size(history->value.array, cursor + 1);
	if (get_number(record, "eN") > cursor)
		set_number(record, "eN", -1);
	if (document->saved_step > (gint64)cursor + document->trimmed_steps)
		document->saved_step = -1;
	if (strcmp(operation->id, "236")) {
		set_boolean(record, "at0", TRUE);
		remove_record(get_record(record, "gL"), "lastAppliedComp");
	}
	append_record(history, entry);
	set_number(record, "NP", ++cursor);
	set_boolean(record, "Ma", TRUE);
	document->revision++;
	guint count = 0, limit = document->history_limit ? document->history_limit : extended ? 40 : 20;
	for (gint index = count_records(history) - 1; index >= 0; index--) {
		if (get_boolean(get_item(history, index), "anB"))
			continue;
		if (++count != limit)
			continue;
		set_number(record, "NP", cursor - index);
		document->trimmed_steps += index;
		if (index)
			g_ptr_array_remove_range(history->value.array, 0, index);
		break;
	}
}

Record *get_latest_history_entry(const Document *document) {
	Record *history = get_record(document->record, "history");
	if (get_number(document->record, "NP") != count_records(history) - 1)
		return NULL;
	return get_item(history, count_records(history) - 1);
}

static const HistoryOperation *get_history_operation(const Document *document, const Record *entry) {
	const char *id = get_string(get_record(entry, "U"), "id");
	return id && document->history_operations ? g_hash_table_lookup(document->history_operations, id) : NULL;
}

gboolean undo_document_command(Document *document) {
	guint cursor = get_number(document->record, "NP");
	if (!cursor)
		return FALSE;
	Record *entry = get_item(get_record(document->record, "history"), cursor);
	const HistoryOperation *operation = get_history_operation(document, entry);
	g_return_val_if_fail(operation != NULL, FALSE);
	operation->undo(get_record(entry, "data"), document);
	set_number(document->record, "NP", cursor - 1);
	document->revision++;
	return TRUE;
}

gboolean redo_document_command(Document *document) {
	guint cursor = get_number(document->record, "NP");
	Record *history = get_record(document->record, "history");
	if (cursor == count_records(history) - 1)
		return FALSE;
	Record *entry = get_item(history, cursor + 1);
	const HistoryOperation *operation = get_history_operation(document, entry);
	g_return_val_if_fail(operation != NULL, FALSE);
	operation->redo(get_record(entry, "data"), document);
	set_number(document->record, "NP", cursor + 1);
	document->revision++;
	return TRUE;
}

double get_history_memory_size(const Document *document, const Record *entry) {
	const HistoryOperation *operation = get_history_operation(document, entry);
	return operation ? operation->memory_size(get_record(entry, "data")) : 0;
}

void mark_document_saved(Document *document, gboolean reset_time, double time_ms) {
	set_number(document->record, "eN", get_number(document->record, "NP"));
	document->saved_step = (gint64)get_number(document->record, "NP") + document->trimmed_steps;
	set_number(document->record, "aH2", time_ms);
	set_boolean(document->record, "z3", TRUE);
	if (reset_time)
		set_number(document->record, "aNe", time_ms);
}

gboolean is_document_modified(const Document *document, double time_ms) {
	Record *record = document->record;
	return (gint64)get_number(record, "NP") + document->trimmed_steps != document->saved_step ||
		   (time_ms - get_number(record, "aNe") > 5 * 60 * 1000 && get_number(record, "NP") > 0 &&
			count_records(get_record(record, "Z")) > 1);
}

void mark_document_unsaved(Document *document) {
	set_number(document->record, "eN", -1);
	document->saved_step = -1;
}

void clear_document_history(Document *document, double time_ms) {
	document->saved_step = get_number(document->record, "eN");
	document->trimmed_steps = 0;
	Record *name = create_array(), *history = create_array();
	append_record(name, create_number(1));
	append_record(name, create_number(0));
	append_record(history, create_history_entry(name, NULL, FALSE, time_ms));
	set_record(document->record, "history", history);
	set_number(document->record, "NP", 0);
	set_boolean(document->record, "Ma", TRUE);
	if (document->history_operations)
		g_hash_table_remove_all(document->history_operations);
}
