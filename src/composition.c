#include <math.h>
#include <string.h>

#include "adjustment-kernels.h"
#include "document.h"
#include "blend.h"
#include "kernels.h"
#include "parallel.h"
#include "../lang/en.h"


typedef struct {
	const guint8 *values;
	RasterExtent extent;
	guint step, offset, outside;
} MaskSampler;

/* Scratch buffers follow ga's exact and best-fit allocation rules within a redraw, and natively skip clearing,
 * since every part read back is written first */
typedef struct {
	Record *channels;
	int width, height;
	gboolean active;
} CompositionBuffer;

/* A redraw owns its scratch buffers while editable records remain in the document */
typedef struct {
	const Document *document;
	const Record *selection;
	GPtrArray *buffers;
	GError **error;
	gboolean failed;
} Composition;

/* et.Q_.Y9 combines the default color with density outside the mask extent */
static guint mask_outside_value(const Record *mask) {
	return (guint)round_like_source(255 - (255 - get_number(mask, "color")) * (get_number(mask, "mU") / 255));
}

/* et.tz ignores masks that are disabled or that are empty with an opaque outside */
static gboolean is_mask_active(const Record *layer) {
	Record *mask = get_record(layer, "yy");
	return has_record(layer, "yy") && get_boolean(mask, "isEnabled") &&
		   (!empty_extent(read_extent(get_record(mask, "V"))) || mask_outside_value(mask) != 255);
}

/* et.Q_.AM applies density through two inversions and leaves padding transformed too */
static guint8 *create_mask_values(const Record *mask) {
	Record *channels = get_record(mask, "C");
	guint8 *values = g_memdup2(channels->value.buffer.data, channels->value.buffer.length);
	double density = get_number(mask, "mU");
	if (density == 255)
		return values;
	for (gsize index = 0; index < channels->value.buffer.length; index++)
		values[index] = 255 - ((guint)round_like_source((255 - values[index]) * (density / 255)) & 255);
	return values;
}

static MaskSampler create_mask_sampler(const guint8 *values, gsize length, RasterExtent extent, guint outside) {
	gboolean is_rgba = length == (gsize)extent.width * extent.height * 4;
	return (MaskSampler){values, extent, is_rgba ? 4 : 1, is_rgba ? 3 : 0, outside};
}

static guint sample_mask(const MaskSampler *mask, int x, int y) {
	RasterExtent extent = mask->extent;
	if (x < extent.x || y < extent.y || x >= extent.x + extent.width || y >= extent.y + extent.height)
		return mask->outside;
	return mask->values[((gsize)(y - extent.y) * extent.width + x - extent.x) * mask->step + mask->offset];
}

/* et.ot with gk's mask component intersects for a transparent outside and unions otherwise, and d6.ot gives an
 * adjustment layer the canvas, or its mask's extent when the mask is black by default */
static RasterExtent layer_extent(const Document *document, const Record *layer) {
	if (is_adjustment_layer(layer)) {
		Record *mask = get_record(layer, "yy");
		if (is_mask_active(layer) && get_number(mask, "color") == 0)
			return read_extent(get_record(mask, "V"));
		return (RasterExtent){0, 0, document_width(document), document_height(document)};
	}
	RasterExtent extent = {0}, bounds = read_extent(get_record(layer, "V"));
	if (!((guint)get_number(layer, "NF") & 16) && !empty_extent(bounds))
		extent = union_region(extent, bounds);
	if (!has_record(layer, "yy"))
		return extent;
	Record *mask = get_record(layer, "yy");
	RasterExtent mask_extent = read_extent(get_record(mask, "V"));
	if (empty_extent(mask_extent))
		return extent;
	if (mask_outside_value(mask))
		return union_region(extent, mask_extent);
	return empty_extent(extent) ? mask_extent : intersect_region(extent, mask_extent);
}

/* d6.ot ignores a group's own mask and unions its children */
RasterExtent get_tree_extent(const Document *document, const LayerTree *tree) {
	RasterExtent extent = {0};
	if (!is_layer_visible(tree->layer))
		return extent;
	if (!tree->children)
		return layer_extent(document, tree->layer);
	for (guint index = 0; index < tree->children->len; index++)
		extent = union_region(extent, get_tree_extent(document, g_ptr_array_index(tree->children, index)));
	return extent;
}

static gboolean valid_extent(const Record *extent) {
	if (!extent || extent->type != RECORD_OBJECT)
		return FALSE;
	const char *fields[] = {"x", "y", "s", "T"};
	for (guint index = 0; index < G_N_ELEMENTS(fields); index++) {
		double value = get_number(extent, fields[index]);
		if (!isfinite(value) || value != trunc(value) || fabs(value) > 32768)
			return FALSE;
	}
	return get_number(extent, "s") >= 0 && get_number(extent, "T") >= 0;
}

static gboolean has_bounded_number(const Record *record, const char *key) {
	Record *value = get_record(record, key);
	return value && value->type == RECORD_NUMBER && isfinite(value->value.number) && value->value.number >= 0 &&
		   value->value.number <= 255;
}

