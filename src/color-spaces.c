#include <math.h>
#include <string.h>

#include "color-spaces.h"
#include "descriptor.h"

enum { COLOR_TABLE_SIZE = 8000 };

static const double XYZ_TO_RGB[16] = {3.1338561, -1.6168667, -.4906146, 0, -.9787684, 1.9161415, .033454, 0,
									  .0719453,	 -.2289914,	 1.4052427, 0, 0,		  0,		 0,		  1};
static const double D50_WHITE[3] = {.9642, 1, .8249};

static double linear_table[COLOR_TABLE_SIZE], lab_table[COLOR_TABLE_SIZE], rgb_to_xyz[16];

/* ============================== Tables ========================================== */

static void fill_color_tables(void) {
	static gsize ready;
	if (!g_once_init_enter(&ready))
		return;
	for (int index = 0; index < COLOR_TABLE_SIZE; index++) {
		double value = index * (1 / 4e3);
		linear_table[index] = linearize_srgb(value);
		lab_table[index] = value > .008856 ? pow(value, 1 / 3.0) : (903.3 * value + 16) * (1 / 116.0);
	}
	invert_matrix(XYZ_TO_RGB, rgb_to_xyz);
	g_once_init_leave(&ready, 1);
}

/* JavaScript reads undefined past the end of an array, which turns the arithmetic into NaN */
static double read_table(const double *table, double position) {
	double index = trunc(.5 + position);
	return index >= 0 && index < COLOR_TABLE_SIZE ? table[(int)index] : NAN;
}

/* ============================== Conversions ===================================== */

void convert_hsb(double hue, double saturation, double brightness, double channels[3]) {
	int sector = (int)floor(hue * 6);
	double fraction = hue * 6 - sector, low = brightness * (1 - saturation);
	double falling = brightness * (1 - fraction * saturation);
	double rising = brightness * (1 - (1 - fraction) * saturation);
	const double table[6][3] = {{brightness, rising, low},	{falling, brightness, low}, {low, brightness, rising},
								{low, falling, brightness}, {rising, low, brightness},	{brightness, low, falling}};
	memcpy(channels, table[((sector % 6) + 6) % 6], sizeof table[0]);
}

gboolean convert_swatch_color(guint space, const guint values[4], double rgb[3]) {
	switch (space) {
	case 0:
		for (int channel = 0; channel < 3; channel++)
			rgb[channel] = values[channel] * (255.0 / 65535);
		return TRUE;
	case 1:
		convert_hsb(values[0] * (1.0 / 65535), values[1] * (1.0 / 65535), values[2] * (1.0 / 65535), rgb);
		for (int channel = 0; channel < 3; channel++)
			rgb[channel] *= 255;
		return TRUE;
	case 7:
		/* Lab keeps its lightness unsigned and a and b signed, in hundredths */
		convert_lab_to_rgb(values[0] / 100.0, (gint16)values[1] / 100.0, (gint16)values[2] / 100.0, rgb);
		return TRUE;
	case 8:
		for (int channel = 0; channel < 3; channel++)
			rgb[channel] = 255 - values[channel] * (255.0 / 1e4);
		return TRUE;
	case 18:
		for (int channel = 0; channel < 3; channel++)
			rgb[channel] = 255.0 * values[0] / 16384;
		return TRUE;
	case 65535:
		rgb[0] = rgb[1] = rgb[2] = 255;
		return TRUE;
	default:
		return FALSE;
	}
}

double linearize_srgb(double value) {
	return value < .04045 ? value / 12.92 : pow((value + .055) / 1.055, 2.4);
}

double encode_srgb(double value) {
	return value < .0031308 ? 12.92 * value : 1.055 * pow(value, 1 / 2.4) - .055;
}

