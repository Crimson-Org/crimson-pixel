#pragma once

#include "document.h"
#include "../lang/en.h"

#define PSD_MAX_DECODED_BYTES (1024ULL * 1024 * 1024)

/* Reading state for one in-memory Photoshop file */
typedef struct {
	const guint8 *data;
	gsize length;
	gboolean large, failed, lenient;
	GError **error;
	Document *document;
	GCancellable *cancellable;
} PsdReader;

void fail_psd(PsdReader *reader, const char *message);
void note_unsupported_psd_feature(PsdReader *reader, const char *feature);
gboolean has_psd_bytes(PsdReader *reader, gsize offset, gsize count);
int read_psd_byte(PsdReader *reader, gsize offset);
guint read_psd_u16(PsdReader *reader, gsize offset);
guint32 read_psd_u32(PsdReader *reader, gsize offset);
gint16 read_i16(PsdReader *reader, gsize offset);
gint32 read_i32(PsdReader *reader, gsize offset);
gint64 read_large(PsdReader *reader, gsize offset);
gint64 read_length(PsdReader *reader, gsize offset);
double read_double(PsdReader *reader, gsize offset);
double read_float(PsdReader *reader, gsize offset);
double read_fixed(PsdReader *reader, gsize offset);
void read_key(PsdReader *reader, gsize offset, char key[5]);
gboolean is_listed_key(const char *list, const char *key);
char *read_pascal(PsdReader *reader, gsize offset, gsize *length);
char *read_utf16(PsdReader *reader, gsize offset, gsize count);
char *read_unicode(PsdReader *reader, gsize offset);
Record *read_rectangle(PsdReader *reader, gsize offset);
Record *create_byte_array(PsdReader *reader, gsize offset, gsize length);
gsize read_layer_records(PsdReader *reader, gsize offset);
gsize read_tagged_blocks(PsdReader *reader, gsize offset, gsize end, Record *descriptors);
void read_adjustment_block(PsdReader *reader, const char *key, gsize offset, Record *descriptors);
gsize read_layer_and_mask_information(PsdReader *reader, gsize offset);
Record *decode_psd_channel(PsdReader *reader, unsigned depth, gsize width, gsize height, gsize offset, int compression,
						   gint64 length);
Record *read_psd_channel(PsdReader *reader, unsigned depth, gsize width, gsize height, gsize offset, gint64 length);
void interleave_psd_channel(const Record *channel, Record *target, unsigned position);
gsize read_psd_merged_image(PsdReader *reader, gsize offset, unsigned file_depth);
GByteArray *write_psd_document(Document *document, gboolean large, gboolean zip, GError **error);
Document *read_psd_document(const guint8 *data, gsize length, const char *name, GCancellable *cancellable,
							GError **error);
