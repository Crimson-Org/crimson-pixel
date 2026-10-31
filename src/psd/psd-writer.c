#include <limits.h>
#include <math.h>
#include <string.h>
#include <zlib.h>

#include "adjustments/adjustments.h"
#include "descriptor.h"
#include "tools/kernels.h"
#include "document/blend.h"
#include "psd.h"
#include "psd-output.h"

/* PackBits rows hold runs of up to 128 bytes, three equal bytes or more are worth a repeat, and a repeat's header
 * counts down from 257 */
#define PACKBITS_LONGEST_RUN 128
#define PACKBITS_SHORTEST_REPEAT 3
#define PACKBITS_REPEAT_BASE 257
/* Zip data starts with the zlib header 78 9C, is deflated with zlib's usual memory level, and ends with four bytes of
 * room that earlier files also have */
#define ZLIB_HEADER_FIRST 0x78
#define ZLIB_HEADER_SECOND 0x9C
#define ZLIB_MEMORY_LEVEL 8
#define DEFLATE_SLACK 64
#define ZIP_END_ROOM 4
/* Tagged blocks and layer names are padded to four bytes, and most blocks are signed 8BIM, or 8B64 for large
 * lengths in large files */
#define BLOCK_ALIGNMENT 4
#define BLOCK_SIGNATURE "8BIM"
#define LARGE_BLOCK_SIGNATURE "8B64"
/* A layer record lists each channel's id and data length, and keeps a byte for each blending range value */
#define CHANNEL_INFO_BYTES 6
#define LARGE_CHANNEL_INFO_BYTES 10
#define MOST_LAYER_CHANNELS 6
#define BLENDING_RANGES_BYTES BLEND_RANGE_VALUES
/* Mask flags, and the parameters that follow: user density and user feather */
#define MASK_RELATIVE_TO_LAYER 1
#define MASK_DISABLED 2
#define MASK_FROM_OTHER_DATA 8
#define MASK_HAS_PARAMETERS 16
#define MASK_DENSITY_AND_FEATHER 3
/* Delta prediction works on samples a byte apart, wrapping around a byte or a sixteen bit range */
#define BYTE_RANGE (G_MAXUINT8 + 1)
#define WORD_RANGE (G_MAXUINT16 + 1)
#define FLOAT_BYTES 4
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
/* Image resources, most signed 8BIM and the two image ready ones MeSa */
#define IMAGE_READY_FIRST 7000
#define IMAGE_READY_LAST 7001
#define IMAGE_READY_SIGNATURE "MeSa"
#define FIRST_PATH_RESOURCE 2000
#define LAST_PATH_RESOURCE 2016
/* Descriptor resources start with their version, and slices with one more before it */
#define DESCRIPTOR_VERSION 16
#define SLICES_VERSION 8
/* Resolution is a 16.16 fixed point number per axis, in pixels per inch, shown in centimetres */
#define FIXED_POINT_ONE WORD_RANGE
#define RESOLUTION_AXES 2
#define PIXELS_PER_INCH 1
#define SHOWN_IN_CENTIMETRES 2
/* Guides and the grid count in 32nds of a pixel, and every guide record carries the same version, size and ending */
#define GUIDES_VERSION 3
#define GUIDE_RECORD_BYTES 14
#define GUIDE_AXES 2
#define GUIDE_ENDING G_MAXUINT16
#define GUIDE_FLAGS 8
#define GRID_VERSION 1
#define GRID_CYCLE 576
#define UNITS_PER_PIXEL 32
/* Slice ids count from 2 in steps of 3, and sheets and the timeline name their versions */
#define FIRST_SLICE_ID 2
#define SLICE_ID_STEP 3
#define SHEET_VERSION 2
#define TIMELINE_VERSION 1
/* Transparency is checked on whole runs of eight pixels */
#define TRANSPARENCY_BLOCK_BYTES 32

void ensure_size(PsdWriter *writer, gsize size) {
	gsize previous = writer->data->len;
	if (size <= previous)
		return;
	g_byte_array_set_size(writer->data, size);
	memset(writer->data->data + previous, 0, size - previous);
}

void put_byte(PsdWriter *writer, gsize offset, guint value) {
	ensure_size(writer, offset + 1);
	writer->data->data[offset] = value;
}

void put_u16(PsdWriter *writer, gsize offset, guint value) {
	put_byte(writer, offset, value >> CHAR_BIT & G_MAXUINT8);
	put_byte(writer, offset + 1, value & G_MAXUINT8);
}

void put_u32(PsdWriter *writer, gsize offset, double value) {
	guint32 bits = isfinite(value) ? (guint32)(gint64)value : 0;
	for (int index = 0; index < (int)sizeof bits; index++)
		put_byte(writer, offset + index, bits >> (((int)sizeof bits - 1 - index) * CHAR_BIT) & G_MAXUINT8);
}

static void put_large(PsdWriter *writer, gsize offset, double value) {
	/* Shifting twice by half keeps the shift defined for the 32 bit value */
	gint32 high = (gint32)(gint64)value >> (sizeof(guint16) * CHAR_BIT) >> (sizeof(guint16) * CHAR_BIT);
	put_u32(writer, offset, high);
	put_u32(writer, offset + sizeof(guint32), (double)(guint32)(gint64)value);
}

static void put_ascii(PsdWriter *writer, gsize offset, const char *text) {
	for (gsize index = 0; text[index]; index++)
		put_byte(writer, offset + index, (guint8)text[index]);
}

static void put_double(PsdWriter *writer, gsize offset, double value) {
	guint64 bits;
	memcpy(&bits, &value, sizeof bits);
	for (int index = 0; index < (int)sizeof bits; index++)
		put_byte(writer, offset + index, bits >> (((int)sizeof bits - 1 - index) * CHAR_BIT) & G_MAXUINT8);
}

