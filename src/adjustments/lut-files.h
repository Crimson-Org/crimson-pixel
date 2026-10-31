#pragma once

#include <glib.h>

/* ICC profiles start their tag table after a header of this size */
enum { ICC_HEADER_SIZE = 128 };

/* Turns a .cube, .3dl or .look table into an ICC device link profile, and passes an ICC profile through */
GBytes *convert_lut_file(const guint8 *data, gsize length);
/* The same, with the profile named as given */
GBytes *convert_named_lut_file(const guint8 *data, gsize length, const char *name);
