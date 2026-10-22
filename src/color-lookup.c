#include <math.h>
#include <string.h>

#include "color-lookup.h"
#include "color-spaces.h"

enum { ICC_TAG_LIMIT = 100 };

static const double ICC_WHITE[3] = {.9642, 1, .8249};
static const double ICC_XYZ_TO_RGB[9] = {3.1338561, -1.6168667, -.4906146, -.9787684, 1.9161415,
										 .033454,	.0719453,	-.2289914, 1.4052427};
static const double ICC_RGB_TO_XYZ[9] = {.4360747, .3850649, .14308038, .2225045, .7168786,
										 .0606169, .0139322, .0971045,	.7141733};

/* One parsed tag of ICC.R, holding the parts its type has */
typedef struct IccTag IccTag;

struct IccTag {
	char type[5];
	gsize size;
	/* curv table or gamma, para parameters and XYZ values */
	double *values;
	int value_count;
	/* mAB, mft1, mft2 and mpet clut parts, with grid points per axis */
	int inputs, outputs, grid;
	double *table;
	gsize table_count;
	double matrix[12];
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
	char space_in[5], space_out[5];
	GHashTable *tags;
} IccProfile;

/* Reads past the profile end make the whole profile unusable instead of reading undefined values */
typedef struct {
	const guint8 *data;
	gsize length;
	gboolean failed;
} IccReader;

static double linear_table[8000], lab_table[8000];

/* ============================== Reading ========================================= */

static guint read_byte(IccReader *reader, gsize offset) {
	if (offset >= reader->length) {
		reader->failed = TRUE;
		return 0;
	}
	return reader->data[offset];
}

static guint read_u16(IccReader *reader, gsize offset) {
	return read_byte(reader, offset) << 8 | read_byte(reader, offset + 1);
}

static gint32 read_i32(IccReader *reader, gsize offset) {
	return (gint32)(read_byte(reader, offset) << 24 | read_byte(reader, offset + 1) << 16 |
					read_byte(reader, offset + 2) << 8 | read_byte(reader, offset + 3));
}

static double read_fixed(IccReader *reader, gsize offset) {
	return (gint16)read_u16(reader, offset) + read_u16(reader, offset + 2) * (1 / 65536.0);
}

