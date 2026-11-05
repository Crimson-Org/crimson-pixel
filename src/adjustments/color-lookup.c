#include <limits.h>
#include <math.h>
#include <string.h>

#include "document/numbers.h"
#include "color-lookup.h"
#include "color/color-spaces.h"

/* Profiles with more tags or process elements than these are refused */
#define ICC_TAG_LIMIT 100
#define PROCESS_ELEMENT_LIMIT 256
/* mft tags keep their tables after a 48 byte header, and mft1 tables always have 256 entries */
#define MFT_HEADER_SIZE 48
#define MFT1_ENTRIES 256
/* mpet tags list their elements after the channel counts and the element count, as offset and size pairs */
#define PROCESS_COUNT_AT 4
#define PROCESS_ENTRY_SIZE 8
/* A curv tag keeps its count and then its values, and a parametric curve has one of five parameter counts */
#define CURVE_HEADER_SIZE (ICC_TAG_HEAD + ICC_NUMBER_BYTES)
static const int PARAMETRIC_COUNTS[] = {1, 3, 4, 5, 7};
/* Positions are pulled in a hair, so the last entry is never passed */
#define LAST_ENTRY_PULL .9999999
#define GRID_EDGE_PULL 1.000001
/* Blending a grid cell starts from its four corner pairs */
#define CELL_CORNERS 4

static const double ICC_RGB_TO_XYZ[LUT_CHANNELS * LUT_CHANNELS] = {.4360747, .3850649, .14308038, .2225045, .7168786,
										 .0606169, .0139322, .0971045,	.7141733};

/* One parsed tag of an ICC profile, holding the parts the tag's type has */
typedef struct IccTag IccTag;

struct IccTag {
	char type[ICC_SIGNATURE_BYTES + 1];
	gsize size;
	/* curv table or gamma, para parameters and XYZ values */
	double *values;
	int value_count;
	/* mAB, mft1, mft2 and mpet clut parts, with grid points per axis */
	int inputs, outputs, grid;
	double *table;
	gsize table_count;
	double matrix[ICC_MATRIX_VALUES];
	gboolean has_matrix, has_table;
	/* mAB M curves and mft input and output tables */
	IccTag **curves;
	int curve_count;
	double **input_tables, **output_tables;
	int input_entries, output_entries;
	/* mpet elements */
	IccTag **elements;
	int element_count;
};

typedef struct {
	char space_in[ICC_SIGNATURE_BYTES + 1], space_out[ICC_SIGNATURE_BYTES + 1];
	GHashTable *tags;
} IccProfile;

/* A read past the profile end makes the whole profile unusable */
typedef struct {
	const guint8 *data;
	gsize length;
	gboolean failed;
} IccReader;

/* ============================== Reading ========================================= */

static guint read_byte(IccReader *reader, gsize offset) {
	if (offset >= reader->length) {
		reader->failed = TRUE;
		return 0;
	}
	return reader->data[offset];
}

static guint read_u16(IccReader *reader, gsize offset) {
	return read_byte(reader, offset) << CHAR_BIT | read_byte(reader, offset + 1);
}

static gint32 read_i32(IccReader *reader, gsize offset) {
	guint32 value = 0;
	for (int index = 0; index < ICC_NUMBER_BYTES; index++)
		value = value << CHAR_BIT | read_byte(reader, offset + index);
	return (gint32)value;
}

static double read_fixed(IccReader *reader, gsize offset) {
	return (gint16)read_u16(reader, offset) + read_u16(reader, offset + sizeof(guint16)) * (1 / ICC_FIXED_ONE);
}

static void read_signature(IccReader *reader, gsize offset, char signature[ICC_SIGNATURE_BYTES + 1]) {
	for (int index = 0; index < ICC_SIGNATURE_BYTES; index++)
		signature[index] = read_byte(reader, offset + index);
	signature[ICC_SIGNATURE_BYTES] = 0;
}

