#include <math.h>

#include "color-spaces.h"
#include "descriptor.h"
#include "gradient.h"
#include "pixel-selection.h"

#define TWO_TO_32 4294967296.0

static const GradientColorStop DEFAULT_COLORS[] = {{0, 50, GRADIENT_FOREGROUND, 0, 0, 0},
												   {4096, 50, GRADIENT_BACKGROUND, 0, 0, 0}};
static const GradientOpacityStop DEFAULT_OPACITIES[] = {{0, 50, 100}, {4096, 50, 100}};
static const Gradient DEFAULT_GRADIENT = {4096, DEFAULT_COLORS, 2, DEFAULT_OPACITIES, 2};

typedef struct {
	double value[3], location, midpoint;
} ResolvedStop;

static const GradientColorStop TWO_COLORS[] = {{0, 50, GRADIENT_USER_COLOR, 0, 0, 0},
											   {4096, 50, GRADIENT_USER_COLOR, 255, 255, 255}};
static const Gradient TWO_COLOR_GRADIENT = {4096, TWO_COLORS, 2, DEFAULT_OPACITIES, 2};

const Gradient *get_default_gradient(void) {
	return &DEFAULT_GRADIENT;
}

const Gradient *get_two_color_gradient(void) {
	return &TWO_COLOR_GRADIENT;
}

/* ============================== Descriptors ============================== */

static double read_typed_number(const Record *object, const char *key) {
	Record *value = get_record(get_record(object, key), "v");
	if (value && value->type == RECORD_OBJECT)
		return get_number(value, "val");
	return value && value->type == RECORD_NUMBER ? value->value.number : NAN;
}

Gradient *read_gradient_record(const Record *record) {
	Record *colors = get_record(get_record(record, "Clrs"), "v"),
		   *opacities = get_record(get_record(record, "Trns"), "v");
	guint color_count = count_records(colors), opacity_count = count_records(opacities);
	if (!color_count || !opacity_count)
		return NULL;
	Gradient *gradient = g_malloc0(sizeof(Gradient) + color_count * sizeof(GradientColorStop) +
								   opacity_count * sizeof(GradientOpacityStop));
	GradientColorStop *color_stops = (GradientColorStop *)(gradient + 1);
	GradientOpacityStop *opacity_stops = (GradientOpacityStop *)(color_stops + color_count);
	gradient->smoothness = read_typed_number(record, "Intr");
	gradient->colors = color_stops;
	gradient->color_count = color_count;
	gradient->opacities = opacity_stops;
	gradient->opacity_count = opacity_count;
	for (guint index = 0; index < color_count; index++) {
		Record *stop = get_record(get_item(colors, index), "v");
		const char *kind = get_string(get_record(get_record(stop, "Type"), "v"), "Clry");
		GradientColorStop *target = &color_stops[index];
		target->location = read_typed_number(stop, "Lctn");
		target->midpoint = read_typed_number(stop, "Mdpn");
		target->kind = !g_strcmp0(kind, "FrgC")	  ? GRADIENT_FOREGROUND
					   : !g_strcmp0(kind, "BckC") ? GRADIENT_BACKGROUND
												  : GRADIENT_USER_COLOR;
		double rgb[3] = {0};
		if (target->kind == GRADIENT_USER_COLOR &&
			!read_descriptor_color(get_record(get_record(stop, "Clr"), "v"), rgb)) {
			g_free(gradient);
			return NULL;
		}
		target->red = rgb[0];
		target->green = rgb[1];
		target->blue = rgb[2];
	}
	for (guint index = 0; index < opacity_count; index++) {
		Record *stop = get_record(get_item(opacities, index), "v");
		opacity_stops[index] = (GradientOpacityStop){read_typed_number(stop, "Lctn"), read_typed_number(stop, "Mdpn"),
													 read_typed_number(stop, "Opct")};
	}
	return gradient;
}

void free_gradient(Gradient *gradient) {
	g_free(gradient);
}

static Record *create_long(double value) {
	return create_typed_value("long", create_number(value));
}