static gboolean supports_layer_mask(const Record *mask) {
	if (mask->type != RECORD_OBJECT || mask->class != RECORD_MASK || !valid_extent(get_record(mask, "V")))
		return FALSE;
	Record *enabled = get_record(mask, "isEnabled"), *feather = get_record(mask, "O1");
	if (!enabled || enabled->type != RECORD_BOOLEAN || !feather || feather->type != RECORD_NUMBER ||
		feather->value.number != 0 || !has_bounded_number(mask, "color") || !has_bounded_number(mask, "mU"))
		return FALSE;
	Record *channels = get_record(mask, "C");
	RasterExtent extent = read_extent(get_record(mask, "V"));
	gsize area = (gsize)extent.width * extent.height;
	return channels && channels->type == RECORD_BUFFER && channels->value.buffer.depth == 8 &&
		   channels->value.buffer.length == area + (4 - area % 4) % 4;
}

static gboolean supports_adjustment(const char *key, const Record *descriptor) {
	if (!is_ported_adjustment(key) || descriptor->type != RECORD_OBJECT)
		return FALSE;
	/* Settings the kernel cannot build, such as CMYK colors or noise gradients, keep the saved composite */
	g_autofree Adjustment *adjustment = g_new(Adjustment, 1);
	if (!build_adjustment(key, descriptor, adjustment))
		return FALSE;
	if (!strcmp(key, "post")) {
		double levels = get_descriptor_number(descriptor, "Lvls", 0);
		return levels >= 2 && levels <= 255;
	}
	if (!strcmp(key, "thrs"))
		return isfinite(get_descriptor_number(descriptor, "Lvl", NAN));
	if (!strcmp(key, "brit"))
		return isfinite(get_descriptor_number(descriptor, "Brgh", 0)) &&
			   isfinite(get_descriptor_number(descriptor, "Cntr", 0));
	if (!strcmp(key, "vibA"))
		return isfinite(get_descriptor_number(descriptor, "vibrance", 0)) &&
			   isfinite(get_descriptor_number(descriptor, "Strt", 0));
	if (!strcmp(key, "blnc")) {
		const char *tones[] = {"ShdL", "MdtL", "HghL"};
		for (guint index = 0; index < G_N_ELEMENTS(tones); index++) {
			Record *shifts = get_record(get_record(descriptor, tones[index]), "v");
			if (has_record(descriptor, tones[index]) && (!shifts || count_records(shifts) < 3))
				return FALSE;
		}
	}
	if (!strcmp(key, "expA")) {
		const char *fields[] = {"Exps", "Ofst", "gammaCorrection"};
		for (guint index = 0; index < G_N_ELEMENTS(fields); index++)
			if (has_record(descriptor, fields[index]) &&
				!isfinite(get_descriptor_number(descriptor, fields[index], NAN)))
				return FALSE;
		return get_descriptor_number(descriptor, "gammaCorrection", 1) > 0;
	}
	if (!strcmp(key, "levl")) {
		for (int channel = 0; channel < 4; channel++) {
			double values[5];
			get_levels_channel(descriptor, channel, values);
			if (!(values[0] >= 0 && values[1] <= 255 && values[0] < values[1] && values[2] >= 0 && values[3] <= 255 &&
				  values[4] > 0))
				return FALSE;
		}
	}
	return TRUE;
}

static gboolean supports_composition_layer(const Record *layer) {
	Record *descriptors = get_record(layer, "add");
	if (!descriptors || descriptors->type != RECORD_OBJECT)
		return FALSE;
	const char *adjustment = find_adjustment_key(descriptors);
	const char *supported[] = {"lyid", "lsct", "lclr", "fxrp", "luni", "lspf", "iOpa", "clbl", "infx",
							   "lnsr", "lyvr", "sn2P", "vowv", "lmgm", "vmgm", "knko", "brst"};
	GHashTableIter iterator;
	gpointer key, value;
	g_hash_table_iter_init(&iterator, descriptors->value.object);
	while (g_hash_table_iter_next(&iterator, &key, &value)) {
		gboolean found = FALSE;
		for (guint index = 0; index < G_N_ELEMENTS(supported); index++)
			if (g_str_equal(key, supported[index]))
				found = TRUE;
		if (adjustment && g_str_equal(key, adjustment) && supports_adjustment(adjustment, value))
			found = TRUE;
		if (!found)
			return FALSE;
	}
	if (has_record(descriptors, "knko") && get_number(descriptors, "knko") != 0)
		return FALSE;
	Record *restricted = get_record(descriptors, "brst");
	if (restricted) {
		if (restricted->type != RECORD_ARRAY || count_records(restricted) != 3)
			return FALSE;
		for (guint index = 0; index < 3; index++)
			if (get_item(restricted, index)->type != RECORD_NUMBER || get_item(restricted, index)->value.number != 1)
				return FALSE;
	}
	Record *ranges = get_record(layer, "hu");
	if (count_records(ranges) != 40)
		return FALSE;
	for (guint index = 0; index < 40; index++) {
		Record *range = get_item(ranges, index);
		if (!range || range->type != RECORD_NUMBER || !(range->value.number >= 0 && range->value.number <= 255))
			return FALSE;
	}
	double flags = get_number(layer, "NF");
	if (!isfinite(flags) || flags < 0 || flags > G_MAXUINT)
		return FALSE;
	if (has_record(layer, "E_") || (has_record(layer, "yy") && !supports_layer_mask(get_record(layer, "yy"))) ||
		!valid_extent(get_record(layer, "V")))
		return FALSE;
	const char *mode = get_string(layer, "qu");
	if (find_blend_mode(mode) < 0 && !(is_layer_group(layer) && !g_strcmp0(mode, "pass")))
		return FALSE;
	double opacity = get_number(layer, "nv");
	double fill = has_record(descriptors, "iOpa") ? get_number(descriptors, "iOpa") : 255;
	if (!isfinite(opacity) || opacity < 0 || opacity > 255 || !isfinite(fill) || fill < 0 || fill > 255)
		return FALSE;
	Record *buffer = get_record(layer, "buffer");
	RasterExtent extent = read_extent(get_record(layer, "V"));
	return buffer && buffer->type == RECORD_BUFFER && buffer->value.buffer.depth == 8 &&
		   (guint64)extent.width * extent.height * 4 <= buffer->value.buffer.length;
}

