#pragma once

#include <glib.h>

typedef enum {
	RECORD_NULL,
	RECORD_BOOLEAN,
	RECORD_NUMBER,
	RECORD_STRING,
	RECORD_ARRAY,
	RECORD_OBJECT,
	RECORD_BUFFER,
	RECORD_REFERENCE
} RecordType;

/* Prototype identity controls the original class-specific copy operations */
typedef enum {
	RECORD_PLAIN,
	RECORD_POINT,
	RECORD_EXTENT,
	RECORD_TRANSFORM,
	RECORD_LAYER,
	RECORD_LAYER_CACHE,
	RECORD_MASK,
	RECORD_VECTOR_MASK,
	RECORD_DOCUMENT,
	RECORD_VIEW,
	RECORD_HISTORY
} RecordClass;

typedef struct {
	unsigned depth;
	gsize length;
	gpointer data;
} ChannelBuffer;

/* Objects own their children while references model explicit non-owning back links */
typedef struct Record Record;

struct Record {
	gint references;
	RecordType type;
	RecordClass class;
	gsize string_length;
	GPtrArray *order;

	union {
		gboolean boolean;
		double number;
		char *string;
		GPtrArray *array;
		GHashTable *object;
		ChannelBuffer buffer;
		Record *reference;
	} value;
};

Record *create_null(void);
Record *create_boolean(gboolean value);
Record *create_number(double value);
Record *create_string(const char *value);
Record *create_string_length(const char *value, gsize length);
Record *create_array(void);
Record *create_object(RecordClass class);
Record *create_reference(Record *target);
Record *create_channels(gsize length, unsigned depth);
/* Channels left uninitialized, for scratch space that is written before it is read */
Record *create_scratch_channels(gsize length, unsigned depth);
Record *create_bytes(const guint8 *bytes, gsize length);
Record *reference_record(Record *record);
Record *copy_array_shallow(const Record *array);
Record *copy_record(const Record *record);
Record *copy_json_record(const Record *record);
void free_record(Record *record);
void set_record(Record *object, const char *key, Record *value);
void remove_record(Record *object, const char *key);
GPtrArray *get_record_keys(const Record *object);
void append_record(Record *array, Record *value);
Record *get_record(const Record *object, const char *key);
Record *get_item(const Record *array, guint index);
guint count_records(const Record *array);
double get_number(const Record *object, const char *key);
const char *get_string(const Record *object, const char *key);
gboolean get_boolean(const Record *object, const char *key);
gboolean has_record(const Record *object, const char *key);
void set_number(Record *object, const char *key, double value);
void set_boolean(Record *object, const char *key, gboolean value);
void set_string(Record *object, const char *key, const char *value);
double read_channel_slow(const Record *buffer, gsize index);
void write_channel_slow(Record *buffer, gsize index, double value);
double channel_maximum(unsigned depth);
double convert_channel_depth(double value, unsigned from, unsigned to);
guint8 read_channel_byte(const Record *buffer, gsize index);
char *describe_record(const Record *record);
char *describe_record_in_order(const Record *record);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Record, free_record)

/* Eight-bit samples stay inline; deeper or wrapped values take the typed array path */
static inline double read_channel(const Record *buffer, gsize index) {
	if (buffer && buffer->type == RECORD_BUFFER && buffer->value.buffer.depth == 8 &&
		index < buffer->value.buffer.length)
		return ((const guint8 *)buffer->value.buffer.data)[index];
	return read_channel_slow(buffer, index);
}

static inline void write_channel(Record *buffer, gsize index, double value) {
	if (buffer && buffer->type == RECORD_BUFFER && buffer->value.buffer.depth == 8 &&
		index < buffer->value.buffer.length && value >= 0 && value < 256) {
		((guint8 *)buffer->value.buffer.data)[index] = (guint8)value;
		return;
	}
	write_channel_slow(buffer, index, value);
}
