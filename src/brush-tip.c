#include <math.h>
#include <string.h>

#include "stroke.h"

#define PROFILE_SAMPLES 8000

typedef struct {
	double hardness;
	double values[PROFILE_SAMPLES];
	int last_solid, first_empty;
} TipProfile;

static GPtrArray *tip_profiles;
G_LOCK_DEFINE_STATIC(tip_profiles);

/* ============================== Affine transforms ================================ */

Affine create_identity_affine(void) {
	return (Affine){1, 0, 0, 1, 0, 0};
}

void concatenate_affine(Affine *transform, Affine after) {
	Affine first = *transform;
	*transform =
		(Affine){first.a * after.a + first.b * after.c,			  first.a * after.b + first.b * after.d,
				 first.c * after.a + first.d * after.c,			  first.c * after.b + first.d * after.d,
				 first.e * after.a + first.f * after.c + after.e, first.e * after.b + first.f * after.d + after.f};
}

void translate_affine(Affine *transform, double x, double y) {
	transform->e += x;
	transform->f += y;
}

void scale_affine(Affine *transform, double x, double y) {
	concatenate_affine(transform, (Affine){x, 0, 0, y, 0, 0});
}

void rotate_affine(Affine *transform, double angle) {
	concatenate_affine(transform, (Affine){cos(angle), -sin(angle), sin(angle), cos(angle), 0, 0});
}

static Affine invert_affine(Affine transform) {
	double determinant = transform.a * transform.d - transform.b * transform.c;
	return (Affine){transform.d / determinant,
					-transform.b / determinant,
					-transform.c / determinant,
					transform.a / determinant,
					(transform.c * transform.f - transform.d * transform.e) / determinant,
					(transform.b * transform.e - transform.a * transform.f) / determinant};
}

static double affine_scale(Affine transform) {
	return sqrt(fabs(transform.a * transform.d - transform.b * transform.c));
}

static void transform_affine_point(Affine transform, double x, double y, double *result_x, double *result_y) {
	*result_x = x * transform.a + y * transform.c + transform.e;
	*result_y = x * transform.b + y * transform.d + transform.f;
}

gboolean is_translation_affine(Affine transform) {
	return transform.a == 1 && transform.b == 0 && transform.c == 0 && transform.d == 1;
}

/* ============================== Computed tips ==================================== */

static double profile_curve(double distance) {
	double scaled = distance * .85;
	return exp(-(scaled * scaled) * (1 / (2 * .4 * .4))) * (1 / (.4 * sqrt(2 * G_PI)));
}

static double profile_value(double distance, double solid) {
	return distance < solid ? 1 : profile_curve((distance - solid) / pow(1.001 - solid, .86));
}

static const TipProfile *get_tip_profile(double hardness) {
	G_LOCK(tip_profiles);
	if (!tip_profiles)
		tip_profiles = g_ptr_array_new_with_free_func(g_free);
	for (guint index = 0; index < tip_profiles->len; index++) {
		TipProfile *profile = g_ptr_array_index(tip_profiles, index);
		if (profile->hardness == hardness) {
			G_UNLOCK(tip_profiles);
			return profile;
		}
	}
	TipProfile *profile = g_new0(TipProfile, 1);
	profile->hardness = hardness;
	profile->first_empty = PROFILE_SAMPLES;
	double solid = pow(hardness, .55);
	for (int index = 0; index < PROFILE_SAMPLES; index++) {
		double value = profile_value(index * (1.55 / 4000), solid);
		if (value >= 1)
			profile->last_solid = index;
		else if (value <= 0 && profile->first_empty == PROFILE_SAMPLES)
			profile->first_empty = index;
		profile->values[index] = value;
	}
	g_ptr_array_add(tip_profiles, profile);
	G_UNLOCK(tip_profiles);
	return profile;
}

