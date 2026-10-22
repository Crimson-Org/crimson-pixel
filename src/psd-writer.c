#include <math.h>
#include <string.h>
#include <zlib.h>

#include "adjustments.h"
#include "descriptor.h"
#include "kernels.h"
#include "psd.h"
#include "psd-output.h"

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
	put_byte(writer, offset, value >> 8 & 255);
	put_byte(writer, offset + 1, value & 255);
}

void put_u32(PsdWriter *writer, gsize offset, double value) {
	guint32 bits = isfinite(value) ? (guint32)(gint64)value : 0;
	for (int index = 0; index < 4; index++)
		put_byte(writer, offset + index, bits >> (24 - index * 8) & 255);
}

static void put_large(PsdWriter *writer, gsize offset, double value) {
	gint32 high = (gint32)(gint64)value >> 16 >> 16;
	put_u32(writer, offset, high);
	put_u32(writer, offset + 4, (double)(guint32)(gint64)value);
}

static void put_ascii(PsdWriter *writer, gsize offset, const char *text) {
	for (gsize index = 0; text[index]; index++)
		put_byte(writer, offset + index, (guint8)text[index]);
}

static void put_double(PsdWriter *writer, gsize offset, double value) {
	guint64 bits;
	memcpy(&bits, &value, sizeof bits);
	for (int index = 0; index < 8; index++)
		put_byte(writer, offset + index, bits >> (56 - index * 8) & 255);
}

void put_float(PsdWriter *writer, gsize offset, double value) {
	float single = value;
	guint32 bits;
	memcpy(&bits, &single, sizeof bits);
	for (int index = 0; index < 4; index++)
		put_byte(writer, offset + index, bits >> (24 - index * 8) & 255);
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
		put_u16(writer, offset + 4 + index * 2, utf16[index]);
	return 4 + units * 2;
}

void put_i16(PsdWriter *writer, gsize offset, double value) {
	put_u16(writer, offset, isfinite(value) ? (guint16)(gint64)fmod(trunc(value), 65536) : 0);
}

static void put_rectangle(PsdWriter *writer, gsize offset, RasterExtent extent) {
	put_u32(writer, offset, extent.y);
	put_u32(writer, offset + 4, extent.x);
	put_u32(writer, offset + 8, extent.y + extent.height);
	put_u32(writer, offset + 12, extent.x + extent.width);
}

