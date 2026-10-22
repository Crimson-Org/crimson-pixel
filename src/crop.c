#include <math.h>
#include <string.h>

#include "crop.h"
#include "history.h"
#include "pixel-selection.h"

static const char *CROP_OPERATION = "crop";
static const CropOptions FREE_CROP = {CROP_FREE, 0, 0, 0, FALSE};

/* ============================== Box geometry ============================== */

RasterExtent get_crop_rectangle(const double quad[8], double *angle) {
	double across_x = quad[2] - quad[0], across_y = quad[3] - quad[1];
	double down_x = quad[6] - quad[0], down_y = quad[7] - quad[1];
	*angle = atan2(across_y, across_x);
	return (RasterExtent){round_like_source(quad[0]), round_like_source(quad[1]),
						  round_like_source(hypot(across_x, across_y)), round_like_source(hypot(down_x, down_y))};
}

RasterExtent constrain_crop_drag(double start_x, double start_y, double end_x, double end_y, const CropOptions *options,
								 gboolean square, gboolean centered) {
	if (options->mode == CROP_SIZE) {
		double right = round_like_source(end_x), bottom = round_like_source(end_y);
		return (RasterExtent){right - options->width, bottom - options->height, options->width, options->height};
	}
	double ratio = square ? 1 : 0;
	if (options->mode == CROP_RATIO)
		ratio = options->height / options->width;
	return constrain_rectangle_drag(start_x, start_y, end_x, end_y, ratio, centered);
}

RasterExtent get_crop_start_box(int width, int height, const CropOptions *options) {
	RasterExtent box = {0, 0, width, height};
	if (options->mode == CROP_RATIO) {
		double ratio = options->width / options->height;
		if (box.width / ratio < box.height)
			box.height = round_like_source(box.width / ratio);
		else
			box.width = round_like_source(box.height * ratio);
	}
	if (options->mode == CROP_SIZE) {
		box.width = options->width;
		box.height = options->height;
	}
	box.x = round_like_source((width - box.width) / 2.0);
	box.y = round_like_source((height - box.height) / 2.0);
	return box;
}

static void fit_turned_canvas(double angle, int width, int height, double *box_width, double *box_height) {
	double shorter = MIN(width, height), longer = MAX(width, height);
	double turn = fabs(angle - floor((angle + G_PI) / (2 * G_PI)) * 2 * G_PI);
	if (turn > G_PI / 2)
		turn = G_PI - turn;
	double scale = shorter / (longer * sin(turn) + shorter * cos(turn));
	*box_width = width * scale;
	*box_height = height * scale;
}

void get_straighten_box(int width, int height, double start_x, double start_y, double end_x, double end_y,
						double quad[8]) {
	double across = end_x - start_x, down = end_y - start_y;
	if (fabs(across) < fabs(down)) {
		end_x = start_x - down;
		end_y = start_y + across;
	}
	if (start_x > end_x) {
		double swap_x = start_x, swap_y = start_y;
		start_x = end_x;
		start_y = end_y;
		end_x = swap_x;
		end_y = swap_y;
	}
	double angle = atan2(-(end_y - start_y), end_x - start_x), box_width, box_height;
	fit_turned_canvas(angle, width, height, &box_width, &box_height);
	const double corners[8] = {0, 0, box_width, 0, box_width, box_height, 0, box_height};
	double cosine = cos(angle), sine = sin(angle);
	for (int point = 0; point < 4; point++) {
		double x = corners[point * 2] - box_width / 2, y = corners[point * 2 + 1] - box_height / 2;
		quad[point * 2] = x * cosine + y * sine + width / 2.0;
		quad[point * 2 + 1] = -x * sine + y * cosine + height / 2.0;
	}
}

static RasterExtent bound_quad(const double quad[8]) {
	double left = quad[0], top = quad[1], right = quad[0], bottom = quad[1];
	for (int point = 1; point < 4; point++) {
		left = MIN(left, quad[point * 2]);
		right = MAX(right, quad[point * 2]);
		top = MIN(top, quad[point * 2 + 1]);
		bottom = MAX(bottom, quad[point * 2 + 1]);
	}
	int x = floor(left), y = floor(top);
	return (RasterExtent){x, y, (int)ceil(right) - x, (int)ceil(bottom) - y};
}

