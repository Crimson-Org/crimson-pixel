#pragma once

#include <glib.h>

/* One Curves point, where a corner point is not smoothed with its neighbours */
typedef struct {
	double input, output;
	gboolean smooth;
} CurvePoint;

void sample_spline(const CurvePoint *points, int count, float *curve, int length);