static void free_tag(IccTag *tag) {
	if (!tag)
		return;
	g_free(tag->values);
	g_free(tag->table);
	for (int index = 0; index < tag->curve_count; index++)
		free_tag(tag->curves[index]);
	g_free(tag->curves);
	for (int index = 0; index < tag->inputs && tag->input_tables; index++)
		g_free(tag->input_tables[index]);
	for (int index = 0; index < tag->outputs && tag->output_tables; index++)
		g_free(tag->output_tables[index]);
	g_free(tag->input_tables);
	g_free(tag->output_tables);
	for (int index = 0; index < tag->element_count; index++)
		free_tag(tag->elements[index]);
	g_free(tag->elements);
	g_free(tag);
}

static IccTag *read_tag(IccReader *reader, gsize offset, gsize size);

/* Tables are only made once their values are known to lie inside the profile, so a damaged size never asks for more
 * memory than the profile could hold */
static gboolean has_icc_bytes(IccReader *reader, gsize offset, gsize count) {
	if (offset <= reader->length && count <= reader->length - offset)
		return TRUE;
	reader->failed = TRUE;
	return FALSE;
}


/* A colour lookup maps the three colour channels to three, and anything else is not a table this adjustment uses */
static gboolean is_colour_table(IccReader *reader, int inputs, int outputs) {
	if (inputs == LUT_CHANNELS && outputs == LUT_CHANNELS)
		return TRUE;
	reader->failed = TRUE;
	return FALSE;
}


/* Only curve tags count as curves, so two tags pointing at each other can never read without end */
static gboolean is_curve_at(IccReader *reader, gsize offset) {
	char type[ICC_SIGNATURE_BYTES + 1];
	read_signature(reader, offset, type);
	if (!strcmp(type, "curv") || !strcmp(type, "para"))
		return TRUE;
	reader->failed = TRUE;
	return FALSE;
}

static void read_ab_tag(IccReader *reader, IccTag *tag, gsize offset) {
	gsize start = offset - ICC_TAG_HEAD;
	tag->inputs = read_byte(reader, start + MAB_INPUT_CHANNELS);
	tag->outputs = read_byte(reader, start + MAB_OUTPUT_CHANNELS);
	gint32 matrix = read_i32(reader, start + MAB_MATRIX_OFFSET), curves = read_i32(reader, start + MAB_M_CURVES_OFFSET);
	gint32 table = read_i32(reader, start + MAB_CLUT_OFFSET);
	if (!is_colour_table(reader, tag->inputs, tag->outputs))
		return;
	if (matrix) {
		tag->has_matrix = TRUE;
		for (int index = 0; index < ICC_MATRIX_VALUES; index++)
			tag->matrix[index] = read_fixed(reader, start + matrix + index * ICC_NUMBER_BYTES);
	}
	if (curves) {
		tag->curves = g_new0(IccTag *, tag->outputs);
		gsize position = start + curves;
		for (int index = 0; index < tag->outputs && is_curve_at(reader, position); index++) {
			IccTag *curve = read_tag(reader, position, 0);
			tag->curves[tag->curve_count++] = curve;
			position += curve->size;
		}
	}
	if (!table)
		return;
	gsize position = start + table;
	tag->grid = read_byte(reader, position);
	gsize count = tag->outputs;
	for (int index = 0; index < tag->inputs; index++)
		count *= read_byte(reader, position + index);
	int precision = read_byte(reader, position + CLUT_PRECISION);
	position += CLUT_HEADER_SIZE;
	gboolean is_known_precision = precision == sizeof(guint8) || precision == sizeof(guint16);
	if (!count || !is_known_precision || !has_icc_bytes(reader, position, count * precision)) {
		reader->failed = TRUE;
		return;
	}
	tag->has_table = TRUE;
	tag->table = g_new0(double, MAX(count, 1));
	tag->table_count = count;
	for (gsize index = 0; index < count && !reader->failed; index++)
		tag->table[index] = precision == sizeof(guint8)	   ? read_byte(reader, position + index) * BYTE_UNIT
							: precision == sizeof(guint16) ? read_u16(reader, position + sizeof(guint16) * index) * WORD_UNIT
														   : 0;
}