void put_float(PsdWriter *writer, gsize offset, double value) {
	float single = value;
	guint32 bits;
	memcpy(&bits, &single, sizeof bits);
	for (int index = 0; index < (int)sizeof bits; index++)
		put_byte(writer, offset + index, bits >> (((int)sizeof bits - 1 - index) * CHAR_BIT) & G_MAXUINT8);
}

gsize put_bytes(PsdWriter *writer, gsize offset, const guint8 *bytes, gsize length) {
	ensure_size(writer, offset + length);
	memcpy(writer->data->data + offset, bytes, length);
	return length;
}

gsize put_counted_text(PsdWriter *writer, gsize offset, const char *text) {
	glong units = 0;
	g_autofree gunichar2 *utf16 = g_utf8_to_utf16(text, -1, NULL, &units, NULL);
	put_u32(writer, offset, units);
	for (glong index = 0; index < units; index++)
		put_u16(writer, offset + sizeof(guint32) + index * sizeof(guint16), utf16[index]);
	return sizeof(guint32) + units * sizeof(guint16);
}

void put_i16(PsdWriter *writer, gsize offset, double value) {
	put_u16(writer, offset, (guint16)wrap_uint32(value));
}

static void put_rectangle(PsdWriter *writer, gsize offset, RasterExtent extent) {
	put_u32(writer, offset, extent.y);
	put_u32(writer, offset + sizeof(guint32), extent.x);
	put_u32(writer, offset + 2 * sizeof(guint32), extent.y + extent.height);
	put_u32(writer, offset + 3 * sizeof(guint32), extent.x + extent.width);
}

static gsize put_pascal(PsdWriter *writer, gsize offset, const char *text) {
	glong units = 0;
	g_autofree gunichar2 *utf16 = g_utf8_to_utf16(text, -1, NULL, &units, NULL);
	gsize length = MIN((gsize)units, G_MAXUINT8);
	ensure_size(writer, offset + length + 2);
	put_byte(writer, offset, length);
	for (gsize index = 0; index < length; index++)
		put_byte(writer, offset + 1 + index, utf16[index] & G_MAXUINT8);
	if (length % 2 == 0) {
		put_byte(writer, offset + 1 + length, 0);
		length++;
	}
	return length + 1;
}

/* ============================== Channel encoding ================================= */

static gsize pack_row(const guint8 *source, gsize start, gsize count, PsdWriter *writer, gsize offset) {
	gsize position = start, end = start + count, out = offset;
	while (count > 0) {
		gsize limit = count < PACKBITS_LONGEST_RUN ? count : PACKBITS_LONGEST_RUN, next, run;
		gboolean repeated = position + PACKBITS_SHORTEST_REPEAT <= end && source[position + 1] == source[position]
			&& source[position + 2] == source[position];
		if (repeated) {
			for (next = position + PACKBITS_SHORTEST_REPEAT; next < position + limit && source[next] == source[position];)
				next++;
			run = next - position;
			put_byte(writer, out++, PACKBITS_REPEAT_BASE - run);
			put_byte(writer, out++, source[position]);
		} else {
			for (next = position; next < position + limit;) {
				if (next + PACKBITS_SHORTEST_REPEAT <= end && source[next + 1] == source[next] && source[next + 2] == source[next])
					break;
				next++;
			}
			run = next - position;
			put_byte(writer, out++, run - 1);
			put_bytes(writer, out, source + position, run);
			out += run;
		}
		position = next;
		count -= run;
	}
	return out - offset;
}

static gsize pack_rows(const guint8 *source, PsdWriter *writer, gsize row_bytes, gsize rows, gsize counts, gsize offset,
					   gsize count_size) {
	gsize start = offset;
	for (gsize row = 0; row < rows; row++) {
		gsize packed = pack_row(source, row * row_bytes, row_bytes, writer, offset);
		if (count_size == sizeof(guint16))
			put_u16(writer, counts + row * sizeof(guint16), packed);
		else
			put_u32(writer, counts + row * sizeof(guint32), packed);
		offset += packed;
	}
	return offset - start;
}

static void predict_samples(guint8 *bytes, gsize width, gsize height, unsigned depth) {
	if (depth == BYTE_DEPTH) {
		for (gsize row = 0; row < height; row++)
			for (gsize index = row * width + width - 1; index > row * width; index--)
				bytes[index] = (bytes[index] - bytes[index - 1] + BYTE_RANGE) & G_MAXUINT8;
	} else if (depth == WORD_DEPTH) {
		for (gsize row = 0; row < height; row++) {
			gsize start = row * width;
			for (gsize index = start + width - 1; index > start; index--) {
				gsize at = index << 1;
				guint value = ((bytes[at + 1] << CHAR_BIT | bytes[at]) - (bytes[at - 1] << CHAR_BIT | bytes[at - 2]) + WORD_RANGE)
					& G_MAXUINT16;
				bytes[at] = value >> CHAR_BIT;
				bytes[at + 1] = value & G_MAXUINT8;
			}
			guint8 swap = bytes[start * 2];
			bytes[start * 2] = bytes[start * 2 + 1];
			bytes[start * 2 + 1] = swap;
		}
	} else {
		/* Floats are split into byte planes, the most significant byte first, before the delta runs across them */
		g_autofree guint8 *planes = g_malloc(MAX(width * FLOAT_BYTES, 1));
		for (gsize row = 0; row < height; row++) {
			guint8 *line = bytes + row * width * FLOAT_BYTES;
			for (gsize index = 0; index < width; index++)
				for (gsize plane = 0; plane < FLOAT_BYTES; plane++)
					planes[width * plane + index] = line[FLOAT_BYTES * index + FLOAT_BYTES - 1 - plane];
			for (gsize index = width * FLOAT_BYTES - 1; index > 0; index--)
				planes[index] = (planes[index] - planes[index - 1] + BYTE_RANGE) & G_MAXUINT8;
			memcpy(line, planes, width * FLOAT_BYTES);
		}
	}
}

