#pragma once

#include "psd.h"

/* Random access output buffer, which grows with zeroed space */
typedef struct {
	GByteArray *data;
	gboolean large;
	GError **error;
	gboolean failed;
} PsdWriter;

void ensure_size(PsdWriter *writer, gsize size);
void put_byte(PsdWriter *writer, gsize offset, guint value);
void put_u16(PsdWriter *writer, gsize offset, guint value);
void put_u32(PsdWriter *writer, gsize offset, double value);
void put_float(PsdWriter *writer, gsize offset, double value);
void put_i16(PsdWriter *writer, gsize offset, double value);
gsize put_bytes(PsdWriter *writer, gsize offset, const guint8 *bytes, gsize length);
gsize put_counted_text(PsdWriter *writer, gsize offset, const char *text);

gsize put_adjustment(PsdWriter *writer, gsize offset, const char *key, const Record *descriptor);
gsize put_brightness_settings(PsdWriter *writer, gsize offset, const Record *brightness);