Record *create_gradient_record(const Gradient *gradient, const char *name) {
	const char *const kinds[] = {"UsrS", "FrgC", "BckC"};
	Record *record = create_object(RECORD_PLAIN), *colors = create_array(), *opacities = create_array();
	set_string(record, "classID", "Grdn");
	set_record(record, "Nm", create_typed_value("TEXT", create_string(name)));
	Record *form = create_object(RECORD_PLAIN);
	set_string(form, "GrdF", "CstS");
	set_record(record, "GrdF", create_typed_value("enum", form));
	set_record(record, "Intr", create_typed_value("doub", create_number(gradient->smoothness)));
	for (int index = 0; index < gradient->color_count; index++) {
		const GradientColorStop *source = &gradient->colors[index];
		Record *stop = create_object(RECORD_PLAIN), *kind = create_object(RECORD_PLAIN);
		set_string(stop, "classID", "Clrt");
		set_string(kind, "Clry", kinds[source->kind]);
		set_record(stop, "Type", create_typed_value("enum", kind));
		set_record(stop, "Lctn", create_long(source->location));
		set_record(stop, "Mdpn", create_long(source->midpoint));
		set_record(stop, "Clr",
				   create_typed_value("Objc", create_rgb_descriptor(source->red, source->green, source->blue)));
		append_record(colors, create_typed_value("Objc", stop));
	}
	for (int index = 0; index < gradient->opacity_count; index++) {
		const GradientOpacityStop *source = &gradient->opacities[index];
		Record *stop = create_object(RECORD_PLAIN), *percent = create_object(RECORD_PLAIN);
		set_string(stop, "classID", "TrnS");
		set_string(percent, "type", "#Prc");
		set_number(percent, "val", source->opacity);
		set_record(stop, "Opct", create_typed_value("UntF", percent));
		set_record(stop, "Lctn", create_long(source->location));
		set_record(stop, "Mdpn", create_long(source->midpoint));
		append_record(opacities, create_typed_value("Objc", stop));
	}
	set_record(record, "Clrs", create_typed_value("VlLs", colors));
	set_record(record, "Trns", create_typed_value("VlLs", opacities));
	return record;
}

/* ============================== Source number conversions ============================== */

/* JavaScript ToUint32 wraps a number into 32 bits */
static guint32 wrap_to_uint32(double value) {
	if (!isfinite(value))
		return 0;
	double wrapped = fmod(trunc(value), TWO_TO_32);
	return (guint32)(wrapped < 0 ? wrapped + TWO_TO_32 : wrapped);
}

static gint32 truncate_like_source(double value) {
	if (value > -2147483648.0 && value < 2147483648.0)
		return (gint32)value;
	return (gint32)wrap_to_uint32(value);
}

static guint32 hash_dither_index(gint64 index) {
	guint32 value = (guint32)index;
	gint32 mixed = (gint32)(value ^ 61 ^ (value >> 16));
	guint32 grown = wrap_to_uint32((double)mixed + (double)(gint32)((guint32)mixed << 3));
	gint32 shifted = (gint32)(grown ^ (grown >> 4));
	guint32 product = wrap_to_uint32((double)shifted * 668265261.0);
	return product ^ (product >> 15);
}

/* ============================== Lookup ============================== */

static void resolve_gradient_stops(const Gradient *gradient, guint32 foreground, guint32 background,
								   ResolvedStop *colors, ResolvedStop *opacities, gboolean *opaque) {
	for (int index = 0; index < gradient->color_count; index++) {
		const GradientColorStop *stop = &gradient->colors[index];
		ResolvedStop *resolved = &colors[index];
		guint32 color = stop->kind == GRADIENT_FOREGROUND ? foreground : background;
		if (stop->kind == GRADIENT_USER_COLOR) {
			resolved->value[0] = MAX(stop->red, 0);
			resolved->value[1] = MAX(stop->green, 0);
			resolved->value[2] = MAX(stop->blue, 0);
		} else
			for (int channel = 0; channel < 3; channel++)
				resolved->value[channel] = color >> (16 - channel * 8) & 255;
		resolved->location = stop->location / 4096.0;
		resolved->midpoint = stop->midpoint / 100.0;
	}
	double lowest = 255;
	for (int index = 0; index < gradient->opacity_count; index++) {
		const GradientOpacityStop *stop = &gradient->opacities[index];
		opacities[index].value[0] = stop->opacity * (255.0 / 100);
		opacities[index].location = stop->location / 4096.0;
		opacities[index].midpoint = stop->midpoint / 100.0;
		lowest = MIN(lowest, opacities[index].value[0]);
	}
	*opaque = lowest > 254;
}

