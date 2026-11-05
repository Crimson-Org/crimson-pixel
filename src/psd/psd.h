#pragma once

#include "document/document.h"
#include "lang/en.h"

#define PSD_MAX_DECODED_BYTES (1024ULL * 1024 * 1024)

/* Photoshop files are big endian, their blocks are named by four character keys such as 8BIM, and a rectangle is
 * four four byte numbers: top, left, bottom and right */
#define PSD_KEY_LENGTH 4
#define PSD_RECTANGLE_BYTES 16
#define PSD_KEY_SIZE (PSD_KEY_LENGTH + 1)
#define PSD_GRAY_MODE 1
#define PSD_INDEXED_MODE 2

/* The file header: signature, version, six reserved bytes, channel count, height, width, depth and colour mode */
#define PSD_SIGNATURE "8BPS"
#define PSD_VERSION 1
#define PSB_VERSION 2
#define HEADER_VERSION 4
#define HEADER_RESERVED 6
#define HEADER_CHANNELS 12
#define HEADER_HEIGHT 14
#define HEADER_WIDTH 18
#define HEADER_DEPTH 22
#define HEADER_MODE 24
#define HEADER_BYTES 26

/* Image resources are signed 8BIM, except the two image ready ones, signed MeSa */
#define IMAGE_READY_FIRST 7000
#define IMAGE_READY_LAST 7001
#define IMAGE_READY_SIGNATURE "MeSa"
/* Descriptors in resources and blocks start with their version */
#define DESCRIPTOR_VERSION 16
#define DESCRIPTOR_AT sizeof(guint32)
/* Guides count in 32nds of a pixel; the guides resource has a version, then records of a size, the grid one has a
 * version, two cycles and a count, then records of a location and a direction */
#define UNITS_PER_PIXEL 32
#define GUIDES_VERSION 3
#define GUIDE_RECORD_BYTES 14
#define GRID_VERSION 1
#define GRID_CYCLE 576
#define GRID_COUNT_AT (3 * sizeof(guint32))
#define GRID_RECORD_BYTES 5

/* PackBits rows hold runs of up to 128 bytes, three equal bytes or more are worth a repeat, and a repeat's header
 * counts down from 257 */
#define PACKBITS_LONGEST_RUN 128
#define PACKBITS_SHORTEST_REPEAT 3
#define PACKBITS_REPEAT_BASE 257
/* Zip data has a two byte header and four bytes of room after it */
#define ZIP_HEADER_BYTES 2
#define ZIP_TRAILER_BYTES 4
/* Floats are predicted as separate byte planes */
#define FLOAT_BYTES 4
#define PSD_RGB_MODE 3

/* Tagged blocks and layer names are padded to four bytes, and most blocks are signed 8BIM, or 8B64 for large
 * lengths in large files */
#define BLOCK_ALIGNMENT 4
#define BLOCK_SIGNATURE "8BIM"
#define LARGE_BLOCK_SIGNATURE "8B64"
/* The keys of blocks whose length takes eight bytes in large files, a space between each */
extern const char *const LARGE_LENGTH_KEYS;

/* A layer record lists each channel's id and data length, then its blend mode signature and key, its opacity, its
 * clipping, its flags and a pad */
#define CHANNEL_INFO_BYTES 6
#define LARGE_CHANNEL_INFO_BYTES 10
#define MOST_LAYER_CHANNELS 6
#define LAYER_OPACITY_AT (2 * PSD_KEY_LENGTH)
#define LAYER_CLIPPING_AT (LAYER_OPACITY_AT + 1)
#define LAYER_FLAGS_AT (LAYER_OPACITY_AT + 2)
#define LAYER_BLEND_BYTES (LAYER_OPACITY_AT + 4)

/* Layer mask flags, and which parameters follow them */
enum { MASK_RELATIVE_TO_LAYER = 1, MASK_DISABLED = 2, MASK_FROM_OTHER_DATA = 8, MASK_HAS_PARAMETERS = 16 };
enum { MASK_USER_DENSITY = 1, MASK_USER_FEATHER = 2, MASK_VECTOR_DENSITY = 4, MASK_VECTOR_FEATHER = 8 };
/* A layer mask alone takes 20 bytes, and a second, real mask adds its flags, its colour and its rectangle */
#define PLAIN_MASK_LENGTH 20
#define REAL_MASK_BYTES (2 + PSD_RECTANGLE_BYTES)
/* The parameters are the user density and feather, then the vector density and feather */
#define MASK_PARAMETERS 4

/* How the samples of one channel are stored */
enum {
	PSD_RAW = 0,
	PSD_RLE = 1,
	PSD_ZIP = 2,
	PSD_ZIP_PREDICTED = 3,
};

/* Channel ids besides the colour channels 0 to 2 */
enum {
	PSD_TRANSPARENCY_CHANNEL = -1,
	PSD_USER_MASK_CHANNEL = -2,
	PSD_REAL_MASK_CHANNEL = -3,
};

/* Reading state for one in-memory Photoshop file */
typedef struct {
	const guint8 *data;
	gsize length;
	gboolean large, failed, lenient;
	GError **error;
	Document *document;
	GCancellable *cancellable;
	/* Layer records hold blocks of their own, which may never start reading layer records again, since that would
	 * free layers the outer read still holds */
	gboolean is_reading_layers;
	/* How deep the descriptor being read is nested, which a damaged file could make deep enough to end the program */
	guint descriptor_depth;
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
