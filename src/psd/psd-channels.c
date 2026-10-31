#include <limits.h>
#include <string.h>
#include <zlib.h>

#include "psd.h"

/* Inflated data comes out in pieces of this size */
#define INFLATE_CHUNK 65536

static void unpack_bits_row(PsdReader *reader, gsize source, gsize count, Record *target, gsize offset) {
	guint8 *bytes = target->value.buffer.data;
	gsize length = target->value.buffer.length;
	for (gsize consumed = 0; consumed < count && !reader->failed;) {
		int header = read_psd_byte(reader, source++);
		/* A header from 128 repeats the next byte, and a lower one copies that many bytes and one more */
		if (header >= PACKBITS_LONGEST_RUN) {
			int value = read_psd_byte(reader, source++);
			gsize end = offset + (PACKBITS_REPEAT_BASE - header);
			for (gsize index = offset; index < end && index < length; index++)
				bytes[index] = value;
			offset = end;
			consumed += 2;
		} else {
			for (int index = 0; index <= header; index++) {
				int value = read_psd_byte(reader, source + index);
				if (offset + index < length)
					bytes[offset + index] = value;
			}
			source += header + 1;
			offset += header + 1;
			consumed += 2 + header;
		}
	}
}

static gsize unpack_bits_rows(PsdReader *reader, Record *target, gsize row_stride, gsize rows, gsize counts, gsize data,
							  gsize count_size) {
	gsize start = data;
	for (gsize row = 0; row < rows && !reader->failed; row++) {
		gsize count = count_size == sizeof(guint16) ? read_psd_u16(reader, counts + row * sizeof(guint16))
													: read_psd_u32(reader, counts + row * sizeof(guint32));
		unpack_bits_row(reader, data, count, target, row * row_stride);
		data += count;
	}
	return data - start;
}

static GByteArray *inflate_raw(PsdReader *reader, gsize offset, gsize length) {
	if (!has_psd_bytes(reader, offset, length))
		return NULL;
	z_stream stream = {0};
	if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
		return NULL;
	GByteArray *output = g_byte_array_new();
	guint8 chunk[INFLATE_CHUNK];
	stream.next_in = (Bytef *)reader->data + offset;
	stream.avail_in = length;
	int status;
	do {
		stream.next_out = chunk;
		stream.avail_out = sizeof chunk;
		status = inflate(&stream, Z_NO_FLUSH);
		g_byte_array_append(output, chunk, sizeof chunk - stream.avail_out);
		if (output->len > PSD_MAX_DECODED_BYTES)
			status = Z_MEM_ERROR;
	} while (status == Z_OK);
	inflateEnd(&stream);
	if (status != Z_STREAM_END) {
		fail_psd(reader, TEXT_PSD_DAMAGED);
		g_byte_array_unref(output);
		return NULL;
	}
	return output;
}

static void undo_prediction(guint8 *bytes, gsize length, gsize width, gsize height, unsigned depth) {
	if (depth == BYTE_DEPTH) {
		for (gsize row = 0; row < height; row++) {
			gsize start = row * width + 1, end = start + width - 1;
			if (start - 1 >= length)
				break;
			guint value = bytes[start - 1];
			for (gsize index = start; index < end && index < length; index++) {
				value += bytes[index];
				bytes[index] = value & G_MAXUINT8;
			}
		}
	} else if (depth == WORD_DEPTH) {
		for (gsize row = 0; row < height; row++) {
			guint value = 0;
			for (gsize index = row * width; index < row * width + width; index++) {
				gsize byte = index << 1;
				if (byte + 1 >= length)
					break;
				value += bytes[byte] << CHAR_BIT | bytes[byte + 1];
				bytes[byte] = value & G_MAXUINT8;
				bytes[byte + 1] = (value >> CHAR_BIT) & G_MAXUINT8;
			}
		}
	} else {
		gsize row_bytes = width * FLOAT_BYTES;
		g_autofree guint8 *planes = g_malloc(MAX(row_bytes, 1));
		for (gsize row = 0; row < height && (row + 1) * row_bytes <= length; row++) {
			guint8 *line = bytes + row * row_bytes;
			guint value = line[0];
			for (gsize index = 1; index < row_bytes; index++) {
				value += line[index];
				line[index] = value & G_MAXUINT8;
			}
			/* The byte planes go back into floats, the most significant plane first */
			for (gsize index = 0; index < width; index++)
				for (gsize plane = 0; plane < FLOAT_BYTES; plane++)
					planes[FLOAT_BYTES * index + FLOAT_BYTES - 1 - plane] = line[width * plane + index];
			memcpy(line, planes, row_bytes);
		}
	}
}