gboolean plan_crop(const double quad[8], int width, int height, const CropOptions *options, CropPlan *plan) {
	double angle;
	RasterExtent rectangle = get_crop_rectangle(quad, &angle);
	gboolean resized = FALSE;
	if (options->mode == CROP_SIZE && (double)rectangle.width * rectangle.height != options->width * options->height) {
		rectangle.width = options->width;
		rectangle.height = options->height;
		resized = TRUE;
	}
	if (rectangle.width < 1 || rectangle.height < 1)
		return FALSE;
	Projective box = {{1, 0, 0, 0, 1, 0, 0, 0}};
	projective_from_rectangle(quad, rectangle.x, rectangle.y, rectangle.width, rectangle.height, &box);
	Projective straighten = invert_projective(box);
	const double *m = straighten.m;
	gboolean affine = fabs(m[6]) < 1e-18 && fabs(m[7]) < 1e-18, turned = fabs(atan2(m[3], m[0])) > 1e-9;
	gboolean whole = rectangle.x == 0 && rectangle.y == 0 && rectangle.width == width && rectangle.height == height;
	if (!turned && affine && !resized && whole && !options->delete_pixels)
		return FALSE;
	*plan = (CropPlan){
		rectangle, bound_quad(quad),	straighten,		  turned || !affine || resized || options->delete_pixels,
		FALSE,	   options->resolution, RESAMPLE_BILINEAR};
	plan->clips = plan->changes_layers && !(fabs(m[3] * m[1]) > 1e-9 && affine);
	return TRUE;
}

/* ============================== Document ============================== */

static gboolean is_pixel_layer(Record *layer) {
	return !is_layer_group(layer) && get_number(get_record(layer, "add"), "lsct") == 0;
}

static gboolean is_identity(Projective matrix) {
	const double identity[8] = {1, 0, 0, 0, 1, 0, 0, 0};
	for (int index = 0; index < 8; index++)
		if (fabs(matrix.m[index] - identity[index]) > 1e-12)
			return FALSE;
	return TRUE;
}

static Record *clip_layer_pixels(const Record *pixels, RasterExtent extent, RasterExtent clip, RasterExtent *kept) {
	*kept = intersect_region(extent, clip);
	unsigned depth = pixels->value.buffer.depth;
	gsize pixel_bytes = 4 * (depth / 8);
	Record *copy = create_channels((gsize)MAX(0, kept->width) * MAX(0, kept->height) * 4, depth);
	for (int row = 0; row < kept->height; row++)
		memcpy((guint8 *)copy->value.buffer.data + (gsize)row * kept->width * pixel_bytes,
			   (const guint8 *)pixels->value.buffer.data +
				   ((gsize)(kept->y - extent.y + row) * extent.width + kept->x - extent.x) * pixel_bytes,
			   (gsize)kept->width * pixel_bytes);
	return copy;
}

static Record *crop_pixel_layer(Record *layer, const CropPlan *plan) {
	Record *copy = copy_layer_record(layer);
	RasterExtent extent = read_extent(get_record(copy, "V"));
	if (plan->changes_layers && plan->clips && !empty_extent(extent)) {
		RasterExtent kept;
		set_record(copy, "buffer", clip_layer_pixels(get_record(copy, "buffer"), extent, plan->clip, &kept));
		extent = kept;
	}
	/* Native shortcut: an identity matrix keeps the pixels as they are, as resampling would */
	if (plan->changes_layers && !is_identity(plan->straighten) && !empty_extent(extent)) {
		RasterExtent result;
		Record *pixels = resample_image(get_record(copy, "buffer"), extent, plan->straighten, plan->resample, &result);
		if (pixels) {
			set_record(copy, "buffer", pixels);
			extent = result;
		}
	}
	/* A layer cut away entirely stays empty at the new canvas origin */
	if (empty_extent(extent))
		extent = (RasterExtent){plan->rectangle.x, plan->rectangle.y, 0, 0};
	set_record(copy, "V",
			   create_extent(extent.x - plan->rectangle.x, extent.y - plan->rectangle.y, extent.width, extent.height));
	return copy;
}

/* Crop swaps the whole layer list, the canvas size, the resolution and the selection, so undo and redo exchange
 * them */