static gsize put_pascal(PsdWriter *writer, gsize offset, const char *text) {
	glong units = 0;
	g_autofree gunichar2 *utf16 = g_utf8_to_utf16(text, -1, NULL, &units, NULL);
	gsize length = MIN((gsize)units, 255);
	ensure_size(writer, offset + length + 2);
	put_byte(writer, offset, length);
	for (gsize index = 0; index < length; index++)
		put_byte(writer, offset + 1 + index, utf16[index] & 255);
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
		gsize limit = count < 128 ? count : 128, next, run;
		gboolean repeated =
			position + 3 <= end && source[position + 1] == source[position] && source[position + 2] == source[position];
		if (repeated) {
			for (next = position + 3; next < position + limit && source[next] == source[position];)
				next++;
			run = next - position;
			put_byte(writer, out++, 1 + 256 - run);
			put_byte(writer, out++, source[position]);
		} else {
			for (next = position; next < position + limit;) {
				if (next + 3 <= end && source[next + 1] == source[next] && source[next + 2] == source[next])
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
		if (count_size == 2)
			put_u16(writer, counts + row * 2, packed);
		else
			put_u32(writer, counts + row * 4, packed);
		offset += packed;
	}
	return offset - start;
}

static void predict_samples(guint8 *bytes, gsize width, gsize height, unsigned depth) {
	if (depth == 8) {
		for (gsize row = 0; row < height; row++)
			for (gsize index = row * width + width - 1; index > row * width; index--)
				bytes[index] = (bytes[index] - bytes[index - 1] + 256) & 255;
	} else if (depth == 16) {
		for (gsize row = 0; row < height; row++) {
			gsize start = row * width;
			for (gsize index = start + width - 1; index > start; index--) {
				gsize at = index << 1;
				guint value = ((bytes[at + 1] << 8 | bytes[at]) - (bytes[at - 1] << 8 | bytes[at - 2]) + 65536) & 65535;
				bytes[at] = value >> 8;
				bytes[at + 1] = value & 255;
			}
			guint8 swap = bytes[start * 2];
			bytes[start * 2] = bytes[start * 2 + 1];
			bytes[start * 2 + 1] = swap;
		}
	} else {
		g_autofree guint8 *planes = g_malloc(MAX(width * 4, 1));
		for (gsize row = 0; row < height; row++) {
			guint8 *line = bytes + row * width * 4;
			for (gsize index = 0; index < width; index++) {
				planes[index] = line[4 * index + 3];
				planes[width + index] = line[4 * index + 2];
				planes[width * 2 + index] = line[4 * index + 1];
				planes[width * 3 + index] = line[4 * index];
			}
			for (gsize index = width * 4 - 1; index > 0; index--)
				planes[index] = (planes[index] - planes[index - 1] + 256) & 255;
			memcpy(line, planes, width * 4);
		}
	}
}

static gsize encode_samples(PsdWriter *writer, const Record *samples, gsize width, gsize height, gsize offset,
							int compression) {
	unsigned depth = samples->value.buffer.depth;
	gsize count = width * height, sample_bytes = depth / 8;
	g_autofree guint8 *bytes = g_malloc0(MAX(count * sample_bytes, 1));
	if (samples->value.buffer.length)
		memcpy(bytes, samples->value.buffer.data, MIN(count, samples->value.buffer.length) * sample_bytes);
	if (compression == 1) {
		gsize count_size = writer->large ? 4 : 2;
		gsize packed = pack_rows(bytes, writer, width, height, offset, offset + count_size * height, count_size);
		return offset + count_size * height + packed;
	}
	if (compression == 3)
		predict_samples(bytes, width, height, depth);
	put_byte(writer, offset, 120);
	put_byte(writer, offset + 1, 156);
	offset += 2;
	uLongf capacity = compressBound(count * sample_bytes) + 64;
	g_autofree guint8 *packed = g_malloc(capacity);
	z_stream stream = {0};
	/* The bundle passes a mangled level option that pako ignores, so its default level applies */
	deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
	stream.next_in = bytes;
	stream.avail_in = count * sample_bytes;
	stream.next_out = packed;
	stream.avail_out = capacity;
	deflate(&stream, Z_FINISH);
	gsize length = capacity - stream.avail_out;
	deflateEnd(&stream);
	put_bytes(writer, offset, packed, length);
	ensure_size(writer, offset + length + 4);
	return offset + length + 4;
}

static gsize encode_channel(PsdWriter *writer, const Record *samples, gsize width, gsize height, gsize offset,
							int compression) {
	put_u16(writer, offset, compression);
	return encode_samples(writer, samples, width, height, offset + 2, compression);
}

static Record *extract_channel(const Record *pixels, gsize count, unsigned position) {
	Record *channel = create_channels(count, pixels->value.buffer.depth);
	gsize available = MIN(count, pixels->value.buffer.length / 4);
	for (gsize index = 0; index < available; index++)
		write_channel(channel, index, read_channel(pixels, index * 4 + position));
	return channel;
}

/* ============================== Tagged blocks ==================================== */

static const char *LARGE_LENGTH_KEYS =
	"LMsk Lr16 Lr32 Layr Mt16 Mt32 Mtrn Alph FMsk lnk2 lnkE FEid FXid PxSD extn cinf artd pths";

static gboolean is_listed(const char *list, const char *key) {
	for (const char *position = list;; position += 5) {
		if (!strncmp(position, key, 4))
			return TRUE;
		if (!position[4])
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
		put_ascii(writer, offset, large ? "8B64" : "8BIM");
		offset += 4;
		put_ascii(writer, offset, key);
		offset += 4;
		put_u32(writer, offset, 0);
		offset += large ? 8 : 4;
		gsize size = 0;
		if (!strcmp(key, "iOpa") || !strcmp(key, "knko") || !strcmp(key, "infx") || !strcmp(key, "clbl") ||
			!strcmp(key, "lmgm") || !strcmp(key, "vmgm") || !strcmp(key, "fcmy")) {
			put_byte(writer, offset, value->value.number);
			size = 4;
		} else if (!strcmp(key, "brst")) {
			for (int channel = 0; channel < 3; channel++)
				if (get_item(value, channel) && get_item(value, channel)->value.number == 0) {
					put_u32(writer, offset + size, channel);
					size += 4;
				}
		} else if (!strcmp(key, "lyid") || !strcmp(key, "lyvr") || !strcmp(key, "lspf")) {
			put_u32(writer, offset, value->value.number);
			size = 4;
		} else if (!strcmp(key, "lsct")) {
			put_u32(writer, offset, get_number(value, "J"));
			size = 4;
			if (get_string(value, "qu")) {
				put_ascii(writer, offset + 4, "8BIM");
				put_ascii(writer, offset + 8, get_string(value, "qu"));
				size = 12;
				Record *grouped = get_record(value, "Zd");
				if (grouped && grouped->type != RECORD_NULL) {
					put_u32(writer, offset + 12, grouped->value.boolean ? 1 : 0);
					size += 4;
				}
			}
		} else if (!strcmp(key, "lnsr")) {
			put_ascii(writer, offset, value->value.string);
			size = 4;
		} else if (!strcmp(key, "lclr")) {
			put_u16(writer, offset, value->value.number);
			ensure_size(writer, offset + 4);
			size = 8;
		} else if (!strcmp(key, "luni"))
			size = put_counted_text(writer, offset, value->value.string);
		else if (!strcmp(key, "fxrp")) {
			put_double(writer, offset, get_number(value, "x"));
			put_double(writer, offset + 8, get_number(value, "y"));
			size = 16;
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
		if (size % 4)
			size += 4 - size % 4;
		if (large)
			put_large(writer, offset - 8, size);
		else
			put_u32(writer, offset - 4, size);
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
		flags += 1;
	if (!get_boolean(mask, "isEnabled"))
		flags += 2;
	if (get_boolean(mask, "L7"))
		flags += 8;
	if (parameters)
		flags += 16;
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
	offset += 4;
	if (!has_object(layer, "yy"))
		return offset;
	Record *mask = get_record(layer, "yy"), *parameters = get_writable_mask(layer);
	gsize start = offset;
	put_u32(writer, offset, 0);
	put_rectangle(writer, offset, record_extent(mask));
	offset += 16;
	put_byte(writer, offset++, get_number(mask, "color"));
	put_mask_flags(writer, offset++, mask, TRUE);
	if (has_object(layer, "E_")) {
		Record *real = get_record(layer, "E_");
		put_mask_flags(writer, offset, real, FALSE);
		put_byte(writer, offset + 1, get_number(real, "color"));
		put_rectangle(writer, offset + 2, record_extent(real));
		offset += 18;
	}
	gsize flags_start = offset;
	put_byte(writer, offset++, 3);
	put_byte(writer, offset++, parameters ? get_number(parameters, "mU") : 255);
	put_double(writer, offset, parameters ? get_number(parameters, "O1") : 0);
	offset += 8;
	if ((offset - flags_start) & 1)
		offset++;
	ensure_size(writer, offset);
	put_u32(writer, start - 4, offset - start);
	return offset;
}

static int list_layer_channels(const Record *layer, int channels[6]) {
	int count = 0;
	channels[count++] = -1;
	channels[count++] = 0;
	channels[count++] = 1;
	channels[count++] = 2;
	if (has_object(layer, "yy"))
		channels[count++] = -2;
	if (has_object(layer, "E_"))
		channels[count++] = -3;
	return count;
}

static gsize write_layer_record(PsdWriter *writer, Record *document_record, Record *layer, gsize offset,
								gsize *channel_info) {
	put_rectangle(writer, offset, record_extent(layer));
	offset += 16;
	int channels[6], count = list_layer_channels(layer, channels);
	put_u16(writer, offset, count);
	offset += 2;
	*channel_info = offset;
	for (int index = 0; index < count; index++) {
		put_u16(writer, offset, (guint16)channels[index]);
		if (writer->large)
			put_large(writer, offset + 2, 0);
		else
			put_u32(writer, offset + 2, 0);
		offset += writer->large ? 10 : 6;
	}
	put_ascii(writer, offset, "8BIM");
	put_ascii(writer, offset + 4, get_string(layer, "qu"));
	offset += 8;
	put_byte(writer, offset++, get_number(layer, "nv"));
	Record *first = get_item(get_record(document_record, "Z"), 0);
	put_byte(writer, offset++, get_boolean(layer, "jl") && first != layer ? 1 : 0);
	put_byte(writer, offset++, get_number(layer, "NF"));
	put_byte(writer, offset++, 0);
	gsize length_offset = offset;
	put_u32(writer, offset, 0);
	offset += 4;
	gsize extra_start = offset;
	offset = write_layer_mask(writer, layer, offset);
	Record *ranges = get_record(layer, "hu");
	put_u32(writer, offset, count_records(ranges));
	offset += 4;
	ensure_size(writer, offset + 40);
	for (int index = 0; index < 40; index++)
		put_byte(writer, offset + index, (guint)get_item(ranges, index)->value.number);
	offset += 40;
	const char *name = get_string(layer, "name");
	gsize name_size = put_pascal(writer, offset, name ? name : "");
	offset += name_size;
	if (name_size % 4)
		offset += 4 - name_size % 4;
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
	int channels[6], count = list_layer_channels(layer, channels);
	Record *pixels = get_record(layer, "buffer");
	RasterExtent extent = record_extent(layer);
	unsigned depth = pixels->value.buffer.depth;
	int compression = depth != 8 || zip ? 3 : 1;
	for (int index = 0; index < count; index++) {
		int identifier = channels[index];
		const Record *target = identifier == -3	  ? get_record(layer, "E_")
							   : identifier == -2 ? get_record(layer, "yy")
												  : layer;
		RasterExtent channel_extent = record_extent(target);
		g_autoptr(Record) samples = identifier >= -1
										? extract_channel(pixels, (gsize)MAX(0, extent.width) * MAX(0, extent.height),
														  identifier == -1 ? 3 : identifier)
										: reference_record(get_record(target, "C"));
		gsize start = offset;
		offset = encode_channel(writer, samples, MAX(0, channel_extent.width), MAX(0, channel_extent.height), offset,
								compression);
		if (writer->large)
			put_large(writer, channel_info + index * 10 + 2, offset - start);
		else
			put_u32(writer, channel_info + index * 6 + 2, offset - start);
	}
	return offset;
}

static gsize write_layer_records(PsdWriter *writer, Record *document_record, gsize offset, gboolean zip,
								 gboolean merged) {
	Record *layers = get_record(document_record, "Z");
	guint count = count_records(layers);
	put_u16(writer, offset, merged ? (guint16) - (gint)count : count);
	offset += 2;
	g_autofree gsize *channel_info = g_new0(gsize, MAX(count, 1));
	for (guint index = 0; index < count; index++)
		offset = write_layer_record(writer, document_record, get_item(layers, index), offset, &channel_info[index]);
	for (guint index = 0; index < count; index++)
		offset = write_layer_channels(writer, get_item(layers, index), offset, channel_info[index], zip);
	return offset;
}

static gsize write_layer_and_mask_information(PsdWriter *writer, Record *document_record, gsize offset, gboolean zip,
											  gboolean transparency) {
	gsize start = offset, length_size = writer->large ? 8 : 4;
	put_u32(writer, offset, 0);
	offset += 4;
	if (writer->large) {
		put_u32(writer, offset, 0);
		offset += 4;
	}
	gsize layers_start = offset;
	offset += length_size;
	unsigned depth = get_number(document_record, "depth");
	if (depth == 8)
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
	offset += 4;
	Record *descriptors = get_record(document_record, "add");
	g_autoptr(Record) blocks = NULL;
	if (depth != 8) {
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
		put_large(writer, start, offset - start - 8);
	else
		put_u32(writer, start, offset - start - 4);
	return offset;
}

/* ============================== Resources and merged image ======================= */

static void set_resource(Record *resources, const char *key, GByteArray *bytes) {
	set_record(resources, key, create_bytes(bytes->data, bytes->len));
}

static GByteArray *encode_descriptor_resource(const Record *descriptor, gboolean slices) {
	GByteArray *bytes = g_byte_array_new();
	if (slices)
		write_u32(bytes, 8);
	write_u32(bytes, 16);
	write_descriptor(bytes, descriptor);
	return bytes;
}

static void prepare_resources(Record *document_record, gboolean transparency) {
	Record *resources = get_record(document_record, "Pz"), *layers = get_record(document_record, "Z");
	g_autoptr(GByteArray) bytes = g_byte_array_new();
	double resolution = get_number(document_record, "Qf");
	for (int copy = 0; copy < 2; copy++) {
		write_u16(bytes, floor(resolution));
		write_u16(bytes, floor((resolution - floor(resolution)) * 65536));
		write_u16(bytes, 1);
		write_u16(bytes, 2);
	}
	set_resource(resources, "r1005", bytes);
	Record *guides = get_record(document_record, "Jd");
	if (count_records(guides)) {
		g_byte_array_set_size(bytes, 0);
		write_u32(bytes, 3);
		write_u32(bytes, count_records(guides));
		for (guint index = 0; index < count_records(guides); index++) {
			Record *guide = get_item(guides, index);
			write_u16(bytes, 14);
			for (int axis = 0; axis < 2; axis++) {
				float value = get_number(guide, axis ? "x" : "y");
				guint32 bits;
				memcpy(&bits, &value, 4);
				write_u32(bytes, (double)(gint32)bits);
			}
			write_u16(bytes, 65535);
			write_u16(bytes, 8);
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
	write_u32(bytes, 1);
	write_u32(bytes, 576);
	write_u32(bytes, 576);
	write_u32(bytes, count_records(grid));
	for (guint index = 0; index < count_records(grid); index++) {
		Record *line = get_item(grid, index);
		write_u32(bytes, round_like_source(get_item(line, 1)->value.number * 32));
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
		guint8 display[17] = {0, 0, 0, 1, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 100, 1};
		set_record(resources, "r1077", create_bytes(display, sizeof display));
	} else {
		guint8 display[4] = {0, 0, 0, 1};
		set_record(resources, "r1077", create_bytes(display, sizeof display));
	}
	Record *slices = get_record(document_record, "Xd");
	for (guint index = 0; index < count_records(slices); index++) {
		Record *slice = get_record(get_item(slices, count_records(slices) - index - 1), "v");
		set_number(get_record(slice, "sliceID"), "v", 2 + index * 3);
	}
	g_autoptr(Record) slice_descriptor = create_object(RECORD_PLAIN);
	set_string(slice_descriptor, "classID", "null");
	set_record(slice_descriptor, "baseName", create_typed_value("TEXT", create_string("User")));
	Record *bounds = create_object(RECORD_PLAIN);
	set_string(bounds, "classID", "Rct1");
	const char *sides[] = {"Top ", "Left", "Btom", "Rght"};
	for (int side = 0; side < 4; side++)
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
		set_record(sheet, "Vrsn", create_typed_value("long", create_number(2)));
		set_record(sheet, "sheetID",
				   create_typed_value("long", create_number(get_number(get_record(layer, "add"), "lyid"))));
		set_record(sheet, "sheetDisclosed",
				   create_typed_value("bool", create_boolean(get_item(get_record(layer, "md"), 0)->value.boolean)));
		append_record(sheets, create_typed_value("Objc", sheet));
	}
	if (count_records(sheets)) {
		g_autoptr(Record) options = create_object(RECORD_PLAIN);
		set_string(options, "classID", "null");
		set_record(options, "Vrsn", create_typed_value("long", create_number(1)));
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
	offset += 4;
	Record *resources = get_record(document_record, "Pz");
	g_autoptr(GPtrArray) keys = get_record_keys(resources);
	g_ptr_array_sort(keys, compare_resource_identifiers);
	for (guint index = 0; index < keys->len; index++) {
		const char *key = g_ptr_array_index(keys, index);
		int identifier = atoi(key + 1);
		put_ascii(writer, offset + size, identifier == 7000 || identifier == 7001 ? "MeSa" : "8BIM");
		size += 4;
		put_u16(writer, offset + size, identifier);
		size += 2;
		size += put_pascal(writer, offset + size, "");
		Record *bytes = get_record(resources, key);
		gsize length = bytes->value.buffer.length;
		put_u32(writer, offset + size, length);
		size += 4;
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
	length -= length & 31;
	guint mask = 255;
	for (gsize index = 3; index < length; index += 32)
		for (int step = 0; step < 32; step += 4)
			mask &= bytes[index + step];
	return mask != 255;
}

static gsize write_merged_image(PsdWriter *writer, Record *document_record, gsize offset, gboolean transparency) {
	put_u16(writer, offset, 1);
	offset += 2;
	gsize width = get_number(document_record, "s"), height = get_number(document_record, "T"), area = width * height;
	unsigned depth = get_number(document_record, "depth");
	Record *pixels = get_record(document_record, "buffer");
	int count = 3 + (transparency ? 1 : 0);
	g_autoptr(GPtrArray) channels = g_ptr_array_new_with_free_func((GDestroyNotify)free_record);
	if (depth == 8) {
		for (int channel = 0; channel < 3; channel++) {
			Record *values = create_channels(area, 8);
			guint8 *target = values->value.buffer.data;
			const guint8 *source = pixels->value.buffer.data;
			for (gsize index = 0; index < area; index++) {
				double alpha = source[index * 4 + 3] * (1.0 / 255);
				target[index] = (int)(source[index * 4 + channel] * alpha + 255 * (1 - alpha));
			}
			g_ptr_array_add(channels, values);
		}
		if (transparency)
			g_ptr_array_add(channels, extract_channel(pixels, area, 3));
	} else {
		Record *blank = create_channels(area, depth);
		for (gsize index = 0; depth == 16 && index < area; index++)
			write_channel(blank, index, 65535);
		for (int channel = 0; channel < count; channel++)
			g_ptr_array_add(channels, reference_record(blank));
		free_record(blank);
	}
	gsize counts = offset, count_size = writer->large ? 4 : 2, row_bytes = width * (depth / 8);
	offset += count * height * count_size;
	for (int channel = 0; channel < count; channel++) {
		const Record *values = g_ptr_array_index(channels, channel);
		g_autofree guint8 *bytes = g_malloc0(MAX(area * (depth / 8), 1));
		if (values->value.buffer.length)
			memcpy(bytes, values->value.buffer.data, MIN(area, values->value.buffer.length) * (depth / 8));
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
	put_ascii(&writer, offset, "8BPS");
	put_u16(&writer, offset + 4, large ? 2 : 1);
	put_u32(&writer, offset + 6, 0);
	put_u16(&writer, offset + 10, 0);
	put_u16(&writer, offset + 12, 3 + (transparency ? 1 : 0));
	put_u32(&writer, offset + 14, get_number(record, "T"));
	put_u32(&writer, offset + 18, get_number(record, "s"));
	put_u16(&writer, offset + 22, get_number(record, "depth"));
	put_u16(&writer, offset + 24, 3);
	offset += 26;
	if (get_number(record, "depth") == 32) {
		static const guint8 conversion[] = {
			104, 100, 114, 116, 0, 0,	0,	 3,	  62,  107, 133, 31, 0,	 0,	  0, 2, 0, 0, 0,  7,   0, 76,
			0,	 105, 0,   110, 0, 101, 0,	 97,  0,   114, 0,	 0,	 0,	 2,	  0, 2, 0, 0, 0,  0,   0, 255,
			0,	 255, 1,   1,	0, 0,	0,	 0,	  0,   0,	0,	 0,	 65, 128, 0, 0, 0, 0, 0,  1,   0, 0,
			0,	 0,	  63,  128, 0, 0,	104, 100, 114, 97,	0,	 0,	 0,	 6,	  0, 0, 0, 0, 65, 160, 0, 0,
			65,	 240, 0,   0,	0, 0,	0,	 0,	  0,   0,	0,	 0,	 63, 128, 0, 0, 0, 0, 0,  0,   0, 0};
		put_u32(&writer, offset, sizeof conversion);
		offset += 4;
		offset += put_bytes(&writer, offset, conversion, sizeof conversion);
	} else {
		put_u32(&writer, offset, 0);
		offset += 4;
	}
	offset = write_resources(&writer, record, offset);
	offset = write_layer_and_mask_information(&writer, record, offset, zip, transparency);
	offset = write_merged_image(&writer, record, offset, transparency);
	const char *paths[] = {"r1025", "r7000", "r7001"};
	for (guint index = 0; index < G_N_ELEMENTS(paths); index++)
		remove_record(get_record(record, "Pz"), paths[index]);
	for (int index = 2000; index <= 2016; index++) {
		g_autofree char *key = g_strdup_printf("r%d", index);
		remove_record(get_record(record, "Pz"), key);
	}
	remove_record(record, "p7");
	g_byte_array_set_size(writer.data, offset);
	return writer.data;
}
