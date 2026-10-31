#pragma once

#include "document/record.h"

/* Colour matrices are four by four: red, green, blue and an offset */
#define MATRIX_SIDE 4
#define MATRIX_VALUES (MATRIX_SIDE * MATRIX_SIDE)
/* Hue models split the colour wheel into six sectors */
#define HUE_SECTORS 6

/* The colour spaces of swatch and gradient colours, which keep four sixteen bit values: RGB, HSB, CMYK, Lab, gray in
 * ten thousandths from white, gray as a share of 16384, and an empty colour that reads as white */
enum {
	SWATCH_RGB = 0,
	SWATCH_HSB = 1,
	SWATCH_CMYK = 2,
	SWATCH_LAB = 7,
	SWATCH_GRAY = 8,
	SWATCH_GRAY_SHARE = 18,
	SWATCH_EMPTY = 65535,
};
#define SWATCH_VALUES 4

/* How much red, green and blue count toward the brightness people see */
#define LUMA_RED .3
#define LUMA_GREEN .59
#define LUMA_BLUE .11

static inline double get_luma(double red, double green, double blue) {
	return LUMA_RED * red + LUMA_GREEN * green + LUMA_BLUE * blue;
}

void convert_hsb(double hue, double saturation, double brightness, double channels[RGB_CHANNELS]);

gboolean convert_swatch_color(guint space, const guint values[SWATCH_VALUES], double rgb[RGB_CHANNELS]);

double linearize_srgb(double value);
double encode_srgb(double value);

void convert_lab_to_rgb(double lightness, double a, double b, double rgb[RGB_CHANNELS]);

void convert_rgb_to_lab(double red, double green, double blue, double lab[RGB_CHANNELS]);

void convert_xyz_to_lab(double x, double y, double z, double lab[RGB_CHANNELS]);

double get_rgb_hue(double red, double green, double blue);
void convert_rgb_to_hsl(double red, double green, double blue, double hsl[RGB_CHANNELS]);
void convert_hsl_to_rgb(double hue, double saturation, double lightness, double rgb[RGB_CHANNELS]);
void convert_rgb_to_hsv(double red, double green, double blue, double hsv[RGB_CHANNELS]);

double weigh_hue_range(double hue, double center);

void invert_matrix(const double matrix[MATRIX_VALUES], double inverse[MATRIX_VALUES]);

gboolean read_descriptor_color(const Record *color, double rgb[RGB_CHANNELS]);
Record *create_rgb_descriptor(double red, double green, double blue);
/* The descriptor keys of Lab lightness, a and b */
extern const char *const LAB_KEYS[RGB_CHANNELS];
Record *create_lab_descriptor(double lightness, double a, double b);

/* The D50 white point, and the matrix from XYZ to linear sRGB with a fourth row and column for an offset */
extern const double D50_WHITE[RGB_CHANNELS];
extern const double XYZ_TO_RGB[MATRIX_VALUES];
/* Linear light and the Lab curve are tables over 0 to 2 in steps of one in COLOR_TABLE_SCALE, read at the nearest
 * step of a position, with NaN past the end */
#define COLOR_TABLE_SCALE 4e3
double read_linear_table(double position);
double read_lab_table(double position);
/* X, Y and Z of a Lab colour as shares of the white point */
void convert_lab_to_white_shares(double lightness, double a, double b, double shares[RGB_CHANNELS]);
