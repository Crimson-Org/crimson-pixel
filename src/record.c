#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "record.h"

static Record *allocate_record(RecordType type) {
	Record *record = g_new0(Record, 1);
	record->references = 1;
	record->type = type;
	return record;
}

Record *create_null(void) {
	return allocate_record(RECORD_NULL);
}

Record *create_boolean(gboolean value) {
	Record *record = allocate_record(RECORD_BOOLEAN);
	record->value.boolean = value;
	return record;
}

Record *create_number(double value) {
	Record *record = allocate_record(RECORD_NUMBER);
	record->value.number = value;
	return record;
}

Record *create_string_length(const char *value, gsize length) {
	Record *record = allocate_record(RECORD_STRING);
	record->value.string = g_malloc(length + 1);
	memcpy(record->value.string, value, length);
	record->value.string[length] = 0;
	record->string_length = length;
	return record;
}

Record *create_string(const char *value) {
	return value ? create_string_length(value, strlen(value)) : create_null();
}

Record *create_array(void) {
	Record *record = allocate_record(RECORD_ARRAY);
	record->value.array = g_ptr_array_new_with_free_func((GDestroyNotify)free_record);
	return record;
}

Record *create_object(RecordClass class) {
	Record *record = allocate_record(RECORD_OBJECT);
	record->class = class;
	record->value.object = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_record);
	record->order = g_ptr_array_new();
	return record;
}

Record *create_reference(Record *target) {
	Record *record = allocate_record(RECORD_REFERENCE);
	record->value.reference = target;
	return record;
}

Record *create_bytes(const guint8 *bytes, gsize length) {
	Record *record = allocate_record(RECORD_BUFFER);
	record->value.buffer = (ChannelBuffer){8, length, g_try_malloc0(MAX(length, 1))};
	if (!record->value.buffer.data) {
		g_free(record);
		return NULL;
	}
	if (bytes && length)
		memcpy(record->value.buffer.data, bytes, length);
	return record;
}

static Record *allocate_channels(gsize length, unsigned depth, gboolean cleared) {
	g_return_val_if_fail(depth == 8 || depth == 16 || depth == 32, NULL);
	gsize padding = depth == 8 ? (4 - length % 4) % 4 : depth == 16 ? length & 1 : 0;
	if (length > G_MAXSIZE - padding || length + padding > G_MAXSIZE / (depth / 8))
		return NULL;
	Record *record = allocate_record(RECORD_BUFFER);
	record->value.buffer = (ChannelBuffer){depth, length + padding, NULL};
	record->value.buffer.data =
		cleared ? g_try_malloc0_n(length + padding, depth / 8) : g_try_malloc_n(length + padding, depth / 8);
	if (length + padding && !record->value.buffer.data) {
		g_free(record);
		return NULL;
	}
	return record;
}

Record *create_channels(gsize length, unsigned depth) {
	return allocate_channels(length, depth, TRUE);
}

Record *create_scratch_channels(gsize length, unsigned depth) {
	return allocate_channels(length, depth, FALSE);
}

void free_record(Record *record) {
	if (!record || !g_atomic_int_dec_and_test(&record->references))
		return;
	switch (record->type) {
	case RECORD_STRING:
		g_free(record->value.string);
		break;
	case RECORD_ARRAY:
		g_ptr_array_unref(record->value.array);
		break;
	case RECORD_OBJECT:
		g_ptr_array_unref(record->order);
		g_hash_table_unref(record->value.object);
		break;
	case RECORD_BUFFER:
		g_free(record->value.buffer.data);
		break;
	default:
		break;
	}
	g_free(record);
}

/* Objects remember insertion order so source descriptors keep their JavaScript key order */
void set_record(Record *object, const char *key, Record *value) {
	g_return_if_fail(object && object->type == RECORD_OBJECT && value);
	gpointer existing;
	if (g_hash_table_lookup_extended(object->value.object, key, &existing, NULL)) {
		g_hash_table_insert(object->value.object, g_strdup(key), value);
		return;
	}
	char *owned = g_strdup(key);
	g_hash_table_insert(object->value.object, owned, value);
	g_ptr_array_add(object->order, owned);
}

void remove_record(Record *object, const char *key) {
	g_return_if_fail(object && object->type == RECORD_OBJECT);
	gpointer existing;
	if (!g_hash_table_lookup_extended(object->value.object, key, &existing, NULL))
		return;
	g_ptr_array_remove(object->order, existing);
	g_hash_table_remove(object->value.object, key);
}