static double **read_mft_tables(IccReader *reader, gsize *offset, int channels, int entries, gboolean wide) {
	double **tables = g_new0(double *, channels);
	for (int channel = 0; channel < channels; channel++) {
		tables[channel] = g_new(double, MAX(entries, 1));
		for (int entry = 0; entry < entries; entry++) {
			tables[channel][entry] = wide ? read_u16(reader, *offset) * WORD_UNIT : read_byte(reader, *offset) * BYTE_UNIT;
			*offset += wide ? sizeof(guint16) : sizeof(guint8);
		}
	}
	return tables;
}

static void read_mft_tag(IccReader *reader, IccTag *tag, gsize offset, gboolean wide) {
	/* Channels in, out and the grid size come first */
	tag->inputs = read_byte(reader, offset);
	tag->outputs = read_byte(reader, offset + 1);
	tag->grid = read_byte(reader, offset + 2);
	offset += MFT_HEADER_SIZE - ICC_TAG_HEAD;
	tag->input_entries = tag->output_entries = MFT1_ENTRIES;
	if (wide) {
		tag->input_entries = read_u16(reader, offset);
		tag->output_entries = read_u16(reader, offset + sizeof(guint16));
		offset += 2 * sizeof(guint16);
	}
	if (!is_colour_table(reader, tag->inputs, tag->outputs))
		return;
	gsize value_bytes = wide ? sizeof(guint16) : sizeof(guint8);
	gsize count = (gsize)tag->grid * tag->grid * tag->grid * tag->outputs;
	gsize values = (gsize)tag->inputs * tag->input_entries + count + (gsize)tag->outputs * tag->output_entries;
	if (!has_icc_bytes(reader, offset, values * value_bytes))
		return;
	tag->input_tables = read_mft_tables(reader, &offset, tag->inputs, tag->input_entries, wide);
	tag->has_table = TRUE;
	tag->table = g_new(double, MAX(count, 1));
	tag->table_count = count;
	for (gsize index = 0; index < count; index++) {
		tag->table[index] = wide ? read_u16(reader, offset) * WORD_UNIT : read_byte(reader, offset) * BYTE_UNIT;
		offset += wide ? sizeof(guint16) : sizeof(guint8);
	}
	tag->output_tables = read_mft_tables(reader, &offset, tag->outputs, tag->output_entries, wide);
}

static void read_process_tag(IccReader *reader, IccTag *tag, gsize offset) {
	gsize start = offset - ICC_TAG_HEAD;
	guint32 count = read_i32(reader, offset + PROCESS_COUNT_AT);
	offset += PROCESS_COUNT_AT + ICC_NUMBER_BYTES;
	if (count > PROCESS_ELEMENT_LIMIT) {
		reader->failed = TRUE;
		return;
	}
	tag->elements = g_new0(IccTag *, MAX(count, 1));
	for (guint32 index = 0; index < count && !reader->failed; index++) {
		IccTag *element = g_new0(IccTag, 1);
		tag->elements[tag->element_count++] = element;
		gsize position = start + read_i32(reader, offset);
		offset += PROCESS_ENTRY_SIZE;
		/* An element has its type, four zero bytes, and its channels in and out */
		read_signature(reader, position, element->type);
		position += ICC_SIGNATURE_BYTES;
		if (read_i32(reader, position) != 0)
			reader->failed = TRUE;
		position += ICC_NUMBER_BYTES;
		element->inputs = read_u16(reader, position);
		element->outputs = read_u16(reader, position + sizeof(guint16));
		position += 2 * sizeof(guint16);
		if (strcmp(element->type, "clut"))
			continue;
		if (!is_colour_table(reader, element->inputs, element->outputs))
			return;
		element->grid = read_byte(reader, position);
		position += CLUT_PRECISION;
		gsize values = (gsize)element->grid * element->grid * element->grid * element->outputs;
		if (!has_icc_bytes(reader, position, values * sizeof(float)))
			return;
		element->has_table = TRUE;
		element->table = g_new(double, MAX(values, 1));
		element->table_count = values;
		for (gsize item = 0; item < values; item++, position += sizeof(float)) {
			guint32 bits = (guint32)read_i32(reader, position);
			float value;
			memcpy(&value, &bits, sizeof value);
			element->table[item] = value;
		}
	}
}

