#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	BLEND_NORMAL,
	BLEND_DISSOLVE,
	BLEND_DARKEN,
	BLEND_MULTIPLY,
	BLEND_COLOR_BURN,
	BLEND_LINEAR_BURN,
	BLEND_DARKER_COLOR,
	BLEND_LIGHTEN,
	BLEND_SCREEN,
	BLEND_COLOR_DODGE,
	BLEND_LINEAR_DODGE,
	BLEND_LIGHTER_COLOR,
	BLEND_OVERLAY,
	BLEND_SOFT_LIGHT,
	BLEND_HARD_LIGHT,
	BLEND_VIVID_LIGHT,
	BLEND_LINEAR_LIGHT,
	BLEND_PIN_LIGHT,
	BLEND_HARD_MIX,
	BLEND_DIFFERENCE,
	BLEND_EXCLUSION,
	BLEND_SUBTRACT,
	BLEND_DIVIDE,
	BLEND_HUE,
	BLEND_SATURATION,
	BLEND_COLOR,
	BLEND_LUMINOSITY,
	BLEND_MODE_COUNT
} BlendMode;

typedef struct {
	double fill;
	const double *ranges;
	bool preserve_alpha, style;
} BlendOptions;

typedef struct BlendKernel BlendKernel;
typedef uint32_t (*BlendPixelFunction)(const BlendKernel *kernel, uint32_t source, uint32_t destination,
									   uint32_t index);

struct BlendKernel {
	BlendPixelFunction blend;
	double (*channel)(double source, double destination, double fill);
	void (*color)(const double *source, const double *destination, double *result);
	double opacity;
	BlendOptions options;
};

extern const char *const blend_mode_keys[BLEND_MODE_COUNT];

/* Returns the ea.XV position of a four character key, or -1 */
int find_blend_mode(const char *key);

bool prepare_blend_ranges(const double *layer_ranges, double *prepared);

BlendKernel prepare_blend_kernel(BlendMode mode, double opacity, BlendOptions options);

int32_t hash_dissolve_position(double position);

/* Both pixels have straight ARGB channels and opacity is between zero and one */
uint32_t compose_normal_pixel(uint32_t source, uint32_t destination, double opacity);
