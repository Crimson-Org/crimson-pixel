#pragma once

#include <glib.h>

/* Number conversions every part shares, so stored values wrap and round the same way everywhere */

/* Byte channels run from 0 to 255, unit values from 0 to 1, percentages to 100 and turns to 360 degrees */
#define BYTE_UNIT (1 / 255.0)
#define PERCENT 100.0
#define DEGREES_PER_TURN 360.0

static inline double byte_to_unit(double value) {
	return value / 255.0;
}

static inline double unit_to_byte(double value) {
	return value * 255;
}

static inline double from_percent(double value) {
	return value / PERCENT;
}

/* Rounds halves upward, so -2.5 becomes -2, unlike C's round */
double round_half_up(double value);
/* The smaller or larger number, or NaN when either one is NaN, unlike fmin and fmax */
double nan_min(double first, double second);
double nan_max(double first, double second);
/* The whole part of a number wrapped into 32 bits, where a NaN or an infinity becomes zero */
guint32 wrap_uint32(double value);
gint32 wrap_int32(double value);
/* The low byte of the wrapped whole part */
guint8 wrap_byte(double value);
