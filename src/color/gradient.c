#include <math.h>

#include "document/numbers.h"
#include "color-spaces.h"
#include "psd/descriptor.h"
#include "gradient.h"
#include "selection/pixel-selection.h"

/* Stops sit halfway to their neighbours unless moved, and a full opacity is 100 percent */
#define DEFAULT_MIDPOINT (PERCENT / 2)
/* Lookup entries round a hair under half */
#define LOOKUP_ROUNDING .499
/* Gradients shorter than this are not drawn */
#define SHORTEST_GRADIENT 2
/* The dither starts at a random place among this many times the pixel count */
#define DITHER_OFFSET_SPREAD 100

static const GradientColorStop DEFAULT_COLORS[] = {{0, DEFAULT_MIDPOINT, GRADIENT_FOREGROUND, 0, 0, 0},
												   {GRADIENT_LOCATION_RANGE, DEFAULT_MIDPOINT, GRADIENT_BACKGROUND, 0, 0, 0}};
static const GradientOpacityStop DEFAULT_OPACITIES[] = {{0, DEFAULT_MIDPOINT, PERCENT}, {GRADIENT_LOCATION_RANGE, DEFAULT_MIDPOINT, PERCENT}};
static const Gradient DEFAULT_GRADIENT = {GRADIENT_LOCATION_RANGE, DEFAULT_COLORS, G_N_ELEMENTS(DEFAULT_COLORS),
										  DEFAULT_OPACITIES, G_N_ELEMENTS(DEFAULT_OPACITIES)};

typedef struct {
	double value[RGB_CHANNELS], location, midpoint;
} ResolvedStop;

static const GradientColorStop TWO_COLORS[] = {
	{0, DEFAULT_MIDPOINT, GRADIENT_USER_COLOR, 0, 0, 0},
	{GRADIENT_LOCATION_RANGE, DEFAULT_MIDPOINT, GRADIENT_USER_COLOR, G_MAXUINT8, G_MAXUINT8, G_MAXUINT8}};
static const Gradient TWO_COLOR_GRADIENT = {GRADIENT_LOCATION_RANGE, TWO_COLORS, G_N_ELEMENTS(TWO_COLORS),
											DEFAULT_OPACITIES, G_N_ELEMENTS(DEFAULT_OPACITIES)};

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
		double rgb[RGB_CHANNELS] = {0};
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
			for (int channel = 0; channel < RGB_CHANNELS; channel++)
				resolved->value[channel] = color >> (WORD_RED_SHIFT - channel * CHAR_BIT) & G_MAXUINT8;
		resolved->location = stop->location / (double)GRADIENT_LOCATION_RANGE;
		resolved->midpoint = from_percent(stop->midpoint);
	}
	double lowest = G_MAXUINT8;
	for (int index = 0; index < gradient->opacity_count; index++) {
		const GradientOpacityStop *stop = &gradient->opacities[index];
		opacities[index].value[0] = stop->opacity * (255.0 / PERCENT);
		opacities[index].location = stop->location / (double)GRADIENT_LOCATION_RANGE;
		opacities[index].midpoint = from_percent(stop->midpoint);
		lowest = MIN(lowest, opacities[index].value[0]);
	}
	*opaque = lowest > G_MAXUINT8 - 1;
}

static double weigh_left_stop(const ResolvedStop *stops, int count, int index, double position, double smoothness) {
	/* Two stops bend half as much */
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
	double alpha = G_MAXUINT8;
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
	double channels[RGB_CHANNELS];
	int index = find_left_stop(colors, color_count, position);
	if (index == -1 || index == color_count - 1)
		for (int channel = 0; channel < RGB_CHANNELS; channel++)
			channels[channel] = colors[MAX(index, 0)].value[channel];
	else {
		double weight = weigh_left_stop(colors, color_count, index, position, smoothness);
		for (int channel = 0; channel < RGB_CHANNELS; channel++)
			channels[channel] =
				MIN(G_MAXUINT8, weight * colors[index].value[channel] + (1 - weight) * colors[index + 1].value[channel]);
	}
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		entry[channel] = (float)(channels[channel] * BYTE_UNIT);
	entry[ALPHA_CHANNEL] = (float)(alpha * BYTE_UNIT);
}