static gsize encode_samples(PsdWriter *writer, const Record *samples, gsize width, gsize height, gsize offset,
							int compression) {
	unsigned depth = samples->value.buffer.depth;
	gsize count = width * height, sample_bytes = depth / CHAR_BIT;
	g_autofree guint8 *bytes = g_malloc0(MAX(count * sample_bytes, 1));
	if (samples->value.buffer.length)
		memcpy(bytes, samples->value.buffer.data, MIN(count, samples->value.buffer.length) * sample_bytes);
	if (compression == PSD_RLE) {
		gsize count_size = writer->large ? sizeof(guint32) : sizeof(guint16);
		gsize packed = pack_rows(bytes, writer, width, height, offset, offset + count_size * height, count_size);
		return offset + count_size * height + packed;
	}
	if (compression == PSD_ZIP_PREDICTED)
		predict_samples(bytes, width, height, depth);
	put_byte(writer, offset, ZLIB_HEADER_FIRST);
	put_byte(writer, offset + 1, ZLIB_HEADER_SECOND);
	offset += sizeof(guint16);
	uLongf capacity = compressBound(count * sample_bytes) + DEFLATE_SLACK;
	g_autofree guint8 *packed = g_malloc(capacity);
	z_stream stream = {0};
	/* The default level gives the same bytes as files saved before */
	deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, ZLIB_MEMORY_LEVEL, Z_DEFAULT_STRATEGY);
	stream.next_in = bytes;
	stream.avail_in = count * sample_bytes;
	stream.next_out = packed;
	stream.avail_out = capacity;
	deflate(&stream, Z_FINISH);
	gsize length = capacity - stream.avail_out;
	deflateEnd(&stream);
	put_bytes(writer, offset, packed, length);
	ensure_size(writer, offset + length + ZIP_END_ROOM);
	return offset + length + ZIP_END_ROOM;
}

static gsize encode_channel(PsdWriter *writer, const Record *samples, gsize width, gsize height, gsize offset,
							int compression) {
	put_u16(writer, offset, compression);
	return encode_samples(writer, samples, width, height, offset + sizeof(guint16), compression);
}

static Record *extract_channel(const Record *pixels, gsize count, unsigned position) {
	Record *channel = create_channels(count, pixels->value.buffer.depth);
	gsize available = MIN(count, pixels->value.buffer.length / RGBA_CHANNELS);
	for (gsize index = 0; index < available; index++)
		write_channel(channel, index, read_channel(pixels, index * RGBA_CHANNELS + position));
	return channel;
}

/* ============================== Tagged blocks ==================================== */

static const char *LARGE_LENGTH_KEYS =
	"LMsk Lr16 Lr32 Layr Mt16 Mt32 Mtrn Alph FMsk lnk2 lnkE FEid FXid PxSD extn cinf artd pths";

static gboolean is_listed(const char *list, const char *key) {
	/* The keys are listed with a space between them */
	for (const char *position = list;; position += PSD_KEY_LENGTH + 1) {
		if (!strncmp(position, key, PSD_KEY_LENGTH))
			return TRUE;
		if (!position[PSD_KEY_LENGTH])
			return FALSE;
	}
}

static gsize write_layer_records(PsdWriter *writer, Record *document_record, gsize offset, gboolean zip,
								 gboolean merged);

static gsize write_tagged_blocks(PsdWriter *writer, Record *document_record, gsize offset, Record *descriptors) {
	g_autoptr(GPtrArray) keys = get_record_keys(descriptors);
	for (guint index = 0; index < keys->len && !writer->failed; index++) {
		const char *key = g_ptr_array_index(keys, index);
		Record *value = get_record(descriptors, key);
		gboolean large = writer->large && is_listed(LARGE_LENGTH_KEYS, key);
		gsize start = offset;
		put_ascii(writer, offset, large ? LARGE_BLOCK_SIGNATURE : BLOCK_SIGNATURE);
		offset += PSD_KEY_LENGTH;
		put_ascii(writer, offset, key);
		offset += PSD_KEY_LENGTH;
		put_u32(writer, offset, 0);
		offset += large ? sizeof(guint64) : sizeof(guint32);
		gsize size = 0;
		if (!strcmp(key, "iOpa") || !strcmp(key, "knko") || !strcmp(key, "infx") || !strcmp(key, "clbl") ||
			!strcmp(key, "lmgm") || !strcmp(key, "vmgm") || !strcmp(key, "fcmy")) {
			put_byte(writer, offset, value->value.number);
			size = BLOCK_ALIGNMENT;
		} else if (!strcmp(key, "brst")) {
			for (int channel = 0; channel < RGB_CHANNELS; channel++)
				if (get_item(value, channel) && get_item(value, channel)->value.number == 0) {
					put_u32(writer, offset + size, channel);
					size += sizeof(guint32);
				}
		} else if (!strcmp(key, "lyid") || !strcmp(key, "lyvr") || !strcmp(key, "lspf")) {
			put_u32(writer, offset, value->value.number);
			size = sizeof(guint32);
		} else if (!strcmp(key, "lsct")) {
			/* The section type, then its blend mode signature and key, then whether the group is grouped */
			put_u32(writer, offset, get_number(value, "J"));
			size = sizeof(guint32);
			if (get_string(value, "qu")) {
				put_ascii(writer, offset + size, BLOCK_SIGNATURE);
				put_ascii(writer, offset + size + PSD_KEY_LENGTH, get_string(value, "qu"));
				size += 2 * PSD_KEY_LENGTH;
				Record *grouped = get_record(value, "Zd");
				if (grouped && grouped->type != RECORD_NULL) {
					put_u32(writer, offset + size, grouped->value.boolean ? 1 : 0);
					size += sizeof(guint32);
				}
			}
		} else if (!strcmp(key, "lnsr")) {
			put_ascii(writer, offset, value->value.string);
			size = PSD_KEY_LENGTH;
		} else if (!strcmp(key, "lclr")) {
			/* A colour label is one sixteen bit number in eight bytes */
			put_u16(writer, offset, value->value.number);
			ensure_size(writer, offset + sizeof(guint32));
			size = sizeof(guint64);
		} else if (!strcmp(key, "luni"))
			size = put_counted_text(writer, offset, value->value.string);
		else if (!strcmp(key, "fxrp")) {
			put_double(writer, offset, get_number(value, "x"));
			put_double(writer, offset + sizeof(double), get_number(value, "y"));
			size = 2 * sizeof(double);
		} else if (!strcmp(key, "Lr16") || !strcmp(key, "Lr32"))
			size = write_layer_records(writer, document_record, offset, TRUE, FALSE) - offset;
		else if (is_ported_adjustment(key))
			size = put_adjustment(writer, offset, key, value);
		else if (!strcmp(key, "CgEd"))
			size = put_brightness_settings(writer, offset, value);
		else {
			offset = start;
			continue;
		}
		if (size % BLOCK_ALIGNMENT)
			size += BLOCK_ALIGNMENT - size % BLOCK_ALIGNMENT;
		if (large)
			put_large(writer, offset - sizeof(guint64), size);
		else
			put_u32(writer, offset - sizeof(guint32), size);
		ensure_size(writer, offset + size);
		offset += size;
	}
	return offset;
}