static void read_signature(IccReader *reader, gsize offset, char signature[5]) {
	for (int index = 0; index < 4; index++)
		signature[index] = read_byte(reader, offset + index);
	signature[4] = 0;
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

static void read_ab_tag(IccReader *reader, IccTag *tag, gsize offset) {
	gsize start = offset - 8;
	tag->inputs = read_byte(reader, offset);
	tag->outputs = read_byte(reader, offset + 1);
	gint32 matrix = read_i32(reader, offset + 8), curves = read_i32(reader, offset + 12);
	gint32 table = read_i32(reader, offset + 16);
	if (matrix) {
		tag->has_matrix = TRUE;
		for (int index = 0; index < 12; index++)
			tag->matrix[index] = read_fixed(reader, start + matrix + index * 4);
	}
	if (curves) {
		tag->curves = g_new0(IccTag *, tag->outputs);
		gsize position = start + curves;
		for (int index = 0; index < tag->outputs && !reader->failed; index++) {
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
	int precision = read_byte(reader, position + 16);
	position += 20;
	tag->has_table = TRUE;
	tag->table = g_new0(double, MAX(count, 1));
	tag->table_count = count;
	for (gsize index = 0; index < count && !reader->failed; index++)
		tag->table[index] = precision == 1	 ? read_byte(reader, position + index) * (1 / 255.0)
							: precision == 2 ? read_u16(reader, position + 2 * index) * (1 / 65535.0)
											 : 0;
}

static double **read_mft_tables(IccReader *reader, gsize *offset, int channels, int entries, gboolean wide) {
	double **tables = g_new0(double *, channels);
	for (int channel = 0; channel < channels; channel++) {
		tables[channel] = g_new(double, MAX(entries, 1));
		for (int entry = 0; entry < entries; entry++) {
			tables[channel][entry] =
				wide ? read_u16(reader, *offset) * (1 / 65535.0) : read_byte(reader, *offset) * (1 / 255.0);
			*offset += wide ? 2 : 1;
		}
	}
	return tables;
}

static void read_mft_tag(IccReader *reader, IccTag *tag, gsize offset, gboolean wide) {
	tag->inputs = read_byte(reader, offset);
	tag->outputs = read_byte(reader, offset + 1);
	tag->grid = read_byte(reader, offset + 2);
	offset += 40;
	tag->input_entries = tag->output_entries = 256;
	if (wide) {
		tag->input_entries = read_u16(reader, offset);
		tag->output_entries = read_u16(reader, offset + 2);
		offset += 4;
	}
	tag->input_tables = read_mft_tables(reader, &offset, tag->inputs, tag->input_entries, wide);
	gsize count = (gsize)round(pow(tag->grid, tag->inputs)) * tag->outputs;
	tag->has_table = TRUE;
	tag->table = g_new(double, MAX(count, 1));
	tag->table_count = count;
	for (gsize index = 0; index < count; index++) {
		tag->table[index] = wide ? read_u16(reader, offset) * (1 / 65535.0) : read_byte(reader, offset) * (1 / 255.0);
		offset += wide ? 2 : 1;
	}
	tag->output_tables = read_mft_tables(reader, &offset, tag->outputs, tag->output_entries, wide);
}

static void read_process_tag(IccReader *reader, IccTag *tag, gsize offset) {
	gsize start = offset - 8;
	guint32 count = read_i32(reader, offset + 4);
	offset += 8;
	if (count > 256) {
		reader->failed = TRUE;
		return;
	}
	tag->elements = g_new0(IccTag *, MAX(count, 1));
	for (guint32 index = 0; index < count && !reader->failed; index++) {
		IccTag *element = g_new0(IccTag, 1);
		tag->elements[tag->element_count++] = element;
		gsize position = start + read_i32(reader, offset);
		offset += 8;
		read_signature(reader, position, element->type);
		position += 4;
		if (read_i32(reader, position) != 0)
			reader->failed = TRUE;
		position += 4;
		element->inputs = read_u16(reader, position);
		element->outputs = read_u16(reader, position + 2);
		position += 4;
		if (strcmp(element->type, "clut"))
			continue;
		element->grid = read_byte(reader, position);
		position += 16;
		gsize values = (gsize)element->grid * element->grid * element->grid * element->outputs;
		element->has_table = TRUE;
		element->table = g_new(double, MAX(values, 1));
		element->table_count = values;
		for (gsize item = 0; item < values; item++, position += 4) {
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
	offset += 8;
	if (!strcmp(tag->type, "mAB "))
		read_ab_tag(reader, tag, offset);
	else if (!strcmp(tag->type, "mft1"))
		read_mft_tag(reader, tag, offset, FALSE);
	else if (!strcmp(tag->type, "mft2"))
		read_mft_tag(reader, tag, offset, TRUE);
	else if (!strcmp(tag->type, "mpet"))
		read_process_tag(reader, tag, offset);
	else if (!strcmp(tag->type, "XYZ ")) {
		tag->values = g_new(double, 3);
		tag->value_count = 3;
		for (int index = 0; index < 3; index++)
			tag->values[index] = read_fixed(reader, offset + index * 4);
	} else if (!strcmp(tag->type, "para")) {
		const int counts[] = {1, 3, 4, 5, 7};
		guint kind = read_u16(reader, offset);
		if (kind >= G_N_ELEMENTS(counts))
			reader->failed = TRUE;
		else {
			tag->value_count = counts[kind];
			tag->values = g_new(double, tag->value_count);
			for (int index = 0; index < tag->value_count; index++)
				tag->values[index] = read_fixed(reader, offset + 4 + index * 4);
		}
	} else if (!strcmp(tag->type, "curv")) {
		gint32 count = read_i32(reader, offset);
		if (count < 0 || (gsize)count > reader->length)
			reader->failed = TRUE;
		else {
			tag->value_count = count;
			tag->values = g_new(double, MAX(count, 1));
			if (count == 1)
				tag->values[0] = read_byte(reader, offset + 4) + read_byte(reader, offset + 5) / 256.0;
			else
				for (gint32 index = 0; index < count; index++)
					tag->values[index] = read_u16(reader, offset + 4 + index * 2) * (1 / 65535.0);
			tag->size = 12 + 2 * count;
		}
	}
	if (tag->size & 3)
		tag->size += 4 - (tag->size & 3);
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
	read_signature(&reader, 16, profile->space_in);
	read_signature(&reader, 20, profile->space_out);
	for (gint32 index = 0; index < count && !reader.failed; index++) {
		gsize entry = ICC_HEADER_SIZE + 4 + index * 12;
		char signature[5];
		read_signature(&reader, entry, signature);
		IccTag *tag = read_tag(&reader, read_i32(&reader, entry + 4), read_i32(&reader, entry + 8));
		g_hash_table_replace(profile->tags, g_strdup(signature), tag);
	}
	if (!reader.failed)
		return profile;
	free_profile(profile);
	return NULL;
}

/* ============================== Sampling ======================================== */

static void fill_tables(void) {
	static gsize ready;
	if (!g_once_init_enter(&ready))
		return;
	for (int index = 0; index < 8000; index++) {
		double value = index / 4e3;
		linear_table[index] = linearize_srgb(value);
		lab_table[index] = value > .008856 ? pow(value, 1 / 3.0) : (903.3 * value + 16) * (1 / 116.0);
	}
	g_once_init_leave(&ready, 1);
}

/* JavaScript reads undefined past an array end, which turns the arithmetic into NaN */
static double read_item(const double *values, gsize count, double index) {
	return index >= 0 && index < count && values ? values[(gsize)index] : NAN;
}

static double read_color_table(const double *table, double position) {
	return read_item(table, 8000, trunc(.5 + position));
}

static void convert_rgb_to_xyz(const double rgb[3], double xyz[3]) {
	double red = read_color_table(linear_table, rgb[0] * 4e3), green = read_color_table(linear_table, rgb[1] * 4e3);
	double blue = read_color_table(linear_table, rgb[2] * 4e3);
	for (int row = 0; row < 3; row++)
		xyz[row] =
			ICC_RGB_TO_XYZ[row * 3] * red + ICC_RGB_TO_XYZ[row * 3 + 1] * green + ICC_RGB_TO_XYZ[row * 3 + 2] * blue;
}

static void convert_xyz_to_fractions(const double xyz[3], const double *white, double *point) {
	double x = xyz[0] * (1 / white[0]), y = xyz[1] * (1 / white[1]), z = xyz[2] < 0 ? 0 : xyz[2] * (1 / white[2]);
	double fx = read_color_table(lab_table, x * 4e3), fy = read_color_table(lab_table, y * 4e3);
	double fz = read_color_table(lab_table, z * 4e3);
	point[0] = (116 * fy - 16) / 100;
	point[1] = (127.5 + 500 * (fx - fy)) / 255;
	point[2] = (127.5 + 200 * (fy - fz)) / 255;
}

static void convert_lab_to_fractions(double lightness, double a, double b, const double *white, double *point) {
	const double kappa = 903.3, epsilon = .008856;
	double fy = (lightness + 16) / 116, y_cube = fy * fy * fy, fz = fy - b / 200, z_cube = fz * fz * fz;
	double fx = a / 500 + fy, x_cube = fx * fx * fx;
	double z = z_cube > epsilon ? z_cube : (116 * fz - 16) / kappa;
	double y = y_cube > epsilon ? y_cube : (116 * fy - 16) / kappa;
	double x = x_cube > epsilon ? x_cube : (116 * fx - 16) / kappa;
	double xyz[3] = {x * white[0], y * white[1], z * white[2]};
	for (int row = 0; row < 3; row++)
		point[row] = MAX(0, MIN(1, encode_srgb(ICC_XYZ_TO_RGB[row * 3] * xyz[0] + ICC_XYZ_TO_RGB[row * 3 + 1] * xyz[1] +
											   ICC_XYZ_TO_RGB[row * 3 + 2] * xyz[2])));
}

static double evaluate_table_curve(double value, const double *values, int count) {
	if (count == 0)
		return value;
	if (count == 1)
		return pow(value, values[0]);
	double position = value * (count - 1) * .9999999, whole = trunc(position), part = position - whole;
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
	double one = read_item(from, count, first * 3), two = read_item(from, count, second * 3);
	double one_green = read_item(from, count, first * 3 + 1), two_green = read_item(from, count, second * 3 + 1);
	double one_blue = read_item(from, count, first * 3 + 2), two_blue = read_item(from, count, second * 3 + 2);
	working[slot * 3] = (1 - part) * one + part * two;
	working[slot * 3 + 1] = (1 - part) * one_green + part * two_green;
	working[slot * 3 + 2] = (1 - part) * one_blue + part * two_blue;
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
	blend_entries(working, 12, 0, 1, green - green_index, 0, working);
	blend_entries(working, 12, 2, 3, green - green_index, 2, working);
	blend_entries(working, 12, 0, 2, red - red_index, 0, working);
}

static void apply_grid_to_point(const double *table, gsize count, int grid, double *point) {
	double above[3] = {0, 0, 0}, values[3], working[12], scale = grid - 1.000001;
	for (int channel = 0; channel < 3; channel++) {
		values[channel] = point[channel];
		if (values[channel] > 1) {
			above[channel] = values[channel] - 1;
			values[channel] = 1;
		}
	}
	interpolate_grid(scale * values[0], scale * values[1], scale * values[2], grid, table, count, working);
	for (int channel = 0; channel < 3; channel++)
		point[channel] = working[channel] + above[channel];
}

static gboolean apply_ab_tag(const IccTag *tag, double *point) {
	if (tag->has_table) {
		if (tag->outputs != 3 || (tag->inputs && tag->inputs != 3))
			return FALSE;
		apply_grid_to_point(tag->table, tag->table_count, tag->grid, point);
	}
	if (tag->curves) {
		if (tag->curve_count < 3)
			return FALSE;
		for (int channel = 0; channel < 3; channel++)
			point[channel] = evaluate_curve(point[channel], tag->curves[channel]);
	}
	if (tag->has_matrix) {
		const double *m = tag->matrix, red = point[0], green = point[1], blue = point[2];
		point[0] = MAX(0, MIN(1, m[0] * red + m[1] * green + m[2] * blue + m[9]));
		point[1] = MAX(0, MIN(1, m[3] * red + m[4] * green + m[5] * blue + m[10]));
		point[2] = MAX(0, MIN(1, m[6] * red + m[7] * green + m[8] * blue + m[11]));
	}
	return TRUE;
}

static gboolean apply_mft_tag(const IccTag *tag, double *point) {
	if (tag->inputs != 3 || tag->outputs != 3)
		return FALSE;
	for (int channel = 0; channel < 3; channel++)
		point[channel] = evaluate_table_curve(point[channel], tag->input_tables[channel], tag->input_entries);
	apply_grid_to_point(tag->table, tag->table_count, tag->grid, point);
	for (int channel = 0; channel < 3; channel++)
		point[channel] = evaluate_table_curve(point[channel], tag->output_tables[channel], tag->output_entries);
	return TRUE;
}

static IccTag *find_tag(const IccProfile *profile, const char *signature) {
	return g_hash_table_lookup(profile->tags, signature);
}

static gboolean sample_point(const IccProfile *profile, double *point) {
	const char *input = profile->space_in, *output = profile->space_out;
	if (!g_ascii_strcasecmp(input, "lab ")) {
		double xyz[3];
		convert_rgb_to_xyz(point, xyz);
		convert_xyz_to_fractions(xyz, ICC_WHITE, point);
	} else if (!g_ascii_strcasecmp(input, "xyz ")) {
		double xyz[3];
		convert_rgb_to_xyz(point, xyz);
		memcpy(point, xyz, sizeof xyz);
	} else if (g_ascii_strcasecmp(input, "rgb ") && g_ascii_strcasecmp(input, "gray"))
		return FALSE;
	IccTag *curves[] = {find_tag(profile, "rTRC"), find_tag(profile, "gTRC"), find_tag(profile, "bTRC")};
	if (curves[0]) {
		for (int channel = 0; channel < 3; channel++)
			if (!is_curve(curves[channel]))
				return FALSE;
		for (int channel = 0; channel < 3; channel++)
			point[channel] = evaluate_curve(point[channel], curves[channel]);
	}
	IccTag *columns[] = {find_tag(profile, "rXYZ"), find_tag(profile, "gXYZ"), find_tag(profile, "bXYZ")};
	if (columns[0]) {
		for (int column = 0; column < 3; column++)
			if (!columns[column] || columns[column]->value_count != 3)
				return FALSE;
		double red = point[0], green = point[1], blue = point[2];
		for (int row = 0; row < 3; row++)
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
	const double *white = white_tag && white_tag->value_count == 3 ? white_tag->values : ICC_WHITE;
	if (!g_ascii_strcasecmp(output, "xyz ")) {
		double xyz[3] = {point[0], point[1], point[2]};
		convert_xyz_to_fractions(xyz, white, point);
	} else if (g_ascii_strcasecmp(output, "lab "))
		return FALSE;
	convert_lab_to_fractions(point[0] * 100, -127.5 + 255 * point[1], -127.5 + 255 * point[2], white, point);
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
	fill_tables();
	g_autoptr(IccProfile) profile = read_profile(data, length);
	if (!profile)
		return FALSE;
	const double step = 1.0 / (COLOR_LOOKUP_GRID - 1);
	gsize index = 0;
	for (int red = 0; red < COLOR_LOOKUP_GRID; red++)
		for (int green = 0; green < COLOR_LOOKUP_GRID; green++)
			for (int blue = 0; blue < COLOR_LOOKUP_GRID; blue++, index += 3) {
				double point[4] = {red * step, green * step, blue * step, 0};
				if (!sample_point(profile, point))
					return FALSE;
				memcpy(samples + index, point, 3 * sizeof *point);
			}
	return TRUE;
}

/* ============================== Applying ======================================== */

/* JavaScript stores ToInt32 of the rounded value into a Uint8Array, keeping its low byte */
static guint8 store_rounded_byte(double value) {
	double rounded = trunc(.5 + value * 255);
	return isfinite(rounded) ? (guint8)(gint64)fmod(rounded, 4294967296.0) : 0;
}

/* Each channel byte always falls in the same grid cell and part of it, so those are worked out once */
static int cell_of_level[256];
static double part_of_level[256];

static void fill_cells(void) {
	static gsize ready;
	if (!g_once_init_enter(&ready))
		return;
	const double scale = COLOR_LOOKUP_GRID - 1.000001, unit = 1 / 255.0;
	for (int level = 0; level < 256; level++) {
		double coordinate = scale * (unit * level);
		cell_of_level[level] = (int)coordinate;
		part_of_level[level] = coordinate - cell_of_level[level];
	}
	g_once_init_leave(&ready, 1);
}

/* The bytes always fall inside the grid, so this is Z and C of ICC.U without reads past the table */
void lookup_color_pixel(const double *samples, guint8 *pixel) {
	fill_cells();
	const int grid = COLOR_LOOKUP_GRID, plane = grid * grid;
	int red = cell_of_level[pixel[0]], green = cell_of_level[pixel[1]], blue = cell_of_level[pixel[2]];
	double red_part = part_of_level[pixel[0]], green_part = part_of_level[pixel[1]],
		   blue_part = part_of_level[pixel[2]];
	const double *corners[4];
	int near = blue + grid * green + plane * red, far = blue + grid * (green + 1) + plane * red;
	corners[0] = samples + near * 3;
	corners[1] = samples + far * 3;
	corners[2] = samples + (near + plane) * 3;
	corners[3] = samples + (far + plane) * 3;
	double working[12];
	for (int corner = 0; corner < 4; corner++)
		for (int channel = 0; channel < 3; channel++)
			working[corner * 3 + channel] =
				(1 - blue_part) * corners[corner][channel] + blue_part * corners[corner][channel + 3];
	for (int channel = 0; channel < 3; channel++) {
		working[channel] = (1 - green_part) * working[channel] + green_part * working[3 + channel];
		working[6 + channel] = (1 - green_part) * working[6 + channel] + green_part * working[9 + channel];
	}
	for (int channel = 0; channel < 3; channel++)
		pixel[channel] = store_rounded_byte((1 - red_part) * working[channel] + red_part * working[6 + channel]);
}