static void convert_stop_colors(ResolvedStop *colors, int count, GradientInterpolation interpolation) {
	for (int index = 0; index < count; index++) {
		double *value = colors[index].value;
		if (interpolation == GRADIENT_LINEAR_LIGHT)
			for (int channel = 0; channel < RGB_CHANNELS; channel++)
				value[channel] = G_MAXUINT8 * linearize_srgb(byte_to_unit(value[channel]));
		else if (interpolation == GRADIENT_PERCEPTUAL || interpolation == GRADIENT_SMOOTH) {
			double lab[RGB_CHANNELS];
			convert_rgb_to_lab(value[0], value[1], value[2], lab);
			/* Lab in byte ranges, with a and b centered on half a byte */
			value[0] = lab[0] * (G_MAXUINT8 / PERCENT);
			value[1] = G_MAXUINT8 / 2.0 + lab[1];
			value[2] = G_MAXUINT8 / 2.0 + lab[2];
		}
	}
}

/* Stripes show the stop on the left of each entry at full strength */
static void fill_stripe_entry(const ResolvedStop *colors, int color_count, const ResolvedStop *opacities,
							  int opacity_count, int index, float *entry) {
	int left = 0;
	while (left < color_count - 1 && colors[left + 1].location * GRADIENT_LOOKUP_SIZE < index)
		left++;
	for (int channel = 0; channel < RGB_CHANNELS; channel++)
		entry[channel] = colors[left].value[channel] * BYTE_UNIT;
	left = 0;
	while (left < opacity_count - 1 && opacities[left + 1].location * GRADIENT_LOOKUP_SIZE < index)
		left++;
	entry[ALPHA_CHANNEL] = opacities[left].value[0] * BYTE_UNIT;
}

static void convert_lookup_entry(GradientInterpolation interpolation, float *entry) {
	if (interpolation == GRADIENT_LINEAR_LIGHT)
		for (int channel = 0; channel < RGB_CHANNELS; channel++)
			entry[channel] = encode_srgb(entry[channel]);
	else if (interpolation == GRADIENT_PERCEPTUAL || interpolation == GRADIENT_SMOOTH) {
		double rgb[RGB_CHANNELS];
		convert_lab_to_rgb(entry[0] * PERCENT, unit_to_byte(entry[1] - .5), unit_to_byte(entry[2] - .5), rgb);
		for (int channel = 0; channel < RGB_CHANNELS; channel++)
			entry[channel] = rgb[channel] * BYTE_UNIT;
	}
}

void build_gradient_lookup(const Gradient *gradient, gboolean reverse, GradientInterpolation interpolation,
						   guint32 foreground, guint32 background, float *lookup) {
	g_autofree ResolvedStop *colors = g_new(ResolvedStop, gradient->color_count);
	g_autofree ResolvedStop *opacities = g_new(ResolvedStop, gradient->opacity_count);
	gboolean opaque;
	resolve_gradient_stops(gradient, foreground, background, colors, opacities, &opaque);
	convert_stop_colors(colors, gradient->color_count, interpolation);
	double smoothness = gradient->smoothness * (1.0 / GRADIENT_LOCATION_RANGE), step = 1.0 / GRADIENT_LOOKUP_SIZE;
	for (int index = 0; index < GRADIENT_LOOKUP_SIZE; index++) {
		double position = (reverse ? GRADIENT_LOOKUP_SIZE - 1 - index : index) * step;
		float *entry = lookup + index * RGBA_CHANNELS;
		sample_gradient(colors, gradient->color_count, opacities, gradient->opacity_count, opaque, smoothness, position,
						entry);
		if (interpolation == GRADIENT_STRIPES)
			fill_stripe_entry(colors, gradient->color_count, opacities, gradient->opacity_count, index, entry);
		else
			convert_lookup_entry(interpolation, entry);
	}
}

