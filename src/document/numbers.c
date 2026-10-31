#include <math.h>

#include "numbers.h"

#define UINT32_RANGE ((double)G_MAXUINT32 + 1)

double round_half_up(double value) {
	double whole = floor(value);
	return value - whole >= .5 ? whole + 1 : whole;
}

double nan_min(double first, double second) {
	return isnan(first) || isnan(second) ? NAN : MIN(first, second);
}

double nan_max(double first, double second) {
	return isnan(first) || isnan(second) ? NAN : MAX(first, second);
}

guint32 wrap_uint32(double value) {
	if (!isfinite(value))
		return 0;
	double wrapped = fmod(trunc(value), UINT32_RANGE);
	return (guint32)(wrapped < 0 ? wrapped + UINT32_RANGE : wrapped);
}

gint32 wrap_int32(double value) {
	return (gint32)wrap_uint32(value);
}

/* An integer hash in the style of Thomas Wang's, with sums taken in doubles so nothing overflows */
#define HASH_MIX 61
#define HASH_MULTIPLIER 668265261.0

guint32 hash_whole_number(double value) {
	gint32 mixed = wrap_int32(value);
	mixed = mixed ^ HASH_MIX ^ (gint32)(wrap_uint32(mixed) >> 16);
	double sum = (double)mixed + (double)(gint32)((guint32)mixed << 3);
	mixed = wrap_int32(sum) ^ (gint32)(wrap_uint32(sum) >> 4);
	guint32 product = wrap_uint32((double)mixed * HASH_MULTIPLIER);
	return product ^ (product >> 15);
}

guint8 wrap_byte(double value) {
	if (value >= 0 && value <= G_MAXUINT8)
		return (guint8)value;
	return (guint8)wrap_uint32(value);
}