static void set_symmetric_values(guint8 *tip, int size, int row, int column, guint8 value) {
	int mirrored_column = size - 1 - column, top = row * size, bottom = (size - 1 - row) * size;
	tip[top + column] = tip[top + mirrored_column] = tip[bottom + column] = tip[bottom + mirrored_column] = value;
}

BrushLevel create_computed_tip(double diameter, double hardness, double scale) {
	if (diameter < 4)
		hardness = 1;
	double margin = 1 + .55 * (1 - hardness);
	int size = (int)round_like_source(scale * diameter * margin) + 2;
	BrushLevel level = {create_channels((gsize)size * size, 8), {0, 0, size, size}};
	guint8 *tip = level.values->value.buffer.data;
	double center = size / 2.0, step = margin / (center - 1);
	if (diameter == 2)
		step *= 2;
	if (diameter == 3)
		step *= 1.6;
	const TipProfile *profile = get_tip_profile(hardness);
	int border = (int)round_like_source(PROFILE_SAMPLES / center);
	int solid = profile->last_solid - border, empty = profile->first_empty + border;
	int half = MIN(size, (size + 1) >> 1);
	double blur = step * .4 * (4000 / 1.55);
	g_autofree guint8 *falloff = g_malloc0(PROFILE_SAMPLES);
	for (int index = 0; index < PROFILE_SAMPLES; index++) {
		int far_before = MAX(0, index - blur - blur), before = MAX(0, index - blur);
		int after = MIN(PROFILE_SAMPLES - 1, index + blur), far_after = MIN(PROFILE_SAMPLES - 1, index + blur + blur);
		const double *values = profile->values;
		int value =
			(int)(.5 + 255 * (1.0 / 5) *
						   (values[index] + values[far_before] + values[before] + values[after] + values[far_after]));
		falloff[index] = value;
		if (value == 0)
			break;
	}
	for (int row = 0; row < half; row++) {
		double vertical = step * (row - center + .5), vertical_squared = vertical * vertical;
		for (int column = 0; column < half; column++) {
			double horizontal = step * (column - center + .5);
			int distance = (int)(sqrt(horizontal * horizontal + vertical_squared) * (4000 / 1.55));
			if (empty < distance)
				continue;
			if (distance < solid) {
				for (; column < half; column++)
					set_symmetric_values(tip, size, row, column, 255);
				break;
			}
			set_symmetric_values(tip, size, row, column, distance < PROFILE_SAMPLES ? falloff[distance] : 0);
		}
	}
	return level;
}

/* ============================== Mip levels and resampling ======================== */

static BrushLevel halve_level(BrushLevel level) {
	int width = level.extent.width >> 1, height = level.extent.height >> 1;
	BrushLevel half = {create_channels((gsize)width * height, 8), {level.extent.x, level.extent.y, width, height}};
	const guint8 *source = level.values->value.buffer.data;
	guint8 *target = half.values->value.buffer.data;
	int stride = level.extent.width;
	for (int row = 0; row < height; row++)
		for (int column = 0; column < width; column++) {
			int offset = (row << 1) * stride + (column << 1);
			target[row * width + column] =
				(2 + source[offset] + source[offset + 1] + source[offset + stride] + source[offset + stride + 1]) >> 2;
		}
	return half;
}

void build_brush_levels(GArray *levels) {
	BrushLevel last = g_array_index(levels, BrushLevel, levels->len - 1);
	while (last.extent.width >= 2 && last.extent.height >= 2) {
		last = halve_level(last);
		g_array_append_val(levels, last);
	}
}

void free_brush_levels(GArray *levels) {
	for (guint index = 0; levels && index < levels->len; index++)
		free_record(g_array_index(levels, BrushLevel, index).values);
	if (levels)
		g_array_unref(levels);
}

/* Typed array reads beyond the buffer give undefined, which turns the whole sample into NaN */
static double read_tip_value(const Record *values, int index) {
	if (index < 0 || (gsize)index >= values->value.buffer.length)
		return NAN;
	return ((guint8 *)values->value.buffer.data)[index];
}