static void free_composition_buffer(CompositionBuffer *buffer) {
	free_record(buffer->channels);
	g_free(buffer);
}

static Record *acquire_buffer(Composition *composition, RasterExtent *extent, gboolean exact) {
	CompositionBuffer *best = NULL;
	gint64 area = (gint64)extent->width * extent->height;
	for (guint index = 0; index < composition->buffers->len; index++) {
		CompositionBuffer *candidate = g_ptr_array_index(composition->buffers, index);
		gint64 candidate_area = (gint64)candidate->width * candidate->height;
		if (candidate->active || candidate->width < extent->width || candidate->height < extent->height)
			continue;
		if (exact && (candidate->width != extent->width || candidate->height != extent->height))
			continue;
		if (!exact && area * 10 < candidate_area)
			continue;
		if (!best || candidate_area < (gint64)best->width * best->height)
			best = candidate;
	}
	if (!best) {
		Record *channels =
			area >= 0 && (guint64)area <= DOCUMENT_MAX_PIXELS ? create_scratch_channels(area * 4, 8) : NULL;
		if (!channels) {
			if (!composition->failed)
				g_set_error_literal(composition->error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
									TEXT_CHANNEL_ALLOCATION_FAILED);
			composition->failed = TRUE;
			return NULL;
		}
		best = g_new0(CompositionBuffer, 1);
		best->channels = channels;
		best->width = extent->width;
		best->height = extent->height;
		g_ptr_array_add(composition->buffers, best);
	}
	best->active = TRUE;
	extent->width = best->width;
	extent->height = best->height;
	return best->channels;
}

static void release_buffer(Composition *composition, Record *channels) {
	for (guint index = 0; index < composition->buffers->len; index++) {
		CompositionBuffer *buffer = g_ptr_array_index(composition->buffers, index);
		if (buffer->channels == channels)
			buffer->active = FALSE;
	}
}

/* The arguments of one region operation, shared by the rows each thread works on */
typedef struct {
	const Record *source;
	RasterExtent source_extent;
	Record *destination;
	RasterExtent destination_extent, region;
	const MaskSampler *mask;
	double opacity;
	const BlendKernel *kernel;
	gboolean plain;
	const Adjustment *adjustment;
} RegionWork;

static void clear_rows(int first, int last, gpointer data) {
	const RegionWork *work = data;
	RasterExtent region = work->region, target = work->destination_extent;
	for (int y = first; y < last; y++)
		memset((guint8 *)work->destination->value.buffer.data +
				   ((gsize)(y - target.y) * target.width + region.x - target.x) * 4,
			   0, (gsize)region.width * 4);
}

static void copy_rows(int first, int last, gpointer data) {
	const RegionWork *work = data;
	RasterExtent region = work->region, source_extent = work->source_extent, target = work->destination_extent;
	for (int y = first; y < last; y++) {
		gsize from = ((gsize)(y - source_extent.y) * source_extent.width + region.x - source_extent.x) * 4;
		gsize to = ((gsize)(y - target.y) * target.width + region.x - target.x) * 4;
		memmove((guint8 *)work->destination->value.buffer.data + to,
				(const guint8 *)work->source->value.buffer.data + from, (gsize)region.width * 4);
	}
}

static void copy_region(const Record *source, RasterExtent source_extent, Record *destination,
						RasterExtent destination_extent, RasterExtent clip) {
	RasterExtent region = intersect_region(intersect_region(source_extent, destination_extent), clip);
	if (empty_extent(region))
		return;
	RegionWork work = {.source = source,
					   .source_extent = source_extent,
					   .destination = destination,
					   .destination_extent = destination_extent,
					   .region = region};
	run_rows_in_parallel(region.y, region.height, region.width, copy_rows, &work);
}

static guint divide_255(guint value) {
	return (value + 1 + (value >> 8)) >> 8;
}

static void dissolve_rows(int first, int last, gpointer data) {
	const RegionWork *work = data;
	RasterExtent region = work->region, source_extent = work->source_extent, target = work->destination_extent;
	gint32 threshold = (gint32)(work->opacity * 16777215) & 16777215, scale = (gint32)floor(work->opacity * 257 + .5);
	for (int y = first; y < last; y++)
		for (int x = region.x; x < region.x + region.width; x++) {
			gsize from = (gsize)(y - source_extent.y) * source_extent.width + x - source_extent.x;
			gsize to = (gsize)(y - target.y) * target.width + x - target.x;
			gint32 hash = hash_dissolve_position(from);
			gboolean kept = work->mask ? (hash & 65535) < (gint32)sample_mask(work->mask, x, y) * scale
									   : (hash & 16777215) < threshold;
			if (kept)
				write_rgba_pixel(work->destination, to, read_rgba_pixel(work->source, from));
		}
}