static gboolean is_array_index(const char *key, guint64 *index) {
	if (!*key || (key[0] == '0' && key[1]) || !g_ascii_isdigit(key[0]))
		return FALSE;
	char *end;
	*index = g_ascii_strtoull(key, &end, 10);
	return !*end && *index < 4294967295u;
}

static gint compare_array_keys(gconstpointer first, gconstpointer second) {
	guint64 a = 0, b = 0;
	is_array_index(*(const char *const *)first, &a);
	is_array_index(*(const char *const *)second, &b);
	return a < b ? -1 : a > b;
}

/* JavaScript enumerates integer-like keys ascending before other keys in insertion order */
GPtrArray *get_record_keys(const Record *object) {
	GPtrArray *keys = g_ptr_array_new(), *named = g_ptr_array_new();
	for (guint index = 0; object && index < object->order->len; index++) {
		guint64 number;
		char *key = g_ptr_array_index(object->order, index);
		g_ptr_array_add(is_array_index(key, &number) ? keys : named, key);
	}
	g_ptr_array_sort(keys, compare_array_keys);
	g_ptr_array_extend_and_steal(keys, named);
	return keys;
}

void append_record(Record *array, Record *value) {
	g_return_if_fail(array && array->type == RECORD_ARRAY && value);
	g_ptr_array_add(array->value.array, value);
}

Record *get_record(const Record *object, const char *key) {
	return object && object->type == RECORD_OBJECT ? g_hash_table_lookup(object->value.object, key) : NULL;
}

Record *get_item(const Record *array, guint index) {
	return array && array->type == RECORD_ARRAY && index < array->value.array->len
			   ? g_ptr_array_index(array->value.array, index)
			   : NULL;
}

guint count_records(const Record *array) {
	return array && array->type == RECORD_ARRAY ? array->value.array->len : 0;
}

double get_number(const Record *object, const char *key) {
	Record *value = get_record(object, key);
	return value && value->type == RECORD_NUMBER ? value->value.number : 0;
}

const char *get_string(const Record *object, const char *key) {
	Record *value = get_record(object, key);
	return value && value->type == RECORD_STRING ? value->value.string : NULL;
}

gboolean get_boolean(const Record *object, const char *key) {
	Record *value = get_record(object, key);
	return value && value->type == RECORD_BOOLEAN && value->value.boolean;
}

gboolean has_record(const Record *object, const char *key) {
	Record *value = get_record(object, key);
	return value && value->type != RECORD_NULL;
}

void set_number(Record *object, const char *key, double value) {
	set_record(object, key, create_number(value));
}

void set_boolean(Record *object, const char *key, gboolean value) {
	set_record(object, key, create_boolean(value));
}

void set_string(Record *object, const char *key, const char *value) {
	set_record(object, key, create_string(value));
}

double channel_maximum(unsigned depth) {
	return depth == 8 ? 255 : depth == 16 ? 65535 : 1;
}

double read_channel_slow(const Record *buffer, gsize index) {
	if (!buffer || buffer->type != RECORD_BUFFER || index >= buffer->value.buffer.length)
		return 0;
	ChannelBuffer channels = buffer->value.buffer;
	return channels.depth == 8	  ? ((guint8 *)channels.data)[index]
		   : channels.depth == 16 ? ((guint16 *)channels.data)[index]
								  : ((float *)channels.data)[index];
}

void write_channel_slow(Record *buffer, gsize index, double value) {
	g_return_if_fail(buffer && buffer->type == RECORD_BUFFER && index < buffer->value.buffer.length);
	ChannelBuffer channels = buffer->value.buffer;
	if (channels.depth == 32) {
		/* Binary32 values above its maximum still round finite below the overflow midpoint */
		const double overflow_midpoint = 0x1.ffffffp127;
		if (fabs(value) >= overflow_midpoint)
			((float *)channels.data)[index] = copysign(INFINITY, value);
		else if (fabs(value) > FLT_MAX)
			((float *)channels.data)[index] = copysign(FLT_MAX, value);
		else
			((float *)channels.data)[index] = value;
		return;
	}
	/* Typed arrays truncate and wrap instead of clamping integer assignments */
	double wrapped = isfinite(value) ? fmod(trunc(value), channel_maximum(channels.depth) + 1) : 0;
	if (wrapped < 0)
		wrapped += channel_maximum(channels.depth) + 1;
	if (channels.depth == 8)
		((guint8 *)channels.data)[index] = wrapped;
	else
		((guint16 *)channels.data)[index] = wrapped;
}