static double weigh_left_stop(const ResolvedStop *stops, int count, int index, double position, double smoothness) {
	if (count == 2)
		smoothness *= .5;
	double left = stops[index].location, right = stops[index + 1].location;
	double middle = left + stops[index + 1].midpoint * (right - left), part;
	if (position < middle)
		part = .5 * (position - left) / (middle - left);
	else
		part = .5 + .5 * (position - middle) / (right - middle);
	return .5 + .5 * (smoothness * cos(G_PI * part) + (1 - smoothness) * (1 - 2 * part));
}

static int find_left_stop(const ResolvedStop *stops, int count, double position) {
	int index = -1;
	while (index < count - 1 && stops[index + 1].location <= position)
		index++;
	return index;
}

static void sample_gradient(const ResolvedStop *colors, int color_count, const ResolvedStop *opacities,
							int opacity_count, gboolean opaque, double smoothness, double position, float *entry) {
	double alpha = 255;
	if (!opaque) {
		int index = find_left_stop(opacities, opacity_count, position);
		if (index == -1)
			alpha = opacities[0].value[0];
		else if (index == opacity_count - 1)
			alpha = opacities[index].value[0];
		else {
			double weight = weigh_left_stop(opacities, opacity_count, index, position, smoothness);
			alpha = weight * opacities[index].value[0] + (1 - weight) * opacities[index + 1].value[0];
		}
	}
	double channels[3];
	int index = find_left_stop(colors, color_count, position);
	if (index == -1 || index == color_count - 1)
		for (int channel = 0; channel < 3; channel++)
			channels[channel] = colors[MAX(index, 0)].value[channel];
	else {
		double weight = weigh_left_stop(colors, color_count, index, position, smoothness);
		for (int channel = 0; channel < 3; channel++)
			channels[channel] =
				MIN(255, weight * colors[index].value[channel] + (1 - weight) * colors[index + 1].value[channel]);
	}
	for (int channel = 0; channel < 3; channel++)
		entry[channel] = (float)(channels[channel] * (1.0 / 255));
	entry[3] = (float)(alpha * (1.0 / 255));
}

static void convert_stop_colors(ResolvedStop *colors, int count, GradientInterpolation interpolation) {
	for (int index = 0; index < count; index++) {
		double *value = colors[index].value;
		if (interpolation == GRADIENT_LINEAR_LIGHT)
			for (int channel = 0; channel < 3; channel++)
				value[channel] = 255 * linearize_srgb(value[channel] / 255);
		else if (interpolation == GRADIENT_PERCEPTUAL || interpolation == GRADIENT_SMOOTH) {
			double lab[3];
			convert_rgb_to_lab(value[0], value[1], value[2], lab);
			value[0] = lab[0] * 2.55;
			value[1] = 127.5 + lab[1];
			value[2] = 127.5 + lab[2];
		}
	}
}

/* Stripes show the stop on the left of each entry at full strength */
static void fill_stripe_entry(const ResolvedStop *colors, int color_count, const ResolvedStop *opacities,
							  int opacity_count, int index, float *entry) {
	int left = 0;
	while (left < color_count - 1 && colors[left + 1].location * GRADIENT_LOOKUP_SIZE < index)
		left++;
	for (int channel = 0; channel < 3; channel++)
		entry[channel] = colors[left].value[channel] * (1.0 / 255);
	left = 0;
	while (left < opacity_count - 1 && opacities[left + 1].location * GRADIENT_LOOKUP_SIZE < index)
		left++;
	entry[3] = opacities[left].value[0] * (1.0 / 255);
}