void convert_lab_to_rgb(double lightness, double a, double b, double rgb[3]) {
	const double kappa = 903.3, epsilon = .008856;
	double y = (lightness + 16) / 116, y_cube = y * y * y, z = y - b / 200, z_cube = z * z * z;
	double x = a / 500 + y, x_cube = x * x * x;
	double z_value = z_cube > epsilon ? z_cube : (116 * z - 16) / kappa;
	double y_value = y_cube > epsilon ? y_cube : (116 * y - 16) / kappa;
	double x_value = x_cube > epsilon ? x_cube : (116 * x - 16) / kappa;
	const double xyz[4] = {x_value * 96.42 / 100, y_value * 100 / 100, z_value * 82.49 / 100, 0};
	for (int channel = 0; channel < 3; channel++) {
		const double *row = XYZ_TO_RGB + channel * 4;
		double linear = row[0] * xyz[0] + row[1] * xyz[1] + row[2] * xyz[2] + row[3] * xyz[3];
		double value = encode_srgb(linear) * 255;
		rgb[channel] = isnan(value) ? value : MAX(0, MIN(255, value));
	}
}

void convert_xyz_to_lab(double x, double y, double z, double lab[3]) {
	fill_color_tables();
	double fx = read_table(lab_table, x * 4e3), fy = read_table(lab_table, y * 4e3),
		   fz = read_table(lab_table, z * 4e3);
	lab[0] = 116 * fy - 16;
	lab[1] = 500 * (fx - fy);
	lab[2] = 200 * (fy - fz);
}

void convert_rgb_to_lab(double red, double green, double blue, double lab[3]) {
	fill_color_tables();
	const double scale = 4e3 / 255;
	red = read_table(linear_table, red * scale);
	green = read_table(linear_table, green * scale);
	blue = read_table(linear_table, blue * scale);
	const double *m = rgb_to_xyz;
	double x = m[0] * red + m[1] * green + m[2] * blue, y = m[4] * red + m[5] * green + m[6] * blue;
	double z = m[8] * red + m[9] * green + m[10] * blue;
	convert_xyz_to_lab(x * (1 / D50_WHITE[0]), y * (1 / D50_WHITE[1]), z * (1 / D50_WHITE[2]), lab);
}