static double sample_bilinear(double x, double y, const Record *values, int width) {
	x -= .499999;
	y -= .499999;
	int left = floor(x), top = floor(y);
	double horizontal = x - left, vertical = y - top;
	int offset = top * width + left, below = offset + width;
	return (1 - vertical) * (1 - horizontal) * read_tip_value(values, offset) +
		   (1 - vertical) * horizontal * read_tip_value(values, offset + 1) +
		   vertical * (1 - horizontal) * read_tip_value(values, below) +
		   vertical * horizontal * read_tip_value(values, below + 1);
}

static RasterExtent bound_transformed_extent(RasterExtent extent, Affine transform) {
	double corners[8] = {extent.x,
						 extent.y,
						 extent.x + extent.width,
						 extent.y,
						 extent.x + extent.width,
						 extent.y + extent.height,
						 extent.x,
						 extent.y + extent.height};
	double left = 99999999999, top = 99999999999, right = -99999999999, bottom = -99999999999;
	for (int index = 0; index < 8; index += 2) {
		double x, y;
		transform_affine_point(transform, corners[index], corners[index + 1], &x, &y);
		left = MIN(left, x);
		top = MIN(top, y);
		right = MAX(right, x);
		bottom = MAX(bottom, y);
	}
	int first_x = floor(left), first_y = floor(top);
	return (RasterExtent){first_x, first_y, (int)ceil(right) - first_x, (int)ceil(bottom) - first_y};
}

BrushLevel resample_brush(GArray *levels, Affine transform) {
	build_brush_levels(levels);
	guint level_index = 0;
	while (level_index + 3 < levels->len * 2 &&
		   g_array_index(levels, BrushLevel, (level_index + 2) / 2).extent.width *
				   g_array_index(levels, BrushLevel, (level_index + 2) / 2).extent.height >=
			   4 &&
		   affine_scale(transform) * (1 << (level_index >> 1)) < .3)
		level_index += 2;
	BrushLevel base = g_array_index(levels, BrushLevel, 0), chosen = g_array_index(levels, BrushLevel, level_index / 2);
	Affine level_transform = create_identity_affine();
	translate_affine(&level_transform, -chosen.extent.x, -chosen.extent.y);
	scale_affine(&level_transform, (double)base.extent.width / chosen.extent.width,
				 (double)base.extent.height / chosen.extent.height);
	translate_affine(&level_transform, base.extent.x, base.extent.y);
	concatenate_affine(&level_transform, transform);
	int width = chosen.extent.width, height = chosen.extent.height;
	RasterExtent extent = bound_transformed_extent(chosen.extent, level_transform);
	BrushLevel result = {create_channels((gsize)MAX(0, extent.width) * MAX(0, extent.height), 8), extent};
	Affine inverse = invert_affine(level_transform);
	double origin_x, origin_y, next_x, next_y;
	transform_affine_point(inverse, 0, 0, &origin_x, &origin_y);
	transform_affine_point(inverse, 1, 0, &next_x, &next_y);
	double step_x = next_x - origin_x, step_y = next_y - origin_y;
	guint8 *target = result.values->value.buffer.data;
	for (int row = 0; row < extent.height; row++) {
		double x = inverse.a * (extent.x + .5) + inverse.c * (row + extent.y + .5) + inverse.e;
		double y = inverse.b * (extent.x + .5) + inverse.d * (row + extent.y + .5) + inverse.f;
		for (int column = 0; column < extent.width; column++) {
			if (.5 < x && x < width - .5 && .5 < y && y < height - .5) {
				double value = floor(.5 + sample_bilinear(x, y, chosen.values, width));
				target[row * extent.width + column] = isnan(value) ? 0 : value;
			}
			x += step_x;
			y += step_y;
		}
	}
	return result;
}