static Record *copy_value(const Record *record, gboolean json) {
	if (!record)
		return create_null();
	Record *copy;
	switch (record->type) {
	case RECORD_NULL:
		return create_null();
	case RECORD_BOOLEAN:
		return create_boolean(record->value.boolean);
	case RECORD_NUMBER:
		return json && !isfinite(record->value.number)
				   ? create_null()
				   : create_number(json && record->value.number == 0 ? 0 : record->value.number);
	case RECORD_STRING:
		return create_string_length(record->value.string, record->string_length);
	case RECORD_REFERENCE:
		return create_reference(record->value.reference);
	case RECORD_ARRAY:
		copy = create_array();
		for (guint index = 0; index < count_records(record); index++)
			append_record(copy, copy_value(get_item(record, index), json));
		return copy;
	case RECORD_OBJECT: {
		copy = create_object(json ? RECORD_PLAIN : record->class);
		for (guint index = 0; index < record->order->len; index++) {
			const char *key = g_ptr_array_index(record->order, index);
			set_record(copy, key, copy_value(g_hash_table_lookup(record->value.object, key), json));
		}
		return copy;
	}
	case RECORD_BUFFER:
		if (json) {
			copy = create_object(RECORD_PLAIN);
			for (gsize index = 0; index < record->value.buffer.length; index++) {
				g_autofree char *key = g_strdup_printf("%zu", index);
				double value = read_channel(record, index);
				set_record(copy, key, isfinite(value) ? create_number(value) : create_null());
			}
			return copy;
		}
		/* Typed array slices keep their exact length, including unpadded byte arrays */
		if (record->value.buffer.depth == 8)
			return create_bytes(record->value.buffer.data, record->value.buffer.length);
		copy = create_channels(record->value.buffer.length, record->value.buffer.depth);
		if (copy && record->value.buffer.length)
			memcpy(copy->value.buffer.data, record->value.buffer.data,
				   record->value.buffer.length * (record->value.buffer.depth / 8));
		return copy;
	}
	g_assert_not_reached();
}

Record *reference_record(Record *record) {
	if (record)
		g_atomic_int_inc(&record->references);
	return record;
}

Record *copy_array_shallow(const Record *array) {
	Record *copy = create_array();
	for (guint index = 0; index < count_records(array); index++)
		append_record(copy, reference_record(get_item(array, index)));
	return copy;
}

Record *copy_record(const Record *record) {
	return copy_value(record, FALSE);
}

Record *copy_json_record(const Record *record) {
	return copy_value(record, TRUE);
}

static void append_quoted(GString *output, const char *text, gsize length) {
	g_string_append_c(output, '"');
	for (gsize index = 0; index < length; index++) {
		const unsigned char *letter = (const unsigned char *)text + index;
		if (*letter == '"' || *letter == '\\')
			g_string_append_c(output, '\\');
		if (*letter == '\r')
			g_string_append(output, "\\r");
		else if (*letter == '\n')
			g_string_append(output, "\\n");
		else if (*letter == '\t')
			g_string_append(output, "\\t");
		else if (*letter == '\b')
			g_string_append(output, "\\b");
		else if (*letter == '\f')
			g_string_append(output, "\\f");
		else if (*letter < 32)
			g_string_append_printf(output, "\\u%04x", *letter);
		else
			g_string_append_c(output, *letter);
	}
	g_string_append_c(output, '"');
}

/* JSON.stringify writes the shortest round trip digits and uses exponents outside 1e-7 to 1e21 */
static void append_number(GString *output, double value) {
	if (!isfinite(value)) {
		g_string_append(output, "null");
		return;
	}
	if (value == 0) {
		g_string_append_c(output, '0');
		return;
	}
	char number[G_ASCII_DTOSTR_BUF_SIZE], format[8];
	int precision = 1;
	for (; precision < 17; precision++) {
		g_snprintf(format, sizeof format, "%%.%de", precision - 1);
		g_ascii_formatd(number, sizeof number, format, value);
		if (g_ascii_strtod(number, NULL) == value)
			break;
	}
	g_snprintf(format, sizeof format, "%%.%de", precision - 1);
	g_ascii_formatd(number, sizeof number, format, value);
	char *exponent_text = strchr(number, 'e');
	int exponent = atoi(exponent_text + 1);
	if (exponent >= -7 && exponent < 21) {
		g_snprintf(format, sizeof format, "%%.%df", MAX(0, precision - 1 - exponent));
		g_ascii_formatd(number, sizeof number, format, value);
		g_string_append(output, number);
		return;
	}
	*exponent_text = '\0';
	char *mantissa_end = exponent_text;
	if (strchr(number, '.')) {
		while (mantissa_end[-1] == '0')
			*--mantissa_end = '\0';
		if (mantissa_end[-1] == '.')
			mantissa_end[-1] = '\0';
	}
	g_string_append_printf(output, "%se%c%d", number, exponent < 0 ? '-' : '+', ABS(exponent));
}