static IccTag *read_tag(IccReader *reader, gsize offset, gsize size) {
	IccTag *tag = g_new0(IccTag, 1);
	tag->size = size;
	read_signature(reader, offset, tag->type);
	offset += ICC_TAG_HEAD;
	if (!strcmp(tag->type, "mAB "))
		read_ab_tag(reader, tag, offset);
	else if (!strcmp(tag->type, "mft1"))
		read_mft_tag(reader, tag, offset, FALSE);
	else if (!strcmp(tag->type, "mft2"))
		read_mft_tag(reader, tag, offset, TRUE);
	else if (!strcmp(tag->type, "mpet"))
		read_process_tag(reader, tag, offset);
	else if (!strcmp(tag->type, "XYZ ")) {
		tag->values = g_new(double, LUT_CHANNELS);
		tag->value_count = LUT_CHANNELS;
		for (int index = 0; index < LUT_CHANNELS; index++)
			tag->values[index] = read_fixed(reader, offset + index * ICC_NUMBER_BYTES);
	} else if (!strcmp(tag->type, "para")) {
		guint kind = read_u16(reader, offset);
		if (kind >= G_N_ELEMENTS(PARAMETRIC_COUNTS))
			reader->failed = TRUE;
		else {
			tag->value_count = PARAMETRIC_COUNTS[kind];
			tag->values = g_new(double, tag->value_count);
			for (int index = 0; index < tag->value_count; index++)
				tag->values[index] = read_fixed(reader, offset + (index + 1) * ICC_NUMBER_BYTES);
		}
	} else if (!strcmp(tag->type, "curv")) {
		gint32 count = read_i32(reader, offset);
		if (count < 0 || (gsize)count > reader->length)
			reader->failed = TRUE;
		else {
			tag->value_count = count;
			tag->values = g_new(double, MAX(count, 1));
			gsize values = offset + ICC_NUMBER_BYTES;
			/* One value is a gamma, and more are the curve's points */
			if (count == 1)
				tag->values[0] = read_byte(reader, values) + read_byte(reader, values + 1) / ICC_GAMMA_FIXED_ONE;
			else
				for (gint32 index = 0; index < count; index++)
					tag->values[index] = read_u16(reader, values + index * sizeof(guint16)) * WORD_UNIT;
			tag->size = CURVE_HEADER_SIZE + sizeof(guint16) * count;
		}
	}
	if (tag->size % ICC_ALIGNMENT)
		tag->size += ICC_ALIGNMENT - tag->size % ICC_ALIGNMENT;
	return tag;
}

static void free_profile(IccProfile *profile) {
	g_hash_table_unref(profile->tags);
	g_free(profile);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(IccProfile, free_profile)

static IccProfile *read_profile(const guint8 *data, gsize length) {
	IccReader reader = {data, length, FALSE};
	gint32 count = read_i32(&reader, ICC_HEADER_SIZE);
	if (reader.failed || count > ICC_TAG_LIMIT)
		return NULL;
	IccProfile *profile = g_new0(IccProfile, 1);
	profile->tags = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)free_tag);
	read_signature(&reader, ICC_SPACE_IN_AT, profile->space_in);
	read_signature(&reader, ICC_SPACE_OUT_AT, profile->space_out);
	for (gint32 index = 0; index < count && !reader.failed; index++) {
		gsize entry = ICC_HEADER_SIZE + ICC_NUMBER_BYTES + index * ICC_TAG_ENTRY_SIZE;
		char signature[ICC_SIGNATURE_BYTES + 1];
		read_signature(&reader, entry, signature);
		IccTag *tag =
			read_tag(&reader, read_i32(&reader, entry + TAG_ENTRY_OFFSET), read_i32(&reader, entry + TAG_ENTRY_SIZE));
		g_hash_table_replace(profile->tags, g_strdup(signature), tag);
	}
	if (!reader.failed)
		return profile;
	free_profile(profile);
	return NULL;
}