void invert_matrix(const double m[16], double inverse[16]) {
	inverse[0] = m[5] * m[10] * m[15] - m[5] * m[14] * m[11] - m[6] * m[9] * m[15] + m[6] * m[13] * m[11] +
				 m[7] * m[9] * m[14] - m[7] * m[13] * m[10];
	inverse[1] = -m[1] * m[10] * m[15] + m[1] * m[14] * m[11] + m[2] * m[9] * m[15] - m[2] * m[13] * m[11] -
				 m[3] * m[9] * m[14] + m[3] * m[13] * m[10];
	inverse[2] = m[1] * m[6] * m[15] - m[1] * m[14] * m[7] - m[2] * m[5] * m[15] + m[2] * m[13] * m[7] +
				 m[3] * m[5] * m[14] - m[3] * m[13] * m[6];
	inverse[3] = -m[1] * m[6] * m[11] + m[1] * m[10] * m[7] + m[2] * m[5] * m[11] - m[2] * m[9] * m[7] -
				 m[3] * m[5] * m[10] + m[3] * m[9] * m[6];
	inverse[4] = -m[4] * m[10] * m[15] + m[4] * m[14] * m[11] + m[6] * m[8] * m[15] - m[6] * m[12] * m[11] -
				 m[7] * m[8] * m[14] + m[7] * m[12] * m[10];
	inverse[5] = m[0] * m[10] * m[15] - m[0] * m[14] * m[11] - m[2] * m[8] * m[15] + m[2] * m[12] * m[11] +
				 m[3] * m[8] * m[14] - m[3] * m[12] * m[10];
	inverse[6] = -m[0] * m[6] * m[15] + m[0] * m[14] * m[7] + m[2] * m[4] * m[15] - m[2] * m[12] * m[7] -
				 m[3] * m[4] * m[14] + m[3] * m[12] * m[6];
	inverse[7] = m[0] * m[6] * m[11] - m[0] * m[10] * m[7] - m[2] * m[4] * m[11] + m[2] * m[8] * m[7] +
				 m[3] * m[4] * m[10] - m[3] * m[8] * m[6];
	inverse[8] = m[4] * m[9] * m[15] - m[4] * m[13] * m[11] - m[5] * m[8] * m[15] + m[5] * m[12] * m[11] +
				 m[7] * m[8] * m[13] - m[7] * m[12] * m[9];
	inverse[9] = -m[0] * m[9] * m[15] + m[0] * m[13] * m[11] + m[1] * m[8] * m[15] - m[1] * m[12] * m[11] -
				 m[3] * m[8] * m[13] + m[3] * m[12] * m[9];
	inverse[10] = m[0] * m[5] * m[15] - m[0] * m[13] * m[7] - m[1] * m[4] * m[15] + m[1] * m[12] * m[7] +
				  m[3] * m[4] * m[13] - m[3] * m[12] * m[5];
	inverse[11] = -m[0] * m[5] * m[11] + m[0] * m[9] * m[7] + m[1] * m[4] * m[11] - m[1] * m[8] * m[7] -
				  m[3] * m[4] * m[9] + m[3] * m[8] * m[5];
	inverse[12] = -m[4] * m[9] * m[14] + m[4] * m[13] * m[10] + m[5] * m[8] * m[14] - m[5] * m[12] * m[10] -
				  m[6] * m[8] * m[13] + m[6] * m[12] * m[9];
	inverse[13] = m[0] * m[9] * m[14] - m[0] * m[13] * m[10] - m[1] * m[8] * m[14] + m[1] * m[12] * m[10] +
				  m[2] * m[8] * m[13] - m[2] * m[12] * m[9];
	inverse[14] = -m[0] * m[5] * m[14] + m[0] * m[13] * m[6] + m[1] * m[4] * m[14] - m[1] * m[12] * m[6] -
				  m[2] * m[4] * m[13] + m[2] * m[12] * m[5];
	inverse[15] = m[0] * m[5] * m[10] - m[0] * m[9] * m[6] - m[1] * m[4] * m[10] + m[1] * m[8] * m[6] +
				  m[2] * m[4] * m[9] - m[2] * m[8] * m[5];
	double determinant = m[0] * inverse[0] + m[1] * inverse[4] + m[2] * inverse[8] + m[3] * inverse[12];
	for (int index = 0; index < 16; index++)
		inverse[index] /= determinant;
}

/* ============================== Hue models ====================================== */

double get_rgb_hue(double red, double green, double blue) {
	double high = MAX(red, MAX(green, blue)), low = MIN(red, MIN(green, blue)), hue = 0;
	if (high == low)
		return 0;
	double range = high - low;
	if (high == red)
		hue = (green - blue) / range + (green < blue ? 6 : 0);
	else if (high == green)
		hue = (blue - red) / range + 2;
	else
		hue = (red - green) / range + 4;
	return hue * (1 / 6.0);
}

static double get_hsl_channel(double low, double high, double hue) {
	if (hue < 0)
		hue += 1;
	if (hue > 1)
		hue -= 1;
	if (hue < 1 / 6.0)
		return low + (high - low) * 6 * hue;
	if (hue < 1 / 2.0)
		return high;
	if (hue < 2 / 3.0)
		return low + (high - low) * (2 / 3.0 - hue) * 6;
	return low;
}

void convert_hsl_to_rgb(double hue, double saturation, double lightness, double rgb[3]) {
	if (saturation == 0) {
		rgb[0] = rgb[1] = rgb[2] = lightness;
		return;
	}
	double high = lightness < .5 ? lightness * (1 + saturation) : lightness + saturation - lightness * saturation;
	double low = 2 * lightness - high;
	rgb[0] = get_hsl_channel(low, high, hue + 1 / 3.0);
	rgb[1] = get_hsl_channel(low, high, hue);
	rgb[2] = get_hsl_channel(low, high, hue - 1 / 3.0);
}