/* ============================== Layer records ==================================== */

static RasterExtent record_extent(const Record *object) {
	return read_extent(get_record(object, "V"));
}

static gboolean has_object(const Record *owner, const char *key) {
	Record *value = get_record(owner, key);
	return value && value->type == RECORD_OBJECT;
}

static void put_mask_flags(PsdWriter *writer, gsize offset, const Record *mask, gboolean parameters) {
	guint flags = 0;
	if (!get_boolean(mask, "WE"))
		flags += MASK_RELATIVE_TO_LAYER;
	if (!get_boolean(mask, "isEnabled"))
		flags += MASK_DISABLED;
	if (get_boolean(mask, "L7"))
		flags += MASK_FROM_OTHER_DATA;
	if (parameters)
		flags += MASK_HAS_PARAMETERS;
	put_byte(writer, offset, flags);
}

static Record *get_writable_mask(const Record *layer) {
	if (has_object(layer, "E_"))
		return get_record(layer, "E_");
	Record *vector = get_record(get_record(layer, "add"), "vmsk");
	if (vector && vector->type == RECORD_OBJECT && get_boolean(vector, "isEnabled"))
		return NULL;
	return has_object(layer, "yy") ? get_record(layer, "yy") : NULL;
}

static gsize write_layer_mask(PsdWriter *writer, const Record *layer, gsize offset) {
	put_u32(writer, offset, 0);
	offset += sizeof(guint32);
	if (!has_object(layer, "yy"))
		return offset;
	Record *mask = get_record(layer, "yy"), *parameters = get_writable_mask(layer);
	gsize start = offset;
	put_u32(writer, offset, 0);
	put_rectangle(writer, offset, record_extent(mask));
	offset += PSD_RECTANGLE_BYTES;
	put_byte(writer, offset++, get_number(mask, "color"));
	put_mask_flags(writer, offset++, mask, TRUE);
	if (has_object(layer, "E_")) {
		Record *real = get_record(layer, "E_");
		put_mask_flags(writer, offset, real, FALSE);
		put_byte(writer, offset + 1, get_number(real, "color"));
		put_rectangle(writer, offset + 2, record_extent(real));
		offset += 2 + PSD_RECTANGLE_BYTES;
	}
	gsize flags_start = offset;
	put_byte(writer, offset++, MASK_DENSITY_AND_FEATHER);
	put_byte(writer, offset++, parameters ? get_number(parameters, "mU") : G_MAXUINT8);
	put_double(writer, offset, parameters ? get_number(parameters, "O1") : 0);
	offset += sizeof(double);
	if ((offset - flags_start) & 1)
		offset++;
	ensure_size(writer, offset);
	put_u32(writer, start - sizeof(guint32), offset - start);
	return offset;
}

static int list_layer_channels(const Record *layer, int channels[MOST_LAYER_CHANNELS]) {
	int count = 0;
	channels[count++] = PSD_TRANSPARENCY_CHANNEL;
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		channels[count++] = channel;
	if (has_object(layer, "yy"))
		channels[count++] = PSD_USER_MASK_CHANNEL;
	if (has_object(layer, "E_"))
		channels[count++] = PSD_REAL_MASK_CHANNEL;
	return count;
}