static void interpolate_rows(int first, int last, gpointer data) {
	const RegionWork *work = data;
	RasterExtent region = work->region, source_extent = work->source_extent, target = work->destination_extent;
	const Record *source = work->source;
	Record *destination = work->destination;
	for (int y = first; y < last; y++)
		for (int x = region.x; x < region.x + region.width; x++) {
			gsize from = (gsize)(y - source_extent.y) * source_extent.width + x - source_extent.x;
			gsize to = (gsize)(y - target.y) * target.width + x - target.x;
			guint factor = (guint)(work->opacity * 255) & 255;
			if (work->mask) {
				factor = (guint)(sample_mask(work->mask, x, y) * work->opacity) & 255;
				if (!factor)
					continue;
				if (factor == 255) {
					write_rgba_pixel(destination, to, read_rgba_pixel(source, from));
					continue;
				}
			}
			guint32 foreground = read_rgba_pixel(source, from), background = read_rgba_pixel(destination, to);
			guint alpha = divide_255(factor * (foreground >> 24) + (255 - factor) * (background >> 24));
			guint upper = divide_255((foreground >> 24) * factor);
			guint lower = divide_255((background >> 24) * (255 - factor));
			guint32 pixel = alpha << 24;
			for (guint shift = 0; shift <= 16; shift += 8) {
				guint sum = divide_255(upper * ((foreground >> shift) & 255) + lower * ((background >> shift) & 255));
				guint channel = alpha ? (guint)floor(sum * 255.0 / alpha + .5) & 255 : 0;
				pixel |= channel << shift;
			}
			write_rgba_pixel(destination, to, pixel);
		}
}

static void interpolate_region(const Record *source, RasterExtent source_extent, Record *destination,
							   RasterExtent destination_extent, const MaskSampler *mask, RasterExtent clip,
							   double opacity, gboolean dissolve) {
	RasterExtent region = intersect_region(intersect_region(source_extent, destination_extent), clip);
	if (empty_extent(region))
		return;
	RegionWork work = {.source = source,
					   .source_extent = source_extent,
					   .destination = destination,
					   .destination_extent = destination_extent,
					   .region = region,
					   .mask = mask,
					   .opacity = opacity};
	run_rows_in_parallel(region.y, region.height, region.width, dissolve ? dissolve_rows : interpolate_rows, &work);
}

typedef struct {
	BlendOptions options;
	double ranges[40];
} LayerBlend;

static void read_layer_blend(const Record *layer, LayerBlend *blend) {
	Record *descriptors = get_record(layer, "add"), *ranges = get_record(layer, "hu");
	double values[40];
	for (guint index = 0; index < 40; index++)
		values[index] = get_item(ranges, index)->value.number;
	blend->options =
		(BlendOptions){has_record(descriptors, "iOpa") ? get_number(descriptors, "iOpa") / 255 : 1, NULL, FALSE, FALSE};
	if (prepare_blend_ranges(values, blend->ranges))
		blend->options.ranges = blend->ranges;
}

static void copy_colors_rows(int first, int last, gpointer data) {
	const RegionWork *work = data;
	RasterExtent region = work->region, source_extent = work->source_extent, target = work->destination_extent;
	for (int y = first; y < last; y++) {
		const guint8 *from = (const guint8 *)work->source->value.buffer.data +
							 ((gsize)(y - source_extent.y) * source_extent.width + region.x - source_extent.x) * 4;
		guint8 *to = (guint8 *)work->destination->value.buffer.data +
					 ((gsize)(y - target.y) * target.width + region.x - target.x) * 4;
		for (int x = 0; x < region.width; x++, from += 4, to += 4)
			if (from[3])
				memcpy(to, from, 3);
	}
}

static void blend_rows(int first, int last, gpointer data) {
	const RegionWork *work = data;
	RasterExtent region = work->region, source_extent = work->source_extent, target = work->destination_extent;
	const BlendKernel *kernel = work->kernel;
	for (int y = first; y < last; y++)
		for (int x = region.x; x < region.x + region.width; x++) {
			gsize from = (gsize)(y - source_extent.y) * source_extent.width + x - source_extent.x;
			gsize to = (gsize)(y - target.y) * target.width + x - target.x;
			const guint8 *top = (const guint8 *)work->source->value.buffer.data + from * 4;
			if (work->plain && !top[3])
				continue;
			if (work->plain && top[3] == 255) {
				memcpy((guint8 *)work->destination->value.buffer.data + to * 4, top, 4);
				continue;
			}
			write_rgba_pixel(work->destination, to,
							 kernel->blend(kernel, read_rgba_pixel(work->source, from),
										   read_rgba_pixel(work->destination, to), (guint32)from));
		}
}