/* ============================== Sampling ======================================== */

/* A read past the table end gives NaN, which carries through the arithmetic */
static double read_item(const double *values, gsize count, double index) {
	return index >= 0 && index < count && values ? values[(gsize)index] : NAN;
}

static void convert_rgb_to_xyz(const double rgb[LUT_CHANNELS], double xyz[LUT_CHANNELS]) {
	double red = read_linear_table(rgb[0] * COLOR_TABLE_SCALE);
	double green = read_linear_table(rgb[1] * COLOR_TABLE_SCALE);
	double blue = read_linear_table(rgb[2] * COLOR_TABLE_SCALE);
	for (int row = 0; row < LUT_CHANNELS; row++) {
		const double *weights = ICC_RGB_TO_XYZ + row * LUT_CHANNELS;
		xyz[row] = weights[0] * red + weights[1] * green + weights[2] * blue;
	}
}

/* Lab with the lightness in hundredths, and a and b centered on half a byte, all as fractions */
static void convert_xyz_to_fractions(const double xyz[LUT_CHANNELS], const double *white, double *point) {
	double x = xyz[0] * (1 / white[0]), y = xyz[1] * (1 / white[1]), z = xyz[2] < 0 ? 0 : xyz[2] * (1 / white[2]);
	double fx = read_lab_table(x * COLOR_TABLE_SCALE), fy = read_lab_table(y * COLOR_TABLE_SCALE);
	double fz = read_lab_table(z * COLOR_TABLE_SCALE);
	point[0] = from_percent(116 * fy - 16);
	point[1] = byte_to_unit(127.5 + 500 * (fx - fy));
	point[2] = byte_to_unit(127.5 + 200 * (fy - fz));
}

static void convert_lab_to_fractions(double lightness, double a, double b, const double *white, double *point) {
	double shares[LUT_CHANNELS];
	convert_lab_to_white_shares(lightness, a, b, shares);
	double xyz[LUT_CHANNELS] = {shares[0] * white[0], shares[1] * white[1], shares[2] * white[2]};
	for (int row = 0; row < LUT_CHANNELS; row++) {
		const double *weights = XYZ_TO_RGB + row * (LUT_CHANNELS + 1);
		point[row] = MAX(0, MIN(1, encode_srgb(weights[0] * xyz[0] + weights[1] * xyz[1] + weights[2] * xyz[2])));
	}
}

static double evaluate_table_curve(double value, const double *values, int count) {
	if (count == 0)
		return value;
	if (count == 1)
		return pow(value, values[0]);
	double position = value * (count - 1) * LAST_ENTRY_PULL, whole = trunc(position), part = position - whole;
	return (1 - part) * read_item(values, count, whole) + part * read_item(values, count, whole + 1);
}

static double evaluate_parametric_curve(double value, const double *parameters, int count) {
	double gamma = parameters[0];
	if (count == 1)
		return pow(value, gamma);
	double a = parameters[1], b = parameters[2];
	if (count == 3)
		return value >= -b / a ? pow(a * value + b, gamma) : 0;
	double c = parameters[3];
	if (count == 4)
		return value >= -b / a ? pow(a * value + b, gamma) + c : c;
	double d = parameters[4];
	if (count == 5)
		return value >= d ? pow(a * value + b, gamma) : c * value;
	return value >= d ? pow(a * value + b, gamma) + parameters[5] : c * value + parameters[6];
}

static double evaluate_curve(double value, const IccTag *curve) {
	if (!strcmp(curve->type, "curv"))
		return evaluate_table_curve(value, curve->values, curve->value_count);
	return evaluate_parametric_curve(value, curve->values, curve->value_count);
}

static gboolean is_curve(const IccTag *tag) {
	return tag && (!strcmp(tag->type, "curv") || !strcmp(tag->type, "para")) && (tag->value_count || tag->values);
}

