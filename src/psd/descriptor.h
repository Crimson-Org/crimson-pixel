#pragma once

#include "psd.h"

/* A {t, v} descriptor value of the given four character type */
Record *create_typed_value(const char *type, Record *value);
gsize read_descriptor(PsdReader *reader, gsize offset, Record *object);
void write_descriptor(GByteArray *output, const Record *object);
void write_u8(GByteArray *output, guint value);
void write_u16(GByteArray *output, guint value);
void write_u32(GByteArray *output, double value);
void write_double(GByteArray *output, double value);
void write_ascii(GByteArray *output, const char *text);
void write_counted_text(GByteArray *output, const char *text, gboolean terminated);