static void blend_region(const Record *source, RasterExtent source_extent, Record *destination,
						 RasterExtent destination_extent, RasterExtent clip, const char *mode, double opacity,
						 const BlendOptions *options) {
	RasterExtent region = intersect_region(intersect_region(source_extent, destination_extent), clip);
	if (empty_extent(region))
		return;
	BlendKernel kernel = prepare_blend_kernel(find_blend_mode(mode), opacity, *options);
	gboolean normal = find_blend_mode(mode) == BLEND_NORMAL && !options->ranges && kernel.opacity == 1;
	RegionWork work = {.source = source,
					   .source_extent = source_extent,
					   .destination = destination,
					   .destination_extent = destination_extent,
					   .region = region,
					   .kernel = &kernel,
					   .plain = normal && !options->preserve_alpha};
	run_rows_in_parallel(region.y, region.height, region.width,
						 normal && options->preserve_alpha ? copy_colors_rows : blend_rows, &work);
}

/* d6.oK.aw9 keeps alpha outside the mask only when the outside value is opaque */
static void apply_layer_mask(const guint8 *values, RasterExtent mask_extent, guint outside, Record *shape,
							 RasterExtent shape_extent, RasterExtent area) {
	RasterExtent region = intersect_region(mask_extent, shape_extent);
	area = intersect_region(area, shape_extent);
	for (int y = area.y; y < area.y + area.height; y++)
		for (int x = area.x; x < area.x + area.width; x++) {
			gsize to = ((gsize)(y - shape_extent.y) * shape_extent.width + x - shape_extent.x) * 4 + 3;
			gboolean inside =
				x >= region.x && y >= region.y && x < region.x + region.width && y < region.y + region.height;
			if (!inside) {
				if (outside != 255)
					write_channel(shape, to, 0);
				continue;
			}
			guint value = values[(gsize)(y - mask_extent.y) * mask_extent.width + x - mask_extent.x];
			write_channel(shape, to, divide_255(value * (guint)read_channel(shape, to)));
		}
}

static void compose_tree(Composition *composition, LayerTree *tree, Record *destination, RasterExtent extent,
						 RasterExtent clip, GPtrArray *clipped);

/* Sets every channel, or only alpha, of the area's pixels in a buffer laid out over extent */
static void fill_region(Record *buffer, RasterExtent extent, RasterExtent area, guint8 value, gboolean alpha_only) {
	area = intersect_region(area, extent);
	for (int y = area.y; y < area.y + area.height; y++) {
		guint8 *row =
			(guint8 *)buffer->value.buffer.data + ((gsize)(y - extent.y) * extent.width + area.x - extent.x) * 4;
		if (!alpha_only)
			memset(row, value, (gsize)area.width * 4);
		else
			for (int x = 0; x < area.width; x++)
				row[x * 4 + 3] = value;
	}
}

/* d6.BH skips orphan clipping records and repeats each base stack over wrap tiles */
static void compose_children(Composition *composition, LayerTree *tree, Record *destination, RasterExtent extent,
							 RasterExtent clip) {
	gboolean has_base = FALSE, wrap = get_boolean(composition->document->record, "zn");
	for (guint index = 0; index < tree->children->len && !composition->failed; index++) {
		LayerTree *child = g_ptr_array_index(tree->children, index);
		if (!get_boolean(child->layer, "jl"))
			has_base = TRUE;
		if (!has_base)
			continue;
		g_autoptr(GPtrArray) clipped = g_ptr_array_new();
		for (guint next = index + 1; next < tree->children->len; next++) {
			LayerTree *candidate = g_ptr_array_index(tree->children, next);
			if (!get_boolean(candidate->layer, "jl"))
				break;
			g_ptr_array_add(clipped, candidate);
		}
		for (int y = wrap ? -1 : 0; y < (wrap ? 2 : 1); y++)
			for (int x = wrap ? -1 : 0; x < (wrap ? 2 : 1); x++) {
				/* An adjustment already covers the canvas, so the neighbouring wrap tiles skip it */
				if ((x || y) && is_adjustment_layer(child->layer))
					continue;
				RasterExtent tile = extent, tile_clip = clip;
				tile.x += x * extent.width;
				tile.y += y * extent.height;
				tile_clip.x += x * extent.width;
				tile_clip.y += y * extent.height;
				compose_tree(composition, child, destination, tile, tile_clip, clipped);
			}
		index += clipped->len;
	}
}

static void clear_layer_cache(Record *layer) {
	Record *cache = get_record(layer, "CS");
	set_record(cache, "Hg", create_null());
	set_boolean(cache, "n3", FALSE);
	set_record(cache, "ey", create_null());
	set_boolean(cache, "nW", FALSE);
}

/* d6.as5 adjusts only inside the mask extent when the mask is black outside it */
static RasterExtent get_adjustment_region(const Record *layer, RasterExtent extent) {
	Record *mask = get_record(layer, "yy");
	return is_mask_active(layer) && !mask_outside_value(mask) ? read_extent(get_record(mask, "V")) : extent;
}

static void adjust_rows(int first, int last, gpointer data) {
	const RegionWork *work = data;
	RasterExtent area = work->region, target = work->destination_extent;
	/* Rows near each other share colors, so each worker keeps one cache over its rows */
	AdjustmentCache cache;
	cache.owner = NULL;
	for (int y = first; y < last; y++)
		apply_adjustment(work->adjustment, &cache,
						 (guint8 *)work->destination->value.buffer.data +
							 ((gsize)(y - target.y) * target.width + area.x - target.x) * 4,
						 (gsize)area.width * 4);
}