static gint compare_key_pointers(const char *const *first, const char *const *second) {
	return strcmp(*first, *second);
}

static void append_description(GString *output, const Record *record, gboolean ordered) {
	if (!record || record->type == RECORD_NULL) {
		g_string_append(output, "null");
		return;
	}
	switch (record->type) {
	case RECORD_BOOLEAN:
		g_string_append(output, record->value.boolean ? "true" : "false");
		break;
	case RECORD_NUMBER:
		append_number(output, record->value.number);
		break;
	case RECORD_STRING:
		append_quoted(output, record->value.string, record->string_length);
		break;
	case RECORD_REFERENCE:
		append_quoted(output, "<document>", strlen("<document>"));
		break;
	case RECORD_ARRAY:
		g_string_append_c(output, '[');
		for (guint index = 0; index < count_records(record); index++) {
			if (index)
				g_string_append_c(output, ',');
			append_description(output, get_item(record, index), ordered);
		}
		g_string_append_c(output, ']');
		break;
	case RECORD_BUFFER:
		g_string_append_printf(output, "{\"$buffer\":%u,\"values\":[", record->value.buffer.depth);
		for (gsize index = 0; index < record->value.buffer.length; index++) {
			if (index)
				g_string_append_c(output, ',');
			Record value = {.type = RECORD_NUMBER, .value.number = read_channel(record, index)};
			append_description(output, &value, ordered);
		}
		g_string_append(output, "]}");
		break;
	case RECORD_OBJECT: {
		/* Sorted keys compare across implementations, while ordered keys follow JSON.stringify */
		g_autoptr(GPtrArray) keys = get_record_keys(record);
		if (!ordered)
			g_ptr_array_sort(keys, (GCompareFunc)compare_key_pointers);
		g_string_append_c(output, '{');
		for (guint index = 0; index < keys->len; index++) {
			const char *key = g_ptr_array_index(keys, index);
			if (index)
				g_string_append_c(output, ',');
			append_quoted(output, key, strlen(key));
			g_string_append_c(output, ':');
			append_description(output, get_record(record, key), ordered);
		}
		g_string_append_c(output, '}');
		break;
	}
	default:
		break;
	}
}

char *describe_record(const Record *record) {
	GString *output = g_string_new(NULL);
	append_description(output, record, FALSE);
	return g_string_free(output, FALSE);
}

char *describe_record_in_order(const Record *record) {
	GString *output = g_string_new(NULL);
	append_description(output, record, TRUE);
	return g_string_free(output, FALSE);
}

static double truncate_to_int32(double value) {
	if (!isfinite(value))
		return 0;
	double wrapped = fmod(trunc(value), 4294967296.0);
	if (wrapped < 0)
		wrapped += 4294967296.0;
	return wrapped >= 2147483648.0 ? wrapped - 4294967296.0 : wrapped;
}

double convert_channel_depth(double value, unsigned from, unsigned to) {
	if (from == to)
		return value;
	if (to == 8 && from == 16)
		return (guint32)value >> 8;
	if (to == 8)
		return MIN(255, truncate_to_int32(.5 + 255 * value));
	if (to == 16 && from == 8)
		return (guint32)value << 8 | (guint32)value;
	if (to == 16)
		return MIN(65535, 65535 * value);
	return value * (1.0 / (from == 8 ? 255 : 65535));
}

/* The channel value as h._l stores it in an eight bit typed array */
guint8 read_channel_byte(const Record *buffer, gsize index) {
	double value = read_channel(buffer, index);
	if (buffer->value.buffer.depth == 8)
		return value;
	value = convert_channel_depth(value, buffer->value.buffer.depth, 8);
	return isfinite(value) ? (guint8)((gint64)value & 255) : 0;
}
