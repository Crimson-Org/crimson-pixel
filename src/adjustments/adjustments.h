#pragma once

#include "curve-spline.h"
#include "document/document.h"
#include "color/gradient.h"

/* Levels and curves work on the composite, then on red, green and blue */
#define TONE_CHANNELS (1 + RGB_CHANNELS)
#define CURVE_LEVELS (G_MAXUINT8 + 1)

/* A levels channel keeps its input and output range and its gamma in hundredths */
enum { LEVELS_INPUT_BLACK, LEVELS_INPUT_WHITE, LEVELS_OUTPUT_BLACK, LEVELS_OUTPUT_WHITE, LEVELS_GAMMA, LEVELS_VALUES };
#define LEVELS_GAMMA_SCALE 100

/* Hue and saturation has a master range and six colour ranges, each with four limits and the three shifts */
#define HUE_RANGES 7
enum { HUE_LIMITS = 4, HUE_SHIFT = HUE_LIMITS, HUE_SATURATION, HUE_LIGHTNESS, HUE_RANGE_VALUES };

/* Color balance shifts red, green and blue in the shadows, midtones and highlights */
#define BALANCE_TONES_COUNT 3

/* Photo filter strength in percent */
#define DEFAULT_FILTER_DENSITY 80

/* Selective color moves cyan, magenta, yellow and black inks in nine colour ranges */
#define SELECTIVE_RANGES_COUNT 9
#define SELECTIVE_INKS_COUNT 4

/* The channel mixer has a row per output with red, green and blue, an unused place, and a constant */
#define MIXER_ROWS 4
#define MIXER_INPUTS_COUNT 5
#define MIXER_VALUES (MIXER_ROWS * MIXER_INPUTS_COUNT)

/* Black and white weighs reds, yellows, greens, cyans, blues and magentas */
#define GRAY_WEIGHTS_COUNT 6

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

void get_levels_channel(const Record *descriptor, int channel, double values[LEVELS_VALUES]);
void set_levels_channel(Record *descriptor, int channel, const double values[LEVELS_VALUES]);

void get_hue_range(const Record *descriptor, int range, double values[HUE_RANGE_VALUES]);
void set_hue_range(Record *descriptor, int range, const double values[HUE_RANGE_VALUES]);

/* The cyan to red, magenta to green and yellow to blue shifts of the shadows, midtones or highlights */
void get_color_balance(const Record *descriptor, int tone, double values[RGB_CHANNELS]);
void set_color_balance(Record *descriptor, int tone, const double values[RGB_CHANNELS]);

extern const char *const GRAY_WEIGHTS[GRAY_WEIGHTS_COUNT];

GArray *get_curve_points(const Record *descriptor, int channel, double levels[CURVE_LEVELS]);
void set_curve_points(Record *descriptor, int channel, const CurvePoint *points, guint count);
void set_curve_levels(Record *descriptor, int channel, const double levels[CURVE_LEVELS]);

void get_selective_color(const Record *descriptor, int range, double values[SELECTIVE_INKS_COUNT]);
void set_selective_color(Record *descriptor, int range, const double values[SELECTIVE_INKS_COUNT]);
gboolean is_absolute_selective_color(const Record *descriptor);
/* The profile of a loaded lookup file, or NULL before one is loaded */
GBytes *get_color_lookup_profile(const Record *descriptor);
const char *get_color_lookup_name(const Record *descriptor);
void set_color_lookup_file(Record *descriptor, GBytes *profile, const char *name);
GradientInterpolation get_gradient_map_method(const Record *descriptor);
void set_gradient_map_method(Record *descriptor, GradientInterpolation method);
void set_absolute_selective_color(Record *descriptor, gboolean absolute);

gboolean get_channel_mixer(const Record *descriptor, double values[MIXER_VALUES]);
void set_channel_mixer(Record *descriptor, gboolean monochrome, const double values[MIXER_VALUES]);

gboolean add_adjustment_layer(Document *document, const char *key, gboolean extended_history, GError **error);

gboolean edit_adjustment_layer(Document *document, int layer_index, const Record *descriptor, gboolean extended_history,
							   GError **error);