/* Typed array reinterpretation keeps the host little endian byte view */
static Record *reinterpret_channels(Record *bytes, unsigned depth) {
	if (depth == BYTE_DEPTH)
		return bytes;
	gsize count = bytes->value.buffer.length / (depth / CHAR_BIT);
	Record *typed = create_channels(count, depth);
	if (typed)
		memcpy(typed->value.buffer.data, bytes->value.buffer.data, count * (depth / CHAR_BIT));
	free_record(bytes);
	return typed;
}

static void swap_sample_bytes(guint8 *bytes, gsize length, unsigned depth) {
	gsize sample_bytes = depth / CHAR_BIT;
	for (gsize index = 0; depth > BYTE_DEPTH && index + sample_bytes <= length; index += sample_bytes) {
		for (unsigned low = 0, high = sample_bytes - 1; low < high; low++, high--) {
			guint8 value = bytes[index + low];
			bytes[index + low] = bytes[index + high];
			bytes[index + high] = value;
		}
	}
}

Record *decode_psd_channel(PsdReader *reader, unsigned depth, gsize width, gsize height, gsize offset, int compression,
						   gint64 length) {
	gsize sample_bytes = width * height * (depth / CHAR_BIT);
	gsize padded = sample_bytes + (sample_bytes % sizeof(guint32) ? sizeof(guint32) - sample_bytes % sizeof(guint32) : 0);
	if (sample_bytes > PSD_MAX_DECODED_BYTES) {
		fail_psd(reader, TEXT_PIXEL_LIMIT);
		return NULL;
	}
	if (length <= 0)
		return create_channels(padded, depth);
	Record *bytes = NULL;
	if (compression == PSD_RAW) {
		bytes = create_channels(sample_bytes, BYTE_DEPTH);
		gsize available = offset < reader->length ? MIN(sample_bytes, reader->length - offset) : 0;
		if (bytes && available)
			memcpy(bytes->value.buffer.data, reader->data + offset, available);
		if (bytes)
			swap_sample_bytes(bytes->value.buffer.data, sample_bytes, depth);
	} else if (compression == PSD_RLE) {
		bytes = create_channels(sample_bytes, BYTE_DEPTH);
		gsize count_size = reader->large ? sizeof(guint32) : sizeof(guint16);
		if (bytes)
			unpack_bits_rows(reader, bytes, width, height, offset, offset + count_size * height, count_size);
	} else if (compression == PSD_ZIP || compression == PSD_ZIP_PREDICTED) {
		g_autoptr(GByteArray) inflated =
			length >= ZIP_HEADER_BYTES + ZIP_TRAILER_BYTES
				? inflate_raw(reader, offset + ZIP_HEADER_BYTES, length - ZIP_HEADER_BYTES - ZIP_TRAILER_BYTES)
				: NULL;
		if (!inflated) {
			fail_psd(reader, TEXT_PSD_DAMAGED);
			return NULL;
		}
		if (compression == PSD_ZIP_PREDICTED)
			undo_prediction(inflated->data, inflated->len, width, height, depth);
		bytes = create_channels(padded, BYTE_DEPTH);
		if (bytes)
			memcpy(bytes->value.buffer.data, inflated->data, MIN(inflated->len, bytes->value.buffer.length));
	} else
		bytes = create_channels(padded, BYTE_DEPTH);
	if (!bytes) {
		fail_psd(reader, TEXT_IMAGE_ALLOCATION_FAILED);
		return NULL;
	}
	return reinterpret_channels(bytes, depth);
}

Record *read_psd_channel(PsdReader *reader, unsigned depth, gsize width, gsize height, gsize offset, gint64 length) {
	int compression = read_psd_u16(reader, offset);
	return decode_psd_channel(reader, depth, width, height, offset + sizeof(guint16), compression,
							  length - sizeof(guint16));
}