static void blend_entries(const double *from, gsize count, double first, double second, double part, int slot,
						  double *working) {
	for (int channel = 0; channel < LUT_CHANNELS; channel++) {
		double one = read_item(from, count, first * LUT_CHANNELS + channel);
		double two = read_item(from, count, second * LUT_CHANNELS + channel);
		working[slot * LUT_CHANNELS + channel] = (1 - part) * one + part * two;
	}
}

static void interpolate_grid(double red, double green, double blue, int grid, const double *table, gsize count,
							 double *working) {
	double red_index = trunc(red), green_index = trunc(green), blue_index = trunc(blue), plane = grid * grid;
	double near = blue_index + grid * green_index + plane * red_index;
	double far = blue_index + grid * (green_index + 1) + plane * red_index;
	blend_entries(table, count, near, near + 1, blue - blue_index, 0, working);
	blend_entries(table, count, far, far + 1, blue - blue_index, 1, working);
	blend_entries(table, count, near + plane, near + 1 + plane, blue - blue_index, 2, working);
	blend_entries(table, count, far + plane, far + 1 + plane, blue - blue_index, 3, working);
	const gsize cells = CELL_CORNERS * LUT_CHANNELS;
	blend_entries(working, cells, 0, 1, green - green_index, 0, working);
	blend_entries(working, cells, 2, 3, green - green_index, 2, working);
	blend_entries(working, cells, 0, 2, red - red_index, 0, working);
}

static void apply_grid_to_point(const double *table, gsize count, int grid, double *point) {
	double above[LUT_CHANNELS] = {0, 0, 0}, values[LUT_CHANNELS], working[CELL_CORNERS * LUT_CHANNELS];
	double scale = grid - GRID_EDGE_PULL;
	for (int channel = 0; channel < LUT_CHANNELS; channel++) {
		values[channel] = point[channel];
		if (values[channel] > 1) {
			above[channel] = values[channel] - 1;
			values[channel] = 1;
		}
	}
	interpolate_grid(scale * values[0], scale * values[1], scale * values[2], grid, table, count, working);
	for (int channel = 0; channel < LUT_CHANNELS; channel++)
		point[channel] = working[channel] + above[channel];
}

static gboolean apply_ab_tag(const IccTag *tag, double *point) {
	if (tag->has_table) {
		if (tag->outputs != LUT_CHANNELS || (tag->inputs && tag->inputs != LUT_CHANNELS))
			return FALSE;
		apply_grid_to_point(tag->table, tag->table_count, tag->grid, point);
	}
	if (tag->curves) {
		if (tag->curve_count < LUT_CHANNELS)
			return FALSE;
		for (int channel = 0; channel < LUT_CHANNELS; channel++)
			point[channel] = evaluate_curve(point[channel], tag->curves[channel]);
	}
	if (tag->has_matrix) {
		/* The three by three part first, then the offsets of the rows */
		const double *m = tag->matrix, red = point[0], green = point[1], blue = point[2];
		point[0] = MAX(0, MIN(1, m[0] * red + m[1] * green + m[2] * blue + m[9]));
		point[1] = MAX(0, MIN(1, m[3] * red + m[4] * green + m[5] * blue + m[10]));
		point[2] = MAX(0, MIN(1, m[6] * red + m[7] * green + m[8] * blue + m[11]));
	}
	return TRUE;
}

static gboolean apply_mft_tag(const IccTag *tag, double *point) {
	if (tag->inputs != LUT_CHANNELS || tag->outputs != LUT_CHANNELS)
		return FALSE;
	for (int channel = 0; channel < LUT_CHANNELS; channel++)
		point[channel] = evaluate_table_curve(point[channel], tag->input_tables[channel], tag->input_entries);
	apply_grid_to_point(tag->table, tag->table_count, tag->grid, point);
	for (int channel = 0; channel < LUT_CHANNELS; channel++)
		point[channel] = evaluate_table_curve(point[channel], tag->output_tables[channel], tag->output_entries);
	return TRUE;
}

