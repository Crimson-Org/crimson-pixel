#pragma once

#include "curve-spline.h"
#include "document.h"
#include "gradient.h"

const char *find_adjustment_key(const Record *descriptors);
gboolean is_adjustment_layer(const Record *layer);
gboolean is_ported_adjustment(const char *key);
const char *get_adjustment_name(const char *key);
/* The ported adjustments in menu order, or NULL past the last */
const char *get_ported_adjustment(guint index, gboolean *starts_group);

Record *create_adjustment_descriptor(const char *key);
double get_descriptor_number(const Record *descriptor, const char *key, double fallback);
void set_descriptor_number(Record *descriptor, const char *key, const char *type, double value);
gboolean get_descriptor_boolean(const Record *descriptor, const char *key, gboolean fallback);
void set_descriptor_boolean(Record *descriptor, const char *key, gboolean value);

void get_levels_channel(const Record *descriptor, int channel, double values[5]);
void set_levels_channel(Record *descriptor, int channel, const double values[5]);

void get_hue_range(const Record *descriptor, int range, double values[7]);
void set_hue_range(Record *descriptor, int range, const double values[7]);

/* The cyan to red, magenta to green and yellow to blue shifts of the shadows, midtones or highlights */
void get_color_balance(const Record *descriptor, int tone, double values[3]);
void set_color_balance(Record *descriptor, int tone, const double values[3]);

extern const char *const GRAY_WEIGHTS[6];

GArray *get_curve_points(const Record *descriptor, int channel, double levels[256]);
void set_curve_points(Record *descriptor, int channel, const CurvePoint *points, guint count);
void set_curve_levels(Record *descriptor, int channel, const double levels[256]);

void get_selective_color(const Record *descriptor, int range, double values[4]);
void set_selective_color(Record *descriptor, int range, const double values[4]);
gboolean is_absolute_selective_color(const Record *descriptor);
/* The profile of a loaded lookup file, or NULL before one is loaded */
GBytes *get_color_lookup_profile(const Record *descriptor);
const char *get_color_lookup_name(const Record *descriptor);
void set_color_lookup_file(Record *descriptor, GBytes *profile, const char *name);
GradientInterpolation get_gradient_map_method(const Record *descriptor);
void set_gradient_map_method(Record *descriptor, GradientInterpolation method);
void set_absolute_selective_color(Record *descriptor, gboolean absolute);

gboolean get_channel_mixer(const Record *descriptor, double values[20]);
void set_channel_mixer(Record *descriptor, gboolean monochrome, const double values[20]);

gboolean add_adjustment_layer(Document *document, const char *key, gboolean extended_history, GError **error);

gboolean edit_adjustment_layer(Document *document, int layer_index, const Record *descriptor, gboolean extended_history,
							   GError **error);