static gsize write_layer_record(PsdWriter *writer, Record *document_record, Record *layer, gsize offset,
								gsize *channel_info) {
	put_rectangle(writer, offset, record_extent(layer));
	offset += PSD_RECTANGLE_BYTES;
	int channels[MOST_LAYER_CHANNELS], count = list_layer_channels(layer, channels);
	put_u16(writer, offset, count);
	offset += sizeof(guint16);
	*channel_info = offset;
	for (int index = 0; index < count; index++) {
		put_u16(writer, offset, (guint16)channels[index]);
		if (writer->large)
			put_large(writer, offset + sizeof(guint16), 0);
		else
			put_u32(writer, offset + sizeof(guint16), 0);
		offset += writer->large ? LARGE_CHANNEL_INFO_BYTES : CHANNEL_INFO_BYTES;
	}
	put_ascii(writer, offset, BLOCK_SIGNATURE);
	put_ascii(writer, offset + PSD_KEY_LENGTH, get_string(layer, "qu"));
	offset += 2 * PSD_KEY_LENGTH;
	put_byte(writer, offset++, get_number(layer, "nv"));
	Record *first = get_item(get_record(document_record, "Z"), 0);
	put_byte(writer, offset++, get_boolean(layer, "jl") && first != layer ? 1 : 0);
	put_byte(writer, offset++, get_number(layer, "NF"));
	put_byte(writer, offset++, 0);
	gsize length_offset = offset;
	put_u32(writer, offset, 0);
	offset += sizeof(guint32);
	gsize extra_start = offset;
	offset = write_layer_mask(writer, layer, offset);
	Record *ranges = get_record(layer, "hu");
	put_u32(writer, offset, count_records(ranges));
	offset += sizeof(guint32);
	ensure_size(writer, offset + BLENDING_RANGES_BYTES);
	for (int index = 0; index < BLENDING_RANGES_BYTES; index++)
		put_byte(writer, offset + index, (guint)get_item(ranges, index)->value.number);
	offset += BLENDING_RANGES_BYTES;
	const char *name = get_string(layer, "name");
	gsize name_size = put_pascal(writer, offset, name ? name : "");
	offset += name_size;
	if (name_size % BLOCK_ALIGNMENT)
		offset += BLOCK_ALIGNMENT - name_size % BLOCK_ALIGNMENT;
	Record *descriptors = get_record(layer, "add"), *section = get_record(descriptors, "lsct");
	gboolean present = section != NULL;
	gboolean truthy = section && section->type == RECORD_NUMBER && section->value.number != 0;
	g_autoptr(Record) kept = section ? reference_record(section) : NULL;
	Record *grouped = get_record(layer, "Zd");
	if (truthy || !g_strcmp0(get_string(layer, "qu"), "pass")) {
		Record *wrapped = create_object(RECORD_PLAIN);
		set_number(wrapped, "J", section ? section->value.number : 0);
		if (!truthy)
			set_string(wrapped, "qu", "pass");
		set_record(wrapped, "Zd", grouped ? copy_record(grouped) : create_null());
		if (truthy && !g_strcmp0(get_string(layer, "qu"), "pass"))
			set_string(wrapped, "qu", "pass");
		set_record(descriptors, "lsct", wrapped);
	}
	Record *brightness = get_record(descriptors, "brit");
	if (brightness)
		set_record(descriptors, "CgEd", reference_record(brightness));
	offset = write_tagged_blocks(writer, document_record, offset, descriptors);
	if (brightness)
		remove_record(descriptors, "CgEd");
	if (present)
		set_record(descriptors, "lsct", reference_record(kept));
	else
		remove_record(descriptors, "lsct");
	put_u32(writer, length_offset, offset - extra_start);
	return offset;
}

static gsize write_layer_channels(PsdWriter *writer, Record *layer, gsize offset, gsize channel_info, gboolean zip) {
	int channels[MOST_LAYER_CHANNELS], count = list_layer_channels(layer, channels);
	Record *pixels = get_record(layer, "buffer");
	RasterExtent extent = record_extent(layer);
	unsigned depth = pixels->value.buffer.depth;
	int compression = depth != BYTE_DEPTH || zip ? PSD_ZIP_PREDICTED : PSD_RLE;
	for (int index = 0; index < count; index++) {
		int identifier = channels[index];
		const Record *target = identifier == PSD_REAL_MASK_CHANNEL	  ? get_record(layer, "E_")
							   : identifier == PSD_USER_MASK_CHANNEL ? get_record(layer, "yy")
																	 : layer;
		RasterExtent channel_extent = record_extent(target);
		g_autoptr(Record) samples = identifier >= PSD_TRANSPARENCY_CHANNEL
										? extract_channel(pixels, (gsize)MAX(0, extent.width) * MAX(0, extent.height),
														  identifier == PSD_TRANSPARENCY_CHANNEL ? ALPHA_CHANNEL : identifier)
										: reference_record(get_record(target, "C"));
		gsize start = offset;
		offset = encode_channel(writer, samples, MAX(0, channel_extent.width), MAX(0, channel_extent.height), offset,
								compression);
		if (writer->large)
			put_large(writer, channel_info + index * LARGE_CHANNEL_INFO_BYTES + sizeof(guint16), offset - start);
		else
			put_u32(writer, channel_info + index * CHANNEL_INFO_BYTES + sizeof(guint16), offset - start);
	}
	return offset;
}

static gsize write_layer_records(PsdWriter *writer, Record *document_record, gsize offset, gboolean zip,
								 gboolean merged) {
	Record *layers = get_record(document_record, "Z");
	guint count = count_records(layers);
	put_u16(writer, offset, merged ? (guint16) - (gint)count : count);
	offset += sizeof(guint16);
	g_autofree gsize *channel_info = g_new0(gsize, MAX(count, 1));
	for (guint index = 0; index < count; index++)
		offset = write_layer_record(writer, document_record, get_item(layers, index), offset, &channel_info[index]);
	for (guint index = 0; index < count; index++)
		offset = write_layer_channels(writer, get_item(layers, index), offset, channel_info[index], zip);
	return offset;
}

