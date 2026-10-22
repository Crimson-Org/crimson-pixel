#pragma once

#include <glib.h>

/* ICC profiles start their tag table after a header of this size */
enum { ICC_HEADER_SIZE = 128 };

GBytes *convert_lut_file(const guint8 *data, gsize length);
