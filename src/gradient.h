#pragma once

#include "paint.h"

#define GRADIENT_LOOKUP_SIZE 1024

typedef enum { GRADIENT_LINEAR, GRADIENT_RADIAL, GRADIENT_ANGLE, GRADIENT_REFLECTED, GRADIENT_DIAMOND } GradientType;

typedef enum {
	GRADIENT_CLASSIC,
	GRADIENT_LINEAR_LIGHT,
	GRADIENT_PERCEPTUAL,
	GRADIENT_SMOOTH,
	GRADIENT_STRIPES
} GradientInterpolation;

typedef enum { GRADIENT_USER_COLOR, GRADIENT_FOREGROUND, GRADIENT_BACKGROUND } GradientColorKind;

typedef struct {
	int location, midpoint;
	GradientColorKind kind;
	double red, green, blue;
} GradientColorStop;

typedef struct {
	int location, midpoint;
	double opacity;
} GradientOpacityStop;

typedef struct {
	int smoothness;
	const GradientColorStop *colors;
	int color_count;
	const GradientOpacityStop *opacities;
	int opacity_count;
} Gradient;

typedef struct {
	GradientType type;
	GradientInterpolation interpolation;
	double opacity;
	gboolean reverse, dither;
} GradientOptions;

#define GRADIENT_TWO_COLOR_NAME "Default/Two Color"

const Gradient *get_default_gradient(void);
const Gradient *get_two_color_gradient(void);

Gradient *read_gradient_record(const Record *gradient);
void free_gradient(Gradient *gradient);
Record *create_gradient_record(const Gradient *gradient, const char *name);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Gradient, free_gradient)
void build_gradient_lookup(const Gradient *gradient, gboolean reverse, GradientInterpolation interpolation,
						   guint32 foreground, guint32 background, float *lookup);
void constrain_gradient_point(double start_x, double start_y, double *x, double *y);
void fill_gradient_pixels(const Gradient *gradient, const GradientOptions *options, guint32 foreground,
						  guint32 background, double start_x, double start_y, double end_x, double end_y,
						  RasterExtent area, guint32 dither_offset, guint8 *pixels);
gboolean draw_gradient(PaintTool *tool, Document *document, const GradientOptions *options, guint32 foreground,
					   guint32 background, double start_x, double start_y, double end_x, double end_y, GError **error);