static gsize write_layer_and_mask_information(PsdWriter *writer, Record *document_record, gsize offset, gboolean zip,
											  gboolean transparency) {
	gsize start = offset, length_size = writer->large ? sizeof(guint64) : sizeof(guint32);
	put_u32(writer, offset, 0);
	offset += sizeof(guint32);
	if (writer->large) {
		put_u32(writer, offset, 0);
		offset += sizeof(guint32);
	}
	gsize layers_start = offset;
	offset += length_size;
	unsigned depth = get_number(document_record, "depth");
	if (depth == BYTE_DEPTH)
		offset = write_layer_records(writer, document_record, offset, zip, transparency);
	gsize layers_length = offset - layers_start - length_size;
	if (layers_length % 2)
		layers_length++;
	if (writer->large)
		put_large(writer, layers_start, layers_length);
	else
		put_u32(writer, layers_start, layers_length);
	offset = layers_start + length_size + layers_length;
	ensure_size(writer, offset);
	put_u32(writer, offset, 0);
	offset += sizeof(guint32);
	Record *descriptors = get_record(document_record, "add");
	g_autoptr(Record) blocks = NULL;
	if (depth != BYTE_DEPTH) {
		/* Deeper documents put their layers first in the document level blocks */
		blocks = create_object(RECORD_PLAIN);
		g_autofree char *key = g_strdup_printf("Lr%u", depth);
		set_record(blocks, key, create_object(RECORD_PLAIN));
		g_autoptr(GPtrArray) keys = get_record_keys(descriptors);
		for (guint index = 0; index < keys->len; index++)
			set_record(blocks, g_ptr_array_index(keys, index),
					   reference_record(get_record(descriptors, g_ptr_array_index(keys, index))));
		descriptors = blocks;
	}
	offset = write_tagged_blocks(writer, document_record, offset, descriptors);
	if (writer->large)
		put_large(writer, start, offset - start - sizeof(guint64));
	else
		put_u32(writer, start, offset - start - sizeof(guint32));
	return offset;
}

/* ============================== Resources and merged image ======================= */

/* The display info resource, with and without the transparency entry */
static const guint8 TRANSPARENT_DISPLAY_INFO[] = {0, 0, 0, 1, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 100, 1};
static const guint8 DISPLAY_INFO[] = {0, 0, 0, 1};

static void set_resource(Record *resources, const char *key, GByteArray *bytes) {
	set_record(resources, key, create_bytes(bytes->data, bytes->len));
}

static GByteArray *encode_descriptor_resource(const Record *descriptor, gboolean slices) {
	GByteArray *bytes = g_byte_array_new();
	if (slices)
		write_u32(bytes, SLICES_VERSION);
	write_u32(bytes, DESCRIPTOR_VERSION);
	write_descriptor(bytes, descriptor);
	return bytes;
}

static void prepare_resources(Record *document_record, gboolean transparency) {
	Record *resources = get_record(document_record, "Pz"), *layers = get_record(document_record, "Z");
	g_autoptr(GByteArray) bytes = g_byte_array_new();
	double resolution = get_number(document_record, "Qf");
	for (int axis = 0; axis < RESOLUTION_AXES; axis++) {
		write_u16(bytes, floor(resolution));
		write_u16(bytes, floor((resolution - floor(resolution)) * FIXED_POINT_ONE));
		write_u16(bytes, PIXELS_PER_INCH);
		write_u16(bytes, SHOWN_IN_CENTIMETRES);
	}
	set_resource(resources, "r1005", bytes);
	Record *guides = get_record(document_record, "Jd");
	if (count_records(guides)) {
		g_byte_array_set_size(bytes, 0);
		write_u32(bytes, GUIDES_VERSION);
		write_u32(bytes, count_records(guides));
		for (guint index = 0; index < count_records(guides); index++) {
			Record *guide = get_item(guides, index);
			write_u16(bytes, GUIDE_RECORD_BYTES);
			for (int axis = 0; axis < GUIDE_AXES; axis++) {
				float value = get_number(guide, axis ? "x" : "y");
				guint32 bits;
				memcpy(&bits, &value, sizeof bits);
				write_u32(bytes, (double)(gint32)bits);
			}
			write_u16(bytes, GUIDE_ENDING);
			write_u16(bytes, GUIDE_FLAGS);
		}
		set_resource(resources, "r1073", bytes);
	}
	remove_record(resources, "r1022");
	g_byte_array_set_size(bytes, 0);
	g_autoptr(GByteArray) locks = g_byte_array_new();
	double highest_color = 0;
	gboolean all_locked = TRUE;
	for (guint index = 0; index < count_records(layers); index++) {
		Record *layer = get_item(layers, index);
		double color = get_number(layer, "kr");
		write_u16(bytes, color);
		write_u8(locks, get_boolean(layer, "nQ") ? 1 : 0);
		highest_color = MAX(highest_color, color);
		if (!get_boolean(layer, "nQ"))
			all_locked = FALSE;
	}
	if (highest_color > 0)
		set_resource(resources, "r1026", bytes);
	if (!all_locked)
		set_resource(resources, "r1072", locks);
	g_byte_array_set_size(bytes, 0);
	Record *grid = get_record(document_record, "LM");
	write_u32(bytes, GRID_VERSION);
	write_u32(bytes, GRID_CYCLE);
	write_u32(bytes, GRID_CYCLE);
	write_u32(bytes, count_records(grid));
	for (guint index = 0; index < count_records(grid); index++) {
		Record *line = get_item(grid, index);
		write_u32(bytes, round_half_up(get_item(line, 1)->value.number * UNITS_PER_PIXEL));
		write_u8(bytes, get_item(line, 0)->value.number);
	}
	set_resource(resources, "r1032", bytes);
	remove_record(resources, "r1045");
	remove_record(resources, "r1006");
	remove_record(resources, "r1077");
	if (transparency) {
		g_byte_array_set_size(bytes, 0);
		write_counted_text(bytes, "Transparency", TRUE);
		set_resource(resources, "r1045", bytes);
		set_record(resources, "r1077", create_bytes(TRANSPARENT_DISPLAY_INFO, sizeof TRANSPARENT_DISPLAY_INFO));
	} else {
		set_record(resources, "r1077", create_bytes(DISPLAY_INFO, sizeof DISPLAY_INFO));
	}
	Record *slices = get_record(document_record, "Xd");
	for (guint index = 0; index < count_records(slices); index++) {
		Record *slice = get_record(get_item(slices, count_records(slices) - index - 1), "v");
		set_number(get_record(slice, "sliceID"), "v", FIRST_SLICE_ID + index * SLICE_ID_STEP);
	}
	g_autoptr(Record) slice_descriptor = create_object(RECORD_PLAIN);
	set_string(slice_descriptor, "classID", "null");
	set_record(slice_descriptor, "baseName", create_typed_value("TEXT", create_string("User")));
	Record *bounds = create_object(RECORD_PLAIN);
	set_string(bounds, "classID", "Rct1");
	const char *sides[] = {"Top ", "Left", "Btom", "Rght"};
	for (guint side = 0; side < G_N_ELEMENTS(sides); side++)
		set_record(bounds, sides[side], create_typed_value("long", create_number(0)));
	set_record(slice_descriptor, "bounds", create_typed_value("Objc", bounds));
	set_record(slice_descriptor, "slices", create_typed_value("VlLs", copy_record(slices)));
	g_autoptr(GByteArray) slice_bytes = encode_descriptor_resource(slice_descriptor, TRUE);
	set_resource(resources, "r1050", slice_bytes);
	g_autoptr(GByteArray) comps = encode_descriptor_resource(get_record(document_record, "gL"), FALSE);
	set_resource(resources, "r1065", comps);
	Record *sheets = create_array();
	for (guint index = 0; index < count_records(layers); index++) {
		Record *layer = get_item(layers, index), *sheet = create_object(RECORD_PLAIN);
		set_string(sheet, "classID", "null");
		set_record(sheet, "Vrsn", create_typed_value("long", create_number(SHEET_VERSION)));
		set_record(sheet, "sheetID",
				   create_typed_value("long", create_number(get_number(get_record(layer, "add"), "lyid"))));
		set_record(sheet, "sheetDisclosed",
				   create_typed_value("bool", create_boolean(get_item(get_record(layer, "md"), 0)->value.boolean)));
		append_record(sheets, create_typed_value("Objc", sheet));
	}
	if (count_records(sheets)) {
		g_autoptr(Record) options = create_object(RECORD_PLAIN);
		set_string(options, "classID", "null");
		set_record(options, "Vrsn", create_typed_value("long", create_number(TIMELINE_VERSION)));
		set_record(options, "sheetTimelineOptions", create_typed_value("VlLs", sheets));
		g_autoptr(GByteArray) sheet_bytes = encode_descriptor_resource(options, FALSE);
		set_resource(resources, "r1076", sheet_bytes);
	} else
		free_record(sheets);
	g_byte_array_set_size(bytes, 0);
	Record *selected = get_record(document_record, "j");
	write_u16(bytes, count_records(selected));
	for (guint index = 0; index < count_records(selected); index++) {
		Record *layer = get_item(layers, get_item(selected, index)->value.number);
		write_u32(bytes, get_number(get_record(layer, "add"), "lyid"));
	}
	set_resource(resources, "r1069", bytes);
	remove_record(resources, "r1058");
	remove_record(resources, "r1060");
	remove_record(get_record(document_record, "add"), "pths");
	remove_record(resources, "r3000");
}