void convert_rgb_to_hsl(double red, double green, double blue, double hsl[3]) {
	double high = MAX(red, MAX(green, blue)), low = MIN(red, MIN(green, blue));
	hsl[0] = get_rgb_hue(red, green, blue);
	hsl[1] = 0;
	hsl[2] = (high + low) * .5;
	if (high == low)
		return;
	double range = high - low;
	hsl[1] = hsl[2] > .5 ? range / (2 - (high + low)) : range / (high + low);
}

void convert_rgb_to_hsv(double red, double green, double blue, double hsv[3]) {
	double high = MAX(red, MAX(green, blue)), low = MIN(red, MIN(green, blue));
	hsv[0] = get_rgb_hue(red, green, blue);
	hsv[1] = high == 0 ? 0 : (high - low) / high;
	hsv[2] = high;
}

double weigh_hue_range(double hue, double center) {
	double distance = hue - center, size = fabs(distance);
	if (fabs(distance - 1) < size)
		distance -= 1;
	else if (fabs(distance + 1) < size)
		distance += 1;
	distance *= 6;
	return MAX(0, MIN(1, distance < 0 ? 1 + distance : 1 - distance));
}

/* ============================== Descriptors ===================================== */

static double read_color_number(const Record *color, const char *key) {
	Record *value = get_record(get_record(color, key), "v");
	if (value && value->type == RECORD_OBJECT)
		return get_number(value, "val");
	return value && value->type == RECORD_NUMBER ? value->value.number : NAN;
}

gboolean read_descriptor_color(const Record *color, double rgb[3]) {
	const char *kind = get_string(color, "classID");
	if (!g_strcmp0(kind, "RGBC")) {
		gboolean whole = has_record(color, "Rd");
		const char *keys[2][3] = {{"redFloat", "greenFloat", "blueFloat"}, {"Rd", "Grn", "Bl"}};
		for (int channel = 0; channel < 3; channel++)
			rgb[channel] = MAX(read_color_number(color, keys[whole][channel]) * (whole ? 1 : 255), 0);
	} else if (!g_strcmp0(kind, "HSBC")) {
		convert_hsb(read_color_number(color, "H") / 360, read_color_number(color, "Strt") / 100,
					read_color_number(color, "Brgh") / 100, rgb);
		for (int channel = 0; channel < 3; channel++)
			rgb[channel] *= 255;
	} else if (!g_strcmp0(kind, "Grsc"))
		rgb[0] = rgb[1] = rgb[2] = 255 - read_color_number(color, "Gry");
	else if (!g_strcmp0(kind, "LbCl"))
		convert_lab_to_rgb(read_color_number(color, "Lmnc"), read_color_number(color, "A"),
						   read_color_number(color, "B"), rgb);
	else if (!g_strcmp0(kind, "BkCl"))
		rgb[0] = rgb[1] = rgb[2] = 0;
	else
		return FALSE;
	if (isnan(rgb[0]))
		rgb[0] = rgb[1] = rgb[2] = 0;
	return TRUE;
}

static Record *create_color_object(const char *kind, const char *const keys[3], const double values[3]) {
	Record *color = create_object(RECORD_PLAIN);
	set_string(color, "classID", kind);
	for (int channel = 0; channel < 3; channel++)
		set_record(color, keys[channel], create_typed_value("doub", create_number(values[channel])));
	return color;
}

Record *create_rgb_descriptor(double red, double green, double blue) {
	return create_color_object("RGBC", (const char *const[]){"Rd", "Grn", "Bl"}, (const double[]){red, green, blue});
}

Record *create_lab_descriptor(double lightness, double a, double b) {
	return create_color_object("LbCl", (const char *const[]){"Lmnc", "A", "B"}, (const double[]){lightness, a, b});
}