/* d6.apg copies the backdrop into the buffer and applies the layer's adjustment to all of it; natively only the
 * redrawn area is worked on, since curves change each pixel alone, except where Dissolve hashes buffer positions
 * or wrapping reads the buffer through shifted tiles */
static void copy_adjusted_backdrop(const Composition *composition, const Record *layer, const Record *backdrop,
								   RasterExtent backdrop_extent, Record *target, RasterExtent target_extent,
								   RasterExtent clip) {
	gboolean whole = !g_strcmp0(get_string(layer, "qu"), "diss") || get_boolean(composition->document->record, "zn");
	RasterExtent area = whole ? target_extent : intersect_region(target_extent, clip);
	copy_region(backdrop, backdrop_extent, target, target_extent, area);
	Record *descriptors = get_record(layer, "add");
	const char *key = find_adjustment_key(descriptors);
	g_autofree Adjustment *adjustment = g_new(Adjustment, 1);
	if (!build_adjustment(key, get_record(descriptors, key), adjustment))
		return;
	RegionWork work = {
		.destination = target, .destination_extent = target_extent, .region = area, .adjustment = adjustment};
	run_rows_in_parallel(area.y, area.height, area.width, adjust_rows, &work);
}

/* Normal at full opacity puts back the adjusted color of every pixel that is not clear, so adjusting those
 * pixels where they are gives the same bytes without the two copies */
static void adjust_visible_rows(int first, int last, gpointer data) {
	const RegionWork *work = data;
	RasterExtent area = work->region, target = work->destination_extent;
	AdjustmentCache cache;
	cache.owner = NULL;
	for (int y = first; y < last; y++) {
		guint8 *row = (guint8 *)work->destination->value.buffer.data +
					  ((gsize)(y - target.y) * target.width + area.x - target.x) * 4;
		for (int x = 0; x < area.width;) {
			while (x < area.width && !row[x * 4 + 3])
				x++;
			int start = x;
			while (x < area.width && row[x * 4 + 3])
				x++;
			if (x > start)
				apply_adjustment(work->adjustment, &cache, row + start * 4, (gsize)(x - start) * 4);
		}
	}
}

static gboolean adjust_in_place(const Record *layer, Record *destination, RasterExtent extent, RasterExtent area) {
	Record *descriptors = get_record(layer, "add");
	const char *key = find_adjustment_key(descriptors);
	g_autofree Adjustment *adjustment = g_new(Adjustment, 1);
	if (!build_adjustment(key, get_record(descriptors, key), adjustment))
		return FALSE;
	RegionWork work = {
		.destination = destination, .destination_extent = extent, .region = area, .adjustment = adjustment};
	run_rows_in_parallel(area.y, area.height, area.width, adjust_visible_rows, &work);
	return TRUE;
}

/* The adjustment branch of d6.as5 blends the adjusted backdrop back while keeping the backdrop's alpha */
static void adjust_region(Composition *composition, const Record *layer, Record *destination, RasterExtent extent,
						  RasterExtent clip, double opacity, BlendOptions options) {
	RasterExtent region = get_adjustment_region(layer, extent);
	if (find_blend_mode(get_string(layer, "qu")) == BLEND_NORMAL && !options.ranges && opacity * options.fill == 1) {
		RasterExtent area = intersect_region(intersect_region(region, extent), clip);
		if (empty_extent(area) || adjust_in_place(layer, destination, extent, area))
			return;
	}
	Record *adjusted = acquire_buffer(composition, &region, FALSE);
	if (!adjusted)
		return;
	copy_adjusted_backdrop(composition, layer, destination, extent, adjusted, region, clip);
	options.preserve_alpha = TRUE;
	blend_region(adjusted, region, destination, extent, clip, get_string(layer, "qu"), opacity, &options);
	release_buffer(composition, adjusted);
}

/* d6.as5 wraps an unclipped layer or pass group in a copy that returns through its mask */
static void compose_masked_directly(Composition *composition, LayerTree *tree, Record *destination, RasterExtent extent,
									RasterExtent clip, double opacity, const BlendOptions *options) {
	Record *layer = tree->layer, *mask = get_record(layer, "yy");
	RasterExtent target_extent = get_tree_extent(composition->document, tree);
	Record *target = acquire_buffer(composition, &target_extent, FALSE);
	if (!target)
		return;
	copy_region(destination, extent, target, target_extent, target_extent);
	if (tree->children)
		compose_children(composition, tree, target, target_extent, clip);
	else if (is_adjustment_layer(layer))
		adjust_region(composition, layer, target, target_extent, clip, opacity, *options);
	else
		blend_region(get_record(layer, "buffer"), read_extent(get_record(layer, "V")), target, target_extent, clip,
					 get_string(layer, "qu"), opacity, options);
	if (composition->failed)
		return;
	g_autofree guint8 *values = create_mask_values(mask);
	MaskSampler sampler = create_mask_sampler(values, get_record(mask, "C")->value.buffer.length,
											  read_extent(get_record(mask, "V")), mask_outside_value(mask));
	interpolate_region(target, target_extent, destination, extent, &sampler, clip, 1,
					   !g_strcmp0(get_string(layer, "qu"), "diss"));
	release_buffer(composition, target);
}