static void swap_crop(Record *data, Document *document) {
	Record *record = document->record;
	const char *numbers[3] = {"s", "T", "Qf"};
	for (int index = 0; index < 3; index++) {
		double value = get_number(data, numbers[index]);
		set_number(data, numbers[index], get_number(record, numbers[index]));
		set_number(record, numbers[index], value);
	}
	Record *layers = reference_record(get_record(data, "Z")), *current = copy_array_shallow(get_record(record, "Z"));
	Record *selection = reference_record(get_record(data, "g")), *now = reference_record(get_record(record, "g"));
	set_record(data, "Z", current);
	set_record(data, "g", now);
	set_record(record, "g", selection);
	set_record(record, "buffer", create_null());
	set_boolean(record, "di", TRUE);
	replace_document_layers(document, layers, NULL);
	free_record(layers);
	document->selection_changed = TRUE;
}

static double get_crop_history_size(const Record *data) {
	return 0;
}

static const HistoryOperation crop_operation = {"crop", swap_crop, swap_crop, get_crop_history_size};

static gboolean apply_crop_plan(Document *document, const CropPlan *plan_pointer, Record *name,
								gboolean clear_selection, gboolean extended_history) {
	CropPlan plan = *plan_pointer;
	Record *record = document->record, *layers = get_record(record, "Z"), *next = create_array();
	for (guint index = 0; index < count_records(layers); index++) {
		Record *layer = get_item(layers, index);
		append_record(next, is_pixel_layer(layer) ? crop_pixel_layer(layer, &plan) : reference_record(layer));
	}
	Record *data = create_object(RECORD_PLAIN);
	set_number(data, "s", plan.rectangle.width);
	set_number(data, "T", plan.rectangle.height);
	set_number(data, "Qf", plan.resolution > 0 ? plan.resolution : get_number(record, "Qf"));
	set_record(data, "Z", next);
	/* Native choice: a selection only follows a crop that keeps the pixels' scale and direction, and the
	 * Clear selection on crop preference removes it in the same step so undo restores it */
	Record *selection = get_record(record, "g"), *moved;
	if (!clear_selection && selection && selection->type == RECORD_OBJECT && is_identity(plan.straighten)) {
		RasterExtent extent = read_extent(get_record(selection, "V"));
		moved = create_object(RECORD_PLAIN);
		set_record(moved, "C", reference_record(get_record(selection, "C")));
		set_record(
			moved, "V",
			create_extent(extent.x - plan.rectangle.x, extent.y - plan.rectangle.y, extent.width, extent.height));
	} else
		moved = create_null();
	set_record(data, "g", moved);
	Record *entry = create_history_entry(name, CROP_OPERATION, FALSE, g_get_real_time() / 1000);
	set_record(entry, "data", data);
	append_history_entry(document, entry, &crop_operation, extended_history);
	swap_crop(data, document);
	return TRUE;
}

gboolean crop_document(Document *document, const double quad[8], const CropOptions *options, gboolean clear_selection,
					   gboolean extended_history) {
	CropPlan plan;
	if (!options)
		options = &FREE_CROP;
	if (!plan_crop(quad, document_width(document), document_height(document), options, &plan))
		return FALSE;
	return apply_crop_plan(document, &plan, create_history_name(10, 2), clear_selection, extended_history);
}

gboolean resize_image(Document *document, int width, int height, double resolution, ResampleMode mode,
					  gboolean extended_history) {
	int old_width = document_width(document), old_height = document_height(document);
	if (width < 1 || height < 1)
		return FALSE;
	if (width == old_width && height == old_height &&
		(resolution <= 0 || resolution == get_number(document->record, "Qf")))
		return FALSE;
	CropPlan plan = {{0, 0, width, height},
					 {0, 0, width, height},
					 {{(double)width / old_width, 0, 0, 0, (double)height / old_height, 0, 0, 0}},
					 TRUE,
					 FALSE,
					 resolution,
					 mode};
	return apply_crop_plan(document, &plan, create_history_name(11, 10), FALSE, extended_history);
}