static void convert_lookup_entry(GradientInterpolation interpolation, float *entry) {
	if (interpolation == GRADIENT_LINEAR_LIGHT)
		for (int channel = 0; channel < 3; channel++)
			entry[channel] = encode_srgb(entry[channel]);
	else if (interpolation == GRADIENT_PERCEPTUAL || interpolation == GRADIENT_SMOOTH) {
		double rgb[3];
		convert_lab_to_rgb(entry[0] * 100, (entry[1] - .5) * 255, (entry[2] - .5) * 255, rgb);
		for (int channel = 0; channel < 3; channel++)
			entry[channel] = rgb[channel] * (1.0 / 255);
	}
}

void build_gradient_lookup(const Gradient *gradient, gboolean reverse, GradientInterpolation interpolation,
						   guint32 foreground, guint32 background, float *lookup) {
	g_autofree ResolvedStop *colors = g_new(ResolvedStop, gradient->color_count);
	g_autofree ResolvedStop *opacities = g_new(ResolvedStop, gradient->opacity_count);
	gboolean opaque;
	resolve_gradient_stops(gradient, foreground, background, colors, opacities, &opaque);
	convert_stop_colors(colors, gradient->color_count, interpolation);
	double smoothness = gradient->smoothness * (1.0 / 4096), step = 1.0 / GRADIENT_LOOKUP_SIZE;
	for (int index = 0; index < GRADIENT_LOOKUP_SIZE; index++) {
		double position = (reverse ? GRADIENT_LOOKUP_SIZE - 1 - index : index) * step;
		float *entry = lookup + index * 4;
		sample_gradient(colors, gradient->color_count, opacities, gradient->opacity_count, opaque, smoothness, position,
						entry);
		if (interpolation == GRADIENT_STRIPES)
			fill_stripe_entry(colors, gradient->color_count, opacities, gradient->opacity_count, index, entry);
		else
			convert_lookup_entry(interpolation, entry);
	}
}

/* ============================== Geometry ============================== */

void constrain_gradient_point(double start_x, double start_y, double *x, double *y) {
	if (*x == start_x && *y == start_y)
		return;
	double across = *x - start_x, down = *y - start_y;
	double width = fabs(across), height = fabs(down);
	if (width > height)
		height = width / 2 > height ? 0 : width;
	if (height > width)
		width = height / 2 > width ? 0 : height;
	width = across > 0 ? width : -width;
	height = down > 0 ? height : -height;
	double scale = sqrt(across * across + down * down) / sqrt(width * width + height * height);
	*x = start_x + width * scale;
	*y = start_y + height * scale;
}

static double measure_edge_distance(double direction_x, double direction_y, RasterExtent area) {
	double half_height = .5 * area.height, across = half_height * (direction_x / direction_y);
	double vertical = sqrt(half_height * half_height + across * across);
	double half_width = .5 * area.width, down = half_width * (direction_y / direction_x);
	double horizontal = sqrt(half_width * half_width + down * down);
	return MIN(vertical, horizontal);
}

static void store_gradient_points(double *start_x, double *start_y, double *end_x, double *end_y, RasterExtent area) {
	double across = *end_x - *start_x, up = -(*end_y - *start_y), length = sqrt(across * across + up * up);
	double angle = atan2(up, across), scale = length / measure_edge_distance(cos(angle), -sin(angle), area);
	double angle_degrees = 180 * angle / G_PI, scale_percent = scale * 100;
	double offset_x = (*start_x - area.x - area.width / 2.0) / area.width * 100;
	double offset_y = (*start_y - area.y - area.height / 2.0) / area.height * 100;
	double radians = G_PI * angle_degrees / 180, direction_x = cos(radians), direction_y = -sin(radians);
	double reach = measure_edge_distance(direction_x, direction_y, area) * (scale_percent / 100);
	*start_x = area.x + area.width / 2.0 + offset_x / 100 * area.width;
	*start_y = area.y + area.height / 2.0 + offset_y / 100 * area.height;
	*end_x = *start_x + direction_x * reach;
	*end_y = *start_y + direction_y * reach;
}

static double get_shape_position(GradientType type, double along, double sideways) {
	switch (type) {
	case GRADIENT_RADIAL:
		return 2 * sqrt(along * along + sideways * sideways);
	case GRADIENT_ANGLE:
		return (G_PI + atan2(-sideways, -along)) / (2 * G_PI);
	case GRADIENT_REFLECTED:
		return fabs(along * 2);
	case GRADIENT_DIAMOND:
		return 2 * (fabs(along) + fabs(sideways));
	default:
		return along + .5;
	}
}