/* This is d6.as5's unfeathered pixel mask and effect-free raster branch for every blend mode */
static void compose_layer(Composition *composition, LayerTree *tree, Record *destination, RasterExtent extent,
						  RasterExtent clip, GPtrArray *clipped, gboolean isolate_opacity) {
	Record *layer = tree->layer;
	const char *mode = get_string(layer, "qu");
	gboolean group = tree->children != NULL, pass = !g_strcmp0(mode, "pass"), adjustment = is_adjustment_layer(layer);
	LayerBlend blend;
	read_layer_blend(layer, &blend);
	double fill = blend.options.fill;
	double opacity = isolate_opacity ? 1 : get_number(layer, "nv") / 255;
	guint clipped_count = clipped ? clipped->len : 0;
	gboolean masked = is_mask_active(layer), dissolve = !g_strcmp0(mode, "diss");
	if ((group && pass && !blend.options.ranges && !clipped_count && fill == 1) || (!group && !clipped_count)) {
		if (masked)
			compose_masked_directly(composition, tree, destination, extent, clip, opacity, &blend.options);
		else if (group)
			compose_children(composition, tree, destination, extent, clip);
		else if (adjustment)
			adjust_region(composition, layer, destination, extent, clip, opacity, blend.options);
		else
			blend_region(get_record(layer, "buffer"), read_extent(get_record(layer, "V")), destination, extent, clip,
						 mode, opacity, &blend.options);
		clear_layer_cache(layer);
		return;
	}
	gboolean masked_pass = group && pass && (clipped_count || fill != 1);
	RasterExtent region = group		   ? get_tree_extent(composition->document, tree)
						  : adjustment ? get_adjustment_region(layer, extent)
									   : read_extent(get_record(layer, "V"));
	Record *group_pixels = NULL;
	if (group) {
		if ((gint64)region.width * region.height > (gint64)clip.width * clip.height)
			region = intersect_region(region, (RasterExtent){0, 0, document_width(composition->document),
															 document_height(composition->document)});
		group_pixels = acquire_buffer(composition, &region, TRUE);
		if (!group_pixels)
			return;
		if (group_pixels->value.buffer.length)
			memset(group_pixels->value.buffer.data, 0, group_pixels->value.buffer.length);
		compose_children(composition, tree, group_pixels, region, intersect_region(region, clip));
	}
	Record *shape = acquire_buffer(composition, &region, TRUE);
	if (!shape || composition->failed)
		return;
	RasterExtent work = get_boolean(composition->document->record, "zn") ? region : intersect_region(region, clip);
	/* An adjustment's shape is opaque everywhere before its mask */
	if (adjustment)
		fill_region(shape, region, work, 255, FALSE);
	else
		copy_region(group ? group_pixels : get_record(layer, "buffer"), region, shape, region,
					group ? intersect_region(region, clip) : work);
	if (group && !masked_pass)
		release_buffer(composition, group_pixels);
	if (masked) {
		Record *mask = get_record(layer, "yy");
		g_autofree guint8 *values = create_mask_values(mask);
		apply_layer_mask(values, read_extent(get_record(mask, "V")), mask_outside_value(mask), shape, region, work);
	}
	RasterExtent foreground_extent = region;
	Record *foreground = acquire_buffer(composition, &foreground_extent, FALSE);
	if (!foreground)
		return;
	if (group) {
		fill_region(foreground, foreground_extent, work, 0, FALSE);
		if (pass)
			copy_region(destination, extent, foreground, foreground_extent, work);
		/* The CPU branch of d6.oK.R_ is empty, so the pass group copy is only released */
		if (masked_pass)
			release_buffer(composition, group_pixels);
		compose_children(composition, tree, foreground, foreground_extent, clip);
	} else if (adjustment)
		copy_adjusted_backdrop(composition, layer, destination, extent, foreground, foreground_extent, clip);
	else
		copy_region(get_record(layer, "buffer"), region, foreground, foreground_extent, clip);
	fill_region(foreground, foreground_extent, work, 255, TRUE);
	for (guint index = 0; index < clipped_count; index++)
		compose_tree(composition, g_ptr_array_index(clipped, index), foreground, foreground_extent, clip, NULL);
	RasterExtent blended_extent = region;
	Record *blended = acquire_buffer(composition, &blended_extent, FALSE);
	if (!blended || composition->failed)
		return;
	copy_region(destination, extent, blended, blended_extent, clip);
	blend.options.preserve_alpha = adjustment;
	blend_region(foreground, foreground_extent, blended, blended_extent, clip, pass ? "norm" : mode, 1, &blend.options);
	release_buffer(composition, foreground);
	MaskSampler sampler = create_mask_sampler(shape->value.buffer.data, shape->value.buffer.length, region, 0);
	interpolate_region(blended, blended_extent, destination, extent, &sampler, clip, 1, dissolve);
	clear_layer_cache(layer);
	release_buffer(composition, shape);
	release_buffer(composition, blended);
}