void interleave_psd_channel(const Record *channel, Record *target, unsigned position) {
	gsize count = MIN(target->value.buffer.length / RGBA_CHANNELS, channel->value.buffer.length);
	for (gsize index = 0; index < count; index++)
		write_channel(target, index * RGBA_CHANNELS + position, read_channel(channel, index));
}

gsize read_psd_merged_image(PsdReader *reader, gsize offset, unsigned file_depth) {
	Record *record = reader->document->record;
	int mode = get_number(record, "Ia"), channels = get_number(record, "P9");
	/* The colour channels of each colour mode, from bitmap to multichannel */
	const int mode_channels[] = {1, 1, 1, 3, 4, 1, 1, 3};
	gsize width = get_number(record, "s"), height = get_number(record, "T"), area = width * height;
	unsigned depth = get_number(record, "depth");
	/* A first alpha channel named Transparency or no names at all counts as layer transparency */
	Record *alpha_names = get_record(record, "Wa");
	Record *first_name = alpha_names && alpha_names->type == RECORD_ARRAY ? get_item(alpha_names, 0) : NULL;
	gboolean transparency =
		!alpha_names || alpha_names->type != RECORD_ARRAY ||
		(first_name && first_name->type == RECORD_STRING && !strcmp(first_name->value.string, "Transparency"));
	int color_channels = mode_channels[mode] + (transparency ? 1 : 0);
	int compression = read_psd_u16(reader, offset);
	offset += sizeof(guint16);
	Record *buffer = create_channels(area * RGBA_CHANNELS, depth);
	if (!buffer) {
		fail_psd(reader, TEXT_IMAGE_ALLOCATION_FAILED);
		return offset;
	}
	set_record(record, "buffer", buffer);
	double maximum = channel_maximum(depth);
	for (gsize index = 0; index < area; index++)
		write_channel(buffer, index * RGBA_CHANNELS + ALPHA_CHANNEL, depth == FLOAT_DEPTH ? 1 : maximum);
	gsize counts = offset, count_size = reader->large ? sizeof(guint32) : sizeof(guint16);
	gsize sample_bytes = file_depth / CHAR_BIT;
	for (int channel = 0; channel < channels && !reader->failed; channel++) {
		Record *samples = NULL;
		if (compression == PSD_RAW) {
			samples = decode_psd_channel(reader, file_depth, width, height, offset, 0, 1);
			offset += area * sample_bytes;
		} else if (compression == PSD_RLE) {
			Record *bytes = create_channels(area * sample_bytes, BYTE_DEPTH);
			if (!bytes)
				break;
			if (channel == 0)
				offset += channels * height * count_size;
			offset += unpack_bits_rows(reader, bytes, width * sample_bytes, height,
									   counts + channel * height * count_size, offset, count_size);
			swap_sample_bytes(bytes->value.buffer.data, bytes->value.buffer.length, file_depth);
			samples = reinterpret_channels(bytes, file_depth);
		} else {
			note_unsupported_psd_feature(reader, TEXT_PSD_UNSUPPORTED_COMPRESSION);
			return offset;
		}
		if (!samples)
			break;
		if (channel < color_channels) {
			interleave_psd_channel(samples, buffer, channel);
			if (channels == 1 && channel == 0) {
				interleave_psd_channel(samples, buffer, 1);
				interleave_psd_channel(samples, buffer, 2);
			}
		} else
			note_unsupported_psd_feature(reader, TEXT_PSD_ALPHA_CHANNELS);
		free_record(samples);
	}
	set_number(record, "P9", RGBA_CHANNELS);
	/* An indexed palette lists all reds, then all greens, then all blues */
	Record *palette = get_record(record, "rm");
	if (mode == PSD_INDEXED_MODE && palette && palette->type == RECORD_BUFFER) {
		for (gsize index = 0; index < area; index++) {
			int entry = read_channel(buffer, index * RGBA_CHANNELS);
			for (int component = 0; component < RGB_CHANNELS; component++)
				write_channel(buffer, index * RGBA_CHANNELS + component,
							  read_channel(palette, component * (G_MAXUINT8 + 1) + entry));
		}
	}
	return offset;
}