static gint compare_resource_identifiers(gconstpointer first, gconstpointer second) {
	int a = atoi(*(const char *const *)first + 1), b = atoi(*(const char *const *)second + 1);
	return a < b ? -1 : a > b;
}

static gsize write_resources(PsdWriter *writer, Record *document_record, gsize offset) {
	gsize start = offset, size = 0;
	offset += sizeof(guint32);
	Record *resources = get_record(document_record, "Pz");
	g_autoptr(GPtrArray) keys = get_record_keys(resources);
	g_ptr_array_sort(keys, compare_resource_identifiers);
	for (guint index = 0; index < keys->len; index++) {
		const char *key = g_ptr_array_index(keys, index);
		int identifier = atoi(key + 1);
		gboolean is_image_ready = identifier == IMAGE_READY_FIRST || identifier == IMAGE_READY_LAST;
		put_ascii(writer, offset + size, is_image_ready ? IMAGE_READY_SIGNATURE : BLOCK_SIGNATURE);
		size += PSD_KEY_LENGTH;
		put_u16(writer, offset + size, identifier);
		size += sizeof(guint16);
		size += put_pascal(writer, offset + size, "");
		Record *bytes = get_record(resources, key);
		gsize length = bytes->value.buffer.length;
		put_u32(writer, offset + size, length);
		size += sizeof(guint32);
		size += put_bytes(writer, offset + size, bytes->value.buffer.data, length);
		if (length % 2 == 1)
			size++;
		ensure_size(writer, offset + size);
	}
	put_u32(writer, start, size);
	return offset + size;
}

static gboolean has_transparency(const Record *pixels) {
	const guint8 *bytes = pixels->value.buffer.data;
	gsize length = pixels->value.buffer.length;
	length -= length & (TRANSPARENCY_BLOCK_BYTES - 1);
	guint mask = G_MAXUINT8;
	for (gsize index = ALPHA_CHANNEL; index < length; index += TRANSPARENCY_BLOCK_BYTES)
		for (int step = 0; step < TRANSPARENCY_BLOCK_BYTES; step += RGBA_CHANNELS)
			mask &= bytes[index + step];
	return mask != G_MAXUINT8;
}

