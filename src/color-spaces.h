#pragma once

#include "record.h"

void convert_hsb(double hue, double saturation, double brightness, double channels[3]);

gboolean convert_swatch_color(guint space, const guint values[4], double rgb[3]);

double linearize_srgb(double value);
double encode_srgb(double value);

void convert_lab_to_rgb(double lightness, double a, double b, double rgb[3]);

void convert_rgb_to_lab(double red, double green, double blue, double lab[3]);

void convert_xyz_to_lab(double x, double y, double z, double lab[3]);

double get_rgb_hue(double red, double green, double blue);
void convert_rgb_to_hsl(double red, double green, double blue, double hsl[3]);
void convert_hsl_to_rgb(double hue, double saturation, double lightness, double rgb[3]);
void convert_rgb_to_hsv(double red, double green, double blue, double hsv[3]);

double weigh_hue_range(double hue, double center);

void invert_matrix(const double matrix[16], double inverse[16]);

gboolean read_descriptor_color(const Record *color, double rgb[3]);
Record *create_rgb_descriptor(double red, double green, double blue);
Record *create_lab_descriptor(double lightness, double a, double b);
