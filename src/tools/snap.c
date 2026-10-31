#include <math.h>

#include "snap.h"

void initialize_snap_settings(SnapSettings *settings) {
	*settings = (SnapSettings){TRUE, {TRUE, TRUE, FALSE, TRUE, TRUE}};
}

static gboolean is_excluded(const GArray *excluded, int index) {
	for (guint item = 0; excluded && item < excluded->len; item++)
		if (g_array_index(excluded, int, item) == index)
			return TRUE;
	return FALSE;
}

static void consider(double candidate, double value, double *best) {
	if (fabs(candidate - value) < fabs(*best - value))
		*best = candidate;
}

static double snap_value(Document *document, const SnapSettings *settings, double value, int axis, double zoom,
						 const GArray *excluded, const RasterExtent *extra, int extra_count, double *source_x,
						 double *source_y, gboolean *has_source) {
	double layer_best = 1e9, best = 1e9;
	*has_source = FALSE;
	Record *layers = get_record(document->record, "Z");
	if (settings->targets[SNAP_LAYERS])
		for (guint index = 0; index < count_records(layers); index++) {
			Record *layer = get_item(layers, index);
			if (is_layer_group(layer) || !is_layer_visible(layer) || is_excluded(excluded, index))
				continue;
			RasterExtent extent = read_extent(get_record(layer, "V"));
			if (empty_extent(extent))
				continue;
			double before = layer_best;
			int start = axis ? extent.y : extent.x, size = axis ? extent.height : extent.width;
			consider(start, value, &layer_best);
			consider(start + (size >> 1), value, &layer_best);
			consider(start + size, value, &layer_best);
			if (before != layer_best) {
				*source_x = extent.x + (extent.width >> 1);
				*source_y = extent.y + (extent.height >> 1);
				*has_source = TRUE;
			}
		}
	for (int index = 0; index < extra_count; index++) {
		int start = axis ? extra[index].y : extra[index].x, size = axis ? extra[index].height : extra[index].width;
		consider(start, value, &best);
		consider(start + (size >> 1), value, &best);
		consider(start + size, value, &best);
	}
	if (settings->targets[SNAP_BOUNDS]) {
		int size = axis ? document_height(document) : document_width(document);
		consider(0, value, &best);
		consider(size >> 1, value, &best);
		consider(size, value, &best);
	}
	if (fabs(best - value) <= fabs(layer_best - value))
		*has_source = FALSE;
	else
		best = layer_best;
	return fabs(best - value) <= 4 / zoom ? best : 1e9;
}

SnapResult snap_points(Document *document, const SnapSettings *settings, const double *xs, const double *ys, int count,
					   double zoom, const GArray *excluded, const RasterExtent *extra, int extra_count) {
	SnapResult result = {0, 0, NAN, NAN, 0, 0, FALSE, FALSE};
	if (!settings->enabled)
		return result;
	for (int axis = 0; axis < 2; axis++) {
		const double *values = axis ? ys : xs;
		double offset = 1e9;
		for (int index = 0; index < count; index++) {
			double source_x = 0, source_y = 0;
			gboolean has_source = FALSE;
			double snapped = snap_value(document, settings, values[index], axis, zoom, excluded, extra, extra_count,
										&source_x, &source_y, &has_source);
			if (snapped == 1e9 || fabs(snapped - values[index]) >= fabs(offset))
				continue;
			offset = snapped - values[index];
			if (axis) {
				result.offset_y = offset;
				result.line_y = snapped;
				result.has_source_y = has_source;
				result.source_x = has_source ? source_x : result.source_x;
			} else {
				result.offset_x = offset;
				result.line_x = snapped;
				result.has_source_x = has_source;
				result.source_y = has_source ? source_y : result.source_y;
			}
		}
	}
	return result;
}

SnapResult snap_rectangle(Document *document, const SnapSettings *settings, double x, double y, double width,
						  double height, double zoom, const GArray *excluded, const RasterExtent *extra,
						  int extra_count) {
	const double xs[3] = {x, x + round(width * .5), x + width}, ys[3] = {y, y + round(height * .5), y + height};
	return snap_points(document, settings, xs, ys, 3, zoom, excluded, extra, extra_count);
}