static IccTag *find_tag(const IccProfile *profile, const char *signature) {
	return g_hash_table_lookup(profile->tags, signature);
}

static gboolean sample_point(const IccProfile *profile, double *point) {
	const char *input = profile->space_in, *output = profile->space_out;
	if (!g_ascii_strcasecmp(input, "lab ")) {
		double xyz[LUT_CHANNELS];
		convert_rgb_to_xyz(point, xyz);
		convert_xyz_to_fractions(xyz, D50_WHITE, point);
	} else if (!g_ascii_strcasecmp(input, "xyz ")) {
		double xyz[LUT_CHANNELS];
		convert_rgb_to_xyz(point, xyz);
		memcpy(point, xyz, sizeof xyz);
	} else if (g_ascii_strcasecmp(input, "rgb ") && g_ascii_strcasecmp(input, "gray"))
		return FALSE;
	IccTag *curves[] = {find_tag(profile, "rTRC"), find_tag(profile, "gTRC"), find_tag(profile, "bTRC")};
	if (curves[0]) {
		for (int channel = 0; channel < LUT_CHANNELS; channel++)
			if (!is_curve(curves[channel]))
				return FALSE;
		for (int channel = 0; channel < LUT_CHANNELS; channel++)
			point[channel] = evaluate_curve(point[channel], curves[channel]);
	}
	IccTag *columns[] = {find_tag(profile, "rXYZ"), find_tag(profile, "gXYZ"), find_tag(profile, "bXYZ")};
	if (columns[0]) {
		for (int column = 0; column < LUT_CHANNELS; column++)
			if (!columns[column] || columns[column]->value_count != LUT_CHANNELS)
				return FALSE;
		double red = point[0], green = point[1], blue = point[2];
		for (int row = 0; row < LUT_CHANNELS; row++)
			point[row] = MAX(0, MIN(1, red * columns[0]->values[row] + green * columns[1]->values[row] +
										   blue * columns[2]->values[row]));
	}
	IccTag *forward = find_tag(profile, "A2B0");
	if (forward) {
		if (!strcmp(forward->type, "mAB ")) {
			if (!apply_ab_tag(forward, point))
				return FALSE;
		} else if (!strcmp(forward->type, "mft1") || !strcmp(forward->type, "mft2")) {
			if (!apply_mft_tag(forward, point))
				return FALSE;
		} else
			return FALSE;
	}
	IccTag *process = find_tag(profile, "D2B0");
	for (int index = 0; process && index < process->element_count; index++)
		if (!apply_ab_tag(process->elements[index], point))
			return FALSE;
	if (!g_ascii_strcasecmp(output, "rgb "))
		return TRUE;
	IccTag *white_tag = find_tag(profile, "wtpt");
	const double *white = white_tag && white_tag->value_count == LUT_CHANNELS ? white_tag->values : D50_WHITE;
	if (!g_ascii_strcasecmp(output, "xyz ")) {
		double xyz[LUT_CHANNELS] = {point[0], point[1], point[2]};
		convert_xyz_to_fractions(xyz, white, point);
	} else if (g_ascii_strcasecmp(output, "lab "))
		return FALSE;
	convert_lab_to_fractions(point[0] * PERCENT, -127.5 + G_MAXUINT8 * point[1], -127.5 + G_MAXUINT8 * point[2], white, point);
	return TRUE;
}

/* Every redraw checks and applies the same profile, so the last sampled one is kept */
static GMutex last_lock;
static GBytes *last_profile;
static double last_samples[COLOR_LOOKUP_SAMPLES];

static gboolean sample_profile(const guint8 *data, gsize length, double *samples);

