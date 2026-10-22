#include <string.h>
#include <zlib.h>

#include "psd.h"

static void unpack_bits_row(PsdReader *reader, gsize source, gsize count, Record *target, gsize offset) {
	guint8 *bytes = target->value.buffer.data;
	gsize length = target->value.buffer.length;
	for (gsize consumed = 0; consumed < count && !reader->failed;) {
		int header = read_psd_byte(reader, source++);
		if (header >= 128) {
			int value = read_psd_byte(reader, source++);
			gsize end = offset + (257 - header);
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
		gsize count = count_size == 2 ? read_psd_u16(reader, counts + row * 2) : read_psd_u32(reader, counts + row * 4);
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
	guint8 chunk[65536];
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
	if (depth == 8) {
		for (gsize row = 0; row < height; row++) {
			gsize start = row * width + 1, end = start + width - 1;
			if (start - 1 >= length)
				break;
			guint value = bytes[start - 1];
			for (gsize index = start; index < end && index < length; index++) {
				value += bytes[index];
				bytes[index] = value & 255;
			}
		}
	} else if (depth == 16) {
		for (gsize row = 0; row < height; row++) {
			guint value = 0;
			for (gsize index = row * width; index < row * width + width; index++) {
				gsize byte = index << 1;
				if (byte + 1 >= length)
					break;
				value += bytes[byte] << 8 | bytes[byte + 1];
				bytes[byte] = value & 255;
				bytes[byte + 1] = (value >> 8) & 255;
			}
		}
	} else {
		gsize row_bytes = width * 4;
		g_autofree guint8 *planes = g_malloc(MAX(row_bytes, 1));
		for (gsize row = 0; row < height && (row + 1) * row_bytes <= length; row++) {
			guint8 *line = bytes + row * row_bytes;
			guint value = line[0];
			for (gsize index = 1; index < row_bytes; index++) {
				value += line[index];
				line[index] = value & 255;
			}
			for (gsize index = 0; index < width; index++) {
				planes[4 * index + 3] = line[index];
				planes[4 * index + 2] = line[width + index];
				planes[4 * index + 1] = line[width * 2 + index];
				planes[4 * index] = line[width * 3 + index];
			}
			memcpy(line, planes, row_bytes);
		}
	}
}

/* Typed array reinterpretation keeps the host little endian byte view */
static Record *reinterpret_channels(Record *bytes, unsigned depth) {
	if (depth == 8)
		return bytes;
	gsize count = bytes->value.buffer.length / (depth / 8);
	Record *typed = create_channels(count, depth);
	if (typed)
		memcpy(typed->value.buffer.data, bytes->value.buffer.data, count * (depth / 8));
	free_record(bytes);
	return typed;
}

static void swap_sample_bytes(guint8 *bytes, gsize length, unsigned depth) {
	for (gsize index = 0; depth > 8 && index + depth / 8 <= length; index += depth / 8) {
		for (unsigned low = 0, high = depth / 8 - 1; low < high; low++, high--) {
			guint8 value = bytes[index + low];
			bytes[index + low] = bytes[index + high];
			bytes[index + high] = value;
		}
	}
}

Record *decode_psd_channel(PsdReader *reader, unsigned depth, gsize width, gsize height, gsize offset, int compression,
						   gint64 length) {
	gsize sample_bytes = width * height * (depth >> 3);
	gsize padded = sample_bytes + (sample_bytes & 3 ? 4 - (sample_bytes & 3) : 0);
	if (sample_bytes > PSD_MAX_DECODED_BYTES) {
		fail_psd(reader, TEXT_PIXEL_LIMIT);
		return NULL;
	}
	if (length <= 0)
		return create_channels(padded, depth);
	Record *bytes = NULL;
	if (compression == 0) {
		bytes = create_channels(sample_bytes, 8);
		gsize available = offset < reader->length ? MIN(sample_bytes, reader->length - offset) : 0;
		if (bytes && available)
			memcpy(bytes->value.buffer.data, reader->data + offset, available);
		if (bytes)
			swap_sample_bytes(bytes->value.buffer.data, sample_bytes, depth);
	} else if (compression == 1) {
		bytes = create_channels(sample_bytes, 8);
		gsize count_size = reader->large ? 4 : 2;
		if (bytes)
			unpack_bits_rows(reader, bytes, width, height, offset, offset + count_size * height, count_size);
	} else if (compression == 2 || compression == 3) {
		g_autoptr(GByteArray) inflated = length >= 6 ? inflate_raw(reader, offset + 2, length - 6) : NULL;
		if (!inflated) {
			fail_psd(reader, TEXT_PSD_DAMAGED);
			return NULL;
		}
		if (compression == 3)
			undo_prediction(inflated->data, inflated->len, width, height, depth);
		bytes = create_channels(padded, 8);
		if (bytes)
			memcpy(bytes->value.buffer.data, inflated->data, MIN(inflated->len, bytes->value.buffer.length));
	} else
		bytes = create_channels(padded, 8);
	if (!bytes) {
		fail_psd(reader, TEXT_IMAGE_ALLOCATION_FAILED);
		return NULL;
	}
	return reinterpret_channels(bytes, depth);
}

Record *read_psd_channel(PsdReader *reader, unsigned depth, gsize width, gsize height, gsize offset, gint64 length) {
	int compression = read_psd_u16(reader, offset);
	return decode_psd_channel(reader, depth, width, height, offset + 2, compression, length - 2);
}

void interleave_psd_channel(const Record *channel, Record *target, unsigned position) {
	gsize count = MIN(target->value.buffer.length / 4, channel->value.buffer.length);
	for (gsize index = 0; index < count; index++)
		write_channel(target, index * 4 + position, read_channel(channel, index));
}

gsize read_psd_merged_image(PsdReader *reader, gsize offset, unsigned file_depth) {
	Record *record = reader->document->record;
	int mode = get_number(record, "Ia"), channels = get_number(record, "P9");
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
	offset += 2;
	Record *buffer = create_channels(area * 4, depth);
	if (!buffer) {
		fail_psd(reader, TEXT_IMAGE_ALLOCATION_FAILED);
		return offset;
	}
	set_record(record, "buffer", buffer);
	double maximum = channel_maximum(depth);
	for (gsize index = 0; index < area; index++)
		write_channel(buffer, index * 4 + 3, depth == 32 ? 1 : maximum);
	gsize counts = offset, count_size = reader->large ? 4 : 2, sample_bytes = file_depth >> 3;
	for (int channel = 0; channel < channels && !reader->failed; channel++) {
		Record *samples = NULL;
		if (compression == 0) {
			samples = decode_psd_channel(reader, file_depth, width, height, offset, 0, 1);
			offset += area * sample_bytes;
		} else if (compression == 1) {
			Record *bytes = create_channels(area * sample_bytes, 8);
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
	set_number(record, "P9", 4);
	Record *palette = get_record(record, "rm");
	if (mode == 2 && palette && palette->type == RECORD_BUFFER) {
		for (gsize index = 0; index < area; index++) {
			int entry = read_channel(buffer, index * 4);
			for (int component = 0; component < 3; component++)
				write_channel(buffer, index * 4 + component, read_channel(palette, component * 256 + entry));
		}
	}
	return offset;
}
