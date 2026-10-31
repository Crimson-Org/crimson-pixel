#pragma once

#include <glib.h>

/* One named library color with 0 to 255 channel values, f and c */
typedef struct {
	char *name;
	double red, green, blue;
} Swatch;

GArray *create_swatch_list(void);
GArray *read_swatches(const guint8 *bytes, gsize length, GError **error);
GBytes *write_swatches(const GArray *swatches);
guint32 get_swatch_color(const Swatch *swatch);
void add_swatch(GArray *swatches, const char *name, guint32 color);
char *get_swatch_library_path(void);