gboolean sample_color_lookup(const guint8 *data, gsize length, double *samples) {
	g_mutex_lock(&last_lock);
	gsize last_length = 0;
	const guint8 *last = last_profile ? g_bytes_get_data(last_profile, &last_length) : NULL;
	gboolean found = last && last_length == length && !memcmp(last, data, length);
	if (found)
		memcpy(samples, last_samples, sizeof last_samples);
	g_mutex_unlock(&last_lock);
	if (found)
		return TRUE;
	if (!sample_profile(data, length, samples))
		return FALSE;
	g_mutex_lock(&last_lock);
	g_clear_pointer(&last_profile, g_bytes_unref);
	last_profile = g_bytes_new(data, length);
	memcpy(last_samples, samples, sizeof last_samples);
	g_mutex_unlock(&last_lock);
	return TRUE;
}

static gboolean sample_profile(const guint8 *data, gsize length, double *samples) {
	g_autoptr(IccProfile) profile = read_profile(data, length);
	if (!profile)
		return FALSE;
	const double step = 1.0 / (COLOR_LOOKUP_GRID - 1);
	gsize index = 0;
	for (int red = 0; red < COLOR_LOOKUP_GRID; red++)
		for (int green = 0; green < COLOR_LOOKUP_GRID; green++)
			for (int blue = 0; blue < COLOR_LOOKUP_GRID; blue++, index += LUT_CHANNELS) {
				/* The fourth place gives room for a gray profile's extra channel */
				double point[LUT_CHANNELS + 1] = {red * step, green * step, blue * step, 0};
				if (!sample_point(profile, point))
					return FALSE;
				memcpy(samples + index, point, LUT_CHANNELS * sizeof *point);
			}
	return TRUE;
}

/* ============================== Applying ======================================== */

/* A unit value rounded to a byte, where values out of range wrap */
static guint8 store_rounded_byte(double value) {
	return wrap_byte(.5 + value * G_MAXUINT8);
}

/* Each channel byte always falls in the same grid cell and part of it, so those are worked out once */
static int cell_of_level[G_MAXUINT8 + 1];
static double part_of_level[G_MAXUINT8 + 1];

static void fill_cells(void) {
	static gsize ready;
	if (!g_once_init_enter(&ready))
		return;
	const double scale = COLOR_LOOKUP_GRID - GRID_EDGE_PULL, unit = BYTE_UNIT;
	for (int level = 0; level <= G_MAXUINT8; level++) {
		double coordinate = scale * (unit * level);
		cell_of_level[level] = (int)coordinate;
		part_of_level[level] = coordinate - cell_of_level[level];
	}
	g_once_init_leave(&ready, 1);
}

/* The bytes always fall inside the grid, so blending the surrounding cells never reads past the table */
void lookup_color_pixel(const double *samples, guint8 *pixel) {
	fill_cells();
	const int grid = COLOR_LOOKUP_GRID, plane = grid * grid;
	int red = cell_of_level[pixel[0]], green = cell_of_level[pixel[1]], blue = cell_of_level[pixel[2]];
	double red_part = part_of_level[pixel[0]], green_part = part_of_level[pixel[1]],
		   blue_part = part_of_level[pixel[2]];
	int near = blue + grid * green + plane * red, far = blue + grid * (green + 1) + plane * red;
	const double *corners[CELL_CORNERS] = {samples + near * LUT_CHANNELS, samples + far * LUT_CHANNELS,
										   samples + (near + plane) * LUT_CHANNELS, samples + (far + plane) * LUT_CHANNELS};
	/* Each corner pair blends along blue, then the pairs along green and the halves along red */
	double working[CELL_CORNERS][LUT_CHANNELS];
	for (int corner = 0; corner < CELL_CORNERS; corner++)
		for (int channel = 0; channel < LUT_CHANNELS; channel++)
			working[corner][channel] =
				(1 - blue_part) * corners[corner][channel] + blue_part * corners[corner][channel + LUT_CHANNELS];
	for (int channel = 0; channel < LUT_CHANNELS; channel++) {
		working[0][channel] = (1 - green_part) * working[0][channel] + green_part * working[1][channel];
		working[2][channel] = (1 - green_part) * working[2][channel] + green_part * working[3][channel];
	}
	for (int channel = 0; channel < LUT_CHANNELS; channel++)
		pixel[channel] = store_rounded_byte((1 - red_part) * working[0][channel] + red_part * working[2][channel]);
}