gboolean resize_canvas(Document *document, int width, int height, int anchor, gboolean extended_history) {
	int old_width = document_width(document), old_height = document_height(document);
	if (width < 1 || height < 1 || (width == old_width && height == old_height))
		return FALSE;
	int spare_x = old_width - width, spare_y = old_height - height;
	RasterExtent rectangle = {0, 0, width, height};
	if (anchor % 3 == 1)
		rectangle.x = (int)round_like_source(spare_x / 2.0);
	if (anchor % 3 == 2)
		rectangle.x = spare_x;
	if (anchor / 3 == 1)
		rectangle.y = (int)round_like_source(spare_y / 2.0);
	if (anchor / 3 == 2)
		rectangle.y = spare_y;
	CropPlan plan = {rectangle, rectangle, {{1, 0, 0, 0, 1, 0, 0, 0}}, FALSE, FALSE, 0, RESAMPLE_BILINEAR};
	return apply_crop_plan(document, &plan, create_history_name(11, 3), FALSE, extended_history);
}

static gboolean differs_from_trim_color(const Record *pixels, gsize pixel, const guint8 reference[4]) {
	for (int channel = 0; channel < 4; channel++)
		if (read_channel_byte(pixels, pixel * 4 + channel) != reference[channel])
			return TRUE;
	return FALSE;
}

static gboolean is_row_plain(const Record *pixels, int width, int y, const guint8 reference[4]) {
	for (int x = 0; x < width; x++)
		if (differs_from_trim_color(pixels, (gsize)y * width + x, reference))
			return FALSE;
	return TRUE;
}

static gboolean is_column_plain(const Record *pixels, int width, int height, int x, const guint8 reference[4]) {
	for (int y = 0; y < height; y++)
		if (differs_from_trim_color(pixels, (gsize)y * width + x, reference))
			return FALSE;
	return TRUE;
}

RasterExtent find_trim_bounds(const Record *pixels, int width, int height, TrimColor color, const gboolean sides[4]) {
	guint8 reference[4] = {0};
	gsize reference_pixel = color == TRIM_TOP_LEFT ? 0 : (gsize)width * height - 1;
	for (int channel = 0; color != TRIM_TRANSPARENT && channel < 4; channel++)
		reference[channel] = read_channel_byte(pixels, reference_pixel * 4 + channel);
	int top = 0, bottom = 0, left = 0, right = 0;
	if (sides[TRIM_TOP])
		while (top < height && is_row_plain(pixels, width, top, reference))
			top++;
	if (sides[TRIM_BOTTOM])
		while (bottom < height && is_row_plain(pixels, width, height - 1 - bottom, reference))
			bottom++;
	if (sides[TRIM_LEFT])
		while (left < width && is_column_plain(pixels, width, height, left, reference))
			left++;
	if (sides[TRIM_RIGHT])
		while (right < width && is_column_plain(pixels, width, height, width - 1 - right, reference))
			right++;
	RasterExtent bounds = {left, top, width - left - right, height - top - bottom};
	return bounds.width > 0 && bounds.height > 0 ? bounds : (RasterExtent){0};
}

static gboolean change_canvas_to(Document *document, RasterExtent rectangle, int name_item, gboolean extended_history) {
	RasterExtent canvas = {0, 0, document_width(document), document_height(document)};
	if (empty_extent(rectangle) || !memcmp(&rectangle, &canvas, sizeof(rectangle)))
		return FALSE;
	CropPlan plan = {rectangle, rectangle, {{1, 0, 0, 0, 1, 0, 0, 0}}, FALSE, FALSE, 0, RESAMPLE_BILINEAR};
	Record *name = create_history_name(11, 12);
	append_record(name, create_number(name_item));
	return apply_crop_plan(document, &plan, name, FALSE, extended_history);
}

gboolean trim_document(Document *document, TrimColor color, const gboolean sides[4], gboolean extended_history) {
	Record *composite = get_record(document->record, "buffer");
	int width = document_width(document), height = document_height(document);
	if (!composite || composite->type != RECORD_BUFFER || composite->value.buffer.length < (gsize)width * height * 4)
		return FALSE;
	return change_canvas_to(document, find_trim_bounds(composite, width, height, color, sides), 0, extended_history);
}

gboolean reveal_all(Document *document, gboolean extended_history) {
	return change_canvas_to(document, get_tree_extent(document, document->root), 2, extended_history);
}