static gsize write_merged_image(PsdWriter *writer, Record *document_record, gsize offset, gboolean transparency) {
	put_u16(writer, offset, PSD_RLE);
	offset += sizeof(guint16);
	gsize width = get_number(document_record, "s"), height = get_number(document_record, "T"), area = width * height;
	unsigned depth = get_number(document_record, "depth");
	Record *pixels = get_record(document_record, "buffer");
	int count = RGB_CHANNELS + (transparency ? 1 : 0);
	g_autoptr(GPtrArray) channels = g_ptr_array_new_with_free_func((GDestroyNotify)free_record);
	if (depth == BYTE_DEPTH) {
		/* The merged image is drawn over white */
		for (int channel = 0; channel < RGB_CHANNELS; channel++) {
			Record *values = create_channels(area, BYTE_DEPTH);
			guint8 *target = values->value.buffer.data;
			const guint8 *source = pixels->value.buffer.data;
			for (gsize index = 0; index < area; index++) {
				double alpha = source[index * RGBA_CHANNELS + ALPHA_CHANNEL] * (1.0 / G_MAXUINT8);
				target[index] = (int)(source[index * RGBA_CHANNELS + channel] * alpha + G_MAXUINT8 * (1 - alpha));
			}
			g_ptr_array_add(channels, values);
		}
		if (transparency)
			g_ptr_array_add(channels, extract_channel(pixels, area, ALPHA_CHANNEL));
	} else {
		Record *blank = create_channels(area, depth);
		for (gsize index = 0; depth == WORD_DEPTH && index < area; index++)
			write_channel(blank, index, G_MAXUINT16);
		for (int channel = 0; channel < count; channel++)
			g_ptr_array_add(channels, reference_record(blank));
		free_record(blank);
	}
	gsize counts = offset, count_size = writer->large ? sizeof(guint32) : sizeof(guint16);
	gsize row_bytes = width * (depth / CHAR_BIT);
	offset += count * height * count_size;
	for (int channel = 0; channel < count; channel++) {
		const Record *values = g_ptr_array_index(channels, channel);
		g_autofree guint8 *bytes = g_malloc0(MAX(area * (depth / CHAR_BIT), 1));
		if (values->value.buffer.length)
			memcpy(bytes, values->value.buffer.data, MIN(area, values->value.buffer.length) * (depth / CHAR_BIT));
		gsize channel_counts = counts + channel * height * count_size;
		offset += pack_rows(bytes, writer, row_bytes, height, channel_counts, offset, count_size);
	}
	return offset;
}

GByteArray *write_psd_document(Document *document, gboolean large, gboolean zip, GError **error) {
	Record *record = document->record;
	if (document->unsupported_features && document->unsupported_features->len) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_PSD_SAVE_UNSUPPORTED);
		return NULL;
	}
	PsdWriter writer = {g_byte_array_new(), large, error, FALSE};
	set_boolean(record, "p7", large);
	remove_record(get_record(record, "add"), "Txt2");
	gboolean transparency = has_transparency(get_record(record, "buffer"));
	prepare_resources(record, transparency);
	gsize offset = 0;
	put_ascii(&writer, offset, PSD_SIGNATURE);
	put_u16(&writer, offset + HEADER_VERSION, large ? PSB_VERSION : PSD_VERSION);
	put_u32(&writer, offset + HEADER_RESERVED, 0);
	put_u16(&writer, offset + HEADER_RESERVED + sizeof(guint32), 0);
	put_u16(&writer, offset + HEADER_CHANNELS, RGB_CHANNELS + (transparency ? 1 : 0));
	put_u32(&writer, offset + HEADER_HEIGHT, get_number(record, "T"));
	put_u32(&writer, offset + HEADER_WIDTH, get_number(record, "s"));
	put_u16(&writer, offset + HEADER_DEPTH, get_number(record, "depth"));
	put_u16(&writer, offset + HEADER_MODE, PSD_RGB_MODE);
	offset += HEADER_BYTES;
	if (get_number(record, "depth") == FLOAT_DEPTH) {
		static const guint8 conversion[] = {
			104, 100, 114, 116, 0, 0,	0,	 3,	  62,  107, 133, 31, 0,	 0,	  0, 2, 0, 0, 0,  7,   0, 76,
			0,	 105, 0,   110, 0, 101, 0,	 97,  0,   114, 0,	 0,	 0,	 2,	  0, 2, 0, 0, 0,  0,   0, 255,
			0,	 255, 1,   1,	0, 0,	0,	 0,	  0,   0,	0,	 0,	 65, 128, 0, 0, 0, 0, 0,  1,   0, 0,
			0,	 0,	  63,  128, 0, 0,	104, 100, 114, 97,	0,	 0,	 0,	 6,	  0, 0, 0, 0, 65, 160, 0, 0,
			65,	 240, 0,   0,	0, 0,	0,	 0,	  0,   0,	0,	 0,	 63, 128, 0, 0, 0, 0, 0,  0,   0, 0};
		put_u32(&writer, offset, sizeof conversion);
		offset += sizeof(guint32);
		offset += put_bytes(&writer, offset, conversion, sizeof conversion);
	} else {
		put_u32(&writer, offset, 0);
		offset += sizeof(guint32);
	}
	offset = write_resources(&writer, record, offset);
	offset = write_layer_and_mask_information(&writer, record, offset, zip, transparency);
	offset = write_merged_image(&writer, record, offset, transparency);
	const char *paths[] = {"r1025", "r7000", "r7001"};
	for (guint index = 0; index < G_N_ELEMENTS(paths); index++)
		remove_record(get_record(record, "Pz"), paths[index]);
	for (int index = FIRST_PATH_RESOURCE; index <= LAST_PATH_RESOURCE; index++) {
		g_autofree char *key = g_strdup_printf("r%d", index);
		remove_record(get_record(record, "Pz"), key);
	}
	remove_record(record, "p7");
	g_byte_array_set_size(writer.data, offset);
	return writer.data;
}