static int find_lookup_entry(double position) {
	gint32 entry = truncate_like_source(.499 + position * (GRADIENT_LOOKUP_SIZE - 1));
	return CLAMP(entry, 0, GRADIENT_LOOKUP_SIZE - 1) * 4;
}

/* ============================== Rendering ============================== */

static void write_dithered_pixel(const float *entry, guint32 noise, guint8 *pixel) {
	for (int channel = 0; channel < 4; channel++) {
		double value = entry[channel] * 255.0, rounded = floor(value);
		if ((noise >> (channel * 8) & 255) < (value - rounded) * 255)
			rounded++;
		pixel[channel] = (guint8)rounded;
	}
}

static void render_gradient(const float *lookup, GradientType type, gboolean dither, double start_x, double start_y,
							double end_x, double end_y, RasterExtent area, guint32 dither_offset, guint8 *pixels) {
	double across = end_x - start_x, down = end_y - start_y, length = sqrt(across * across + down * down);
	double factor = 1 / (2 * length * length);
	double matrix[4] = {across * factor, down * factor, -down * factor, across * factor};
	double left = area.x - start_x, top = area.y - start_y;
	gint64 pixel = 0;
	for (int row = 0; row < area.height; row++)
		for (int column = 0; column < area.width; column++, pixel++) {
			double x = column + left, y = row + top;
			double along = matrix[0] * x + matrix[1] * y, sideways = matrix[2] * x + matrix[3] * y;
			const float *entry = lookup + find_lookup_entry(get_shape_position(type, along, sideways));
			guint8 *target = pixels + pixel * 4;
			if (dither) {
				write_dithered_pixel(entry, hash_dither_index(pixel + dither_offset), target);
				continue;
			}
			for (int channel = 0; channel < 4; channel++)
				target[channel] = (guint8)(int)(entry[channel] * 255.0);
		}
}

void fill_gradient_pixels(const Gradient *gradient, const GradientOptions *options, guint32 foreground,
						  guint32 background, double start_x, double start_y, double end_x, double end_y,
						  RasterExtent area, guint32 dither_offset, guint8 *pixels) {
	if (options->type == GRADIENT_LINEAR) {
		start_x = (start_x + end_x) / 2;
		start_y = (start_y + end_y) / 2;
	}
	store_gradient_points(&start_x, &start_y, &end_x, &end_y, area);
	g_autofree float *lookup = g_new(float, GRADIENT_LOOKUP_SIZE * 4);
	build_gradient_lookup(gradient, options->reverse, options->interpolation, foreground, background, lookup);
	render_gradient(lookup, options->type, options->dither, start_x, start_y, end_x, end_y, area, dither_offset,
					pixels);
	if (options->opacity == 100)
		return;
	double opacity = options->opacity / 100;
	gsize length = (gsize)area.width * area.height * 4;
	for (gsize index = 3; index < length; index += 4)
		pixels[index] = (guint8)round_like_source(pixels[index] * opacity);
}

gboolean draw_gradient(PaintTool *tool, Document *document, const GradientOptions *options, guint32 foreground,
					   guint32 background, double start_x, double start_y, double end_x, double end_y, GError **error) {
	if (sqrt((end_x - start_x) * (end_x - start_x) + (end_y - start_y) * (end_y - start_y)) <= 2)
		return TRUE;
	Record *selection = get_document_selection(document);
	RasterExtent area = selection ? read_extent(get_record(selection, "V"))
								  : (RasterExtent){0, 0, document_width(document), document_height(document)};
	g_autoptr(Record) pixels = create_channels((gsize)area.width * area.height * 4, 8);
	guint32 offset = (guint32)fmod(round_like_source(g_random_double() * area.width * area.height * 100), TWO_TO_32);
	fill_gradient_pixels(get_default_gradient(), options, foreground, background, start_x, start_y, end_x, end_y, area,
						 offset, pixels->value.buffer.data);
	return draw_paint_bitmap(tool, document, pixels, area, error);
}