/* ============================== Geometry ============================== */

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
	/* The points go through degrees and percents and back, as a saved gradient keeps them */
	const double half_turn = DEGREES_PER_TURN / 2;
	double angle_degrees = half_turn * angle / G_PI, scale_percent = scale * PERCENT;
	double offset_x = (*start_x - area.x - area.width / 2.0) / area.width * PERCENT;
	double offset_y = (*start_y - area.y - area.height / 2.0) / area.height * PERCENT;
	double radians = G_PI * angle_degrees / half_turn, direction_x = cos(radians), direction_y = -sin(radians);
	double reach = measure_edge_distance(direction_x, direction_y, area) * (scale_percent / PERCENT);
	*start_x = area.x + area.width / 2.0 + offset_x / PERCENT * area.width;
	*start_y = area.y + area.height / 2.0 + offset_y / PERCENT * area.height;
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
	gint32 entry = wrap_int32(LOOKUP_ROUNDING + position * (GRADIENT_LOOKUP_SIZE - 1));
	return CLAMP(entry, 0, GRADIENT_LOOKUP_SIZE - 1) * RGBA_CHANNELS;
}

/* ============================== Rendering ============================== */

static void write_dithered_pixel(const float *entry, guint32 noise, guint8 *pixel) {
	for (int channel = 0; channel < RGBA_CHANNELS; channel++) {
		/* Each channel rounds up when its byte of the noise is below the part that was cut off */
		double value = entry[channel] * 255.0, rounded = floor(value);
		if ((noise >> (channel * CHAR_BIT) & G_MAXUINT8) < (value - rounded) * G_MAXUINT8)
			rounded++;
		pixel[channel] = (guint8)rounded;
	}
}

static void render_gradient(const float *lookup, GradientType type, gboolean dither, double start_x, double start_y,
							double end_x, double end_y, RasterExtent area, guint32 dither_offset, guint8 *pixels) {
	double across = end_x - start_x, down = end_y - start_y, length = sqrt(across * across + down * down);
	double factor = 1 / (2 * length * length);
	double matrix[2 * 2] = {across * factor, down * factor, -down * factor, across * factor};
	double left = area.x - start_x, top = area.y - start_y;
	gint64 pixel = 0;
	for (int row = 0; row < area.height; row++)
		for (int column = 0; column < area.width; column++, pixel++) {
			double x = column + left, y = row + top;
			double along = matrix[0] * x + matrix[1] * y, sideways = matrix[2] * x + matrix[3] * y;
			const float *entry = lookup + find_lookup_entry(get_shape_position(type, along, sideways));
			guint8 *target = pixels + pixel * RGBA_CHANNELS;
			if (dither) {
				write_dithered_pixel(entry, hash_whole_number(pixel + dither_offset), target);
				continue;
			}
			for (int channel = 0; channel < RGBA_CHANNELS; channel++)
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
	g_autofree float *lookup = g_new(float, GRADIENT_LOOKUP_SIZE * RGBA_CHANNELS);
	build_gradient_lookup(gradient, options->reverse, options->interpolation, foreground, background, lookup);
	render_gradient(lookup, options->type, options->dither, start_x, start_y, end_x, end_y, area, dither_offset,
					pixels);
	if (options->opacity == PERCENT)
		return;
	double opacity = from_percent(options->opacity);
	gsize length = (gsize)area.width * area.height * RGBA_CHANNELS;
	for (gsize index = ALPHA_CHANNEL; index < length; index += RGBA_CHANNELS)
		pixels[index] = (guint8)round_half_up(pixels[index] * opacity);
}

gboolean draw_gradient(PaintTool *tool, Document *document, const GradientOptions *options, guint32 foreground,
					   guint32 background, double start_x, double start_y, double end_x, double end_y, GError **error) {
	if (sqrt((end_x - start_x) * (end_x - start_x) + (end_y - start_y) * (end_y - start_y)) <= SHORTEST_GRADIENT)
		return TRUE;
	Record *selection = get_document_selection(document);
	RasterExtent area = selection ? read_extent(get_record(selection, "V"))
								  : (RasterExtent){0, 0, document_width(document), document_height(document)};
	g_autoptr(Record) pixels = create_channels((gsize)area.width * area.height * RGBA_CHANNELS, BYTE_DEPTH);
	guint32 offset = wrap_uint32(round_half_up(g_random_double() * area.width * area.height * DITHER_OFFSET_SPREAD));
	fill_gradient_pixels(get_default_gradient(), options, foreground, background, start_x, start_y, end_x, end_y, area,
						 offset, pixels->value.buffer.data);
	return draw_paint_bitmap(tool, document, pixels, area, error);
}
