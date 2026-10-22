#pragma once

#include "lut-files.h"

/* Color Lookup samples its profile at this many points per axis, like ICC.U.sampleLUT with 17 */
#define COLOR_LOOKUP_GRID 17
#define COLOR_LOOKUP_SAMPLES (COLOR_LOOKUP_GRID * COLOR_LOOKUP_GRID * COLOR_LOOKUP_GRID * 3)

gboolean sample_color_lookup(const guint8 *profile, gsize length, double *samples);

void lookup_color_pixel(const double *samples, guint8 *pixel);