static void compose_tree(Composition *composition, LayerTree *tree, Record *destination, RasterExtent extent,
						 RasterExtent clip, GPtrArray *clipped) {
	if (composition->failed)
		return;
	if (!tree->children && composition->selection) {
		const Record *selection = composition->selection;
		gboolean included = selection->type == RECORD_NUMBER && tree->index <= selection->value.number;
		for (guint index = 0; index < count_records(selection); index++)
			if (get_item(selection, index)->value.number == tree->index)
				included = TRUE;
		if (!included)
			return;
	}
	if (!is_layer_visible(tree->layer))
		return;
	if (is_mask_active(tree->layer) && empty_extent(read_extent(get_record(get_record(tree->layer, "yy"), "V"))) &&
		!mask_outside_value(get_record(tree->layer, "yy")))
		return;
	RasterExtent bounds = get_tree_extent(composition->document, tree);
	if (empty_extent(bounds))
		return;
	RasterExtent region = intersect_region(bounds, clip);
	if (!equal_region(extent, clip) && (region.y + region.height < clip.y || region.x > clip.x + clip.width ||
										region.y > clip.y + clip.height || region.x + region.width < clip.x))
		return;
	gboolean isolate = get_number(tree->layer, "nv") != 255 && (tree->children || (clipped && clipped->len));
	if (!isolate) {
		compose_layer(composition, tree, destination, extent, clip, clipped, FALSE);
		return;
	}
	Record *isolated = acquire_buffer(composition, &region, FALSE);
	if (!isolated)
		return;
	copy_region(destination, extent, isolated, region, clip);
	compose_layer(composition, tree, isolated, region, clip, clipped, TRUE);
	if (!composition->failed)
		interpolate_region(isolated, region, destination, extent, NULL, clip, get_number(tree->layer, "nv") / 255,
						   !g_strcmp0(get_string(tree->layer, "qu"), "diss"));
	release_buffer(composition, isolated);
}

/* The ported composition covers eight-bit documents without channels, quick mask, timelines or document
 * descriptors, when all their layers are supported */
static gboolean can_compose_document(const Document *document, const Record *selection) {
	Record *record = document->record, *layers = get_record(record, "Z"), *descriptors = get_record(record, "add");
	if (!descriptors || descriptors->type != RECORD_OBJECT || get_number(record, "depth") != 8 || !document->root ||
		g_hash_table_size(descriptors->value.object) || count_records(get_record(record, "J8")) ||
		count_records(get_record(record, "In")) || has_record(record, "vA"))
		return FALSE;
	for (guint index = 0; index < count_records(layers); index++)
		if (!supports_composition_layer(get_item(layers, index)))
			return FALSE;
	if (selection && selection->type != RECORD_NUMBER && selection->type != RECORD_ARRAY)
		return FALSE;
	for (guint index = 0; index < count_records(selection); index++)
		if (get_item(selection, index)->type != RECORD_NUMBER)
			return FALSE;
	return TRUE;
}

Record *compose_layer_region(const Document *document, const Record *layers, RasterExtent region, GError **error) {
	if (empty_extent(region) || !can_compose_document(document, layers)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_RENDER_UNAVAILABLE);
		return NULL;
	}
	Record *buffer = create_channels((gsize)region.width * region.height * 4, 8);
	Composition composition = {document, layers,
							   g_ptr_array_new_with_free_func((GDestroyNotify)free_composition_buffer), error, FALSE};
	compose_tree(&composition, document->root, buffer, region, region, NULL);
	g_ptr_array_unref(composition.buffers);
	if (!composition.failed)
		return buffer;
	free_record(buffer);
	return NULL;
}

gboolean compose_raster_document(const Document *document, const Record *dirty, const Record *selection,
								 GError **error) {
	if (document->mode != DOCUMENT_RASTER) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_VECTOR_RENDER_UNAVAILABLE);
		return FALSE;
	}
	Record *record = document->record;
	int width, height;
	if (!get_valid_document_dimensions(document, &width, &height) || !valid_extent(dirty) ||
		!can_compose_document(document, selection))
		goto unsupported;
	RasterExtent extent = {0, 0, width, height};
	RasterExtent clip = get_boolean(record, "zn") ? extent : intersect_region(extent, read_extent(dirty));
	Record *buffer = get_record(record, "buffer");
	if (!buffer || buffer->type != RECORD_BUFFER || buffer->value.buffer.depth != 8 ||
		buffer->value.buffer.length != (gsize)width * height * 4) {
		buffer = create_channels((gsize)width * height * 4, 8);
		if (!buffer) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, TEXT_CHANNEL_ALLOCATION_FAILED);
			return FALSE;
		}
		set_record(record, "buffer", buffer);
	}
	if (empty_extent(clip))
		return TRUE;
	RegionWork clearing = {.destination = buffer, .destination_extent = extent, .region = clip};
	run_rows_in_parallel(clip.y, clip.height, clip.width, clear_rows, &clearing);
	Composition composition = {document, selection,
							   g_ptr_array_new_with_free_func((GDestroyNotify)free_composition_buffer), error, FALSE};
	compose_tree(&composition, document->root, buffer, extent, clip, NULL);
	g_ptr_array_unref(composition.buffers);
	if (composition.failed)
		return FALSE;
	set_record(record, "ab6", copy_record(dirty));
	return TRUE;
unsupported:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_RENDER_UNAVAILABLE);
	return FALSE;
}
