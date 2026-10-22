#include <math.h>

#include "document.h"

Record *create_point(double x, double y) {
	Record *point = create_object(RECORD_POINT);
	set_number(point, "x", x == 0 || isnan(x) ? 0 : x);
	set_number(point, "y", y == 0 || isnan(y) ? 0 : y);
	return point;
}

Record *create_extent(double x, double y, double width, double height) {
	Record *extent = create_object(RECORD_EXTENT);
	set_number(extent, "x", x == 0 || isnan(x) ? 0 : x);
	set_number(extent, "y", y == 0 || isnan(y) ? 0 : y);
	set_number(extent, "s", width == 0 || isnan(width) ? 0 : width);
	set_number(extent, "T", height == 0 || isnan(height) ? 0 : height);
	return extent;
}

Record *create_transform(double xx, double yx, double xy, double yy, double x, double y) {
	Record *transform = create_object(RECORD_TRANSFORM);
	set_number(transform, "_", xx);
	set_number(transform, "c", yx);
	set_number(transform, "gy", xy);
	set_number(transform, "kv", yy);
	set_number(transform, "$q", x);
	set_number(transform, "hl", y);
	return transform;
}

Record *create_identity_transform(void) {
	return create_transform(1, 0, 0, 1, 0, 0);
}

void concatenate_transform(Record *transform, const Record *after) {
	double xx = get_number(transform, "_"), yx = get_number(transform, "c");
	double xy = get_number(transform, "gy"), yy = get_number(transform, "kv");
	double x = get_number(transform, "$q"), y = get_number(transform, "hl");
	double a = get_number(after, "_"), b = get_number(after, "c");
	double c = get_number(after, "gy"), d = get_number(after, "kv");
	set_number(transform, "_", xx * a + yx * c);
	set_number(transform, "c", xx * b + yx * d);
	set_number(transform, "gy", xy * a + yy * c);
	set_number(transform, "kv", xy * b + yy * d);
	set_number(transform, "$q", x * a + y * c + get_number(after, "$q"));
	set_number(transform, "hl", x * b + y * d + get_number(after, "hl"));
}

void invert_transform(Record *transform) {
	double xx = get_number(transform, "_"), yx = get_number(transform, "c");
	double xy = get_number(transform, "gy"), yy = get_number(transform, "kv");
	double x = get_number(transform, "$q"), y = get_number(transform, "hl");
	double determinant = xx * yy - yx * xy;
	set_number(transform, "_", yy / determinant);
	set_number(transform, "c", -yx / determinant);
	set_number(transform, "gy", -xy / determinant);
	set_number(transform, "kv", xx / determinant);
	set_number(transform, "$q", (xy * y - yy * x) / determinant);
	set_number(transform, "hl", (yx * x - xx * y) / determinant);
}

Record *transform_point(const Record *transform, const Record *point) {
	double x = get_number(point, "x"), y = get_number(point, "y");
	return create_point(x * get_number(transform, "_") + y * get_number(transform, "gy") + get_number(transform, "$q"),
						x * get_number(transform, "c") + y * get_number(transform, "kv") + get_number(transform, "hl"));
}

Record *intersect_extents(const Record *first, const Record *second) {
	double x = MAX(get_number(first, "x"), get_number(second, "x"));
	double y = MAX(get_number(first, "y"), get_number(second, "y"));
	double right =
		MIN(get_number(first, "x") + get_number(first, "s"), get_number(second, "x") + get_number(second, "s"));
	double bottom =
		MIN(get_number(first, "y") + get_number(first, "T"), get_number(second, "y") + get_number(second, "T"));
	return right < x || bottom < y ? create_extent(0, 0, 0, 0) : create_extent(x, y, right - x, bottom - y);
}

Record *union_extents(const Record *first, const Record *second) {
	if (get_number(first, "s") <= 0 || get_number(first, "T") <= 0)
		return copy_record(second);
	if (get_number(second, "s") <= 0 || get_number(second, "T") <= 0)
		return copy_record(first);
	double x = MIN(get_number(first, "x"), get_number(second, "x"));
	double y = MIN(get_number(first, "y"), get_number(second, "y"));
	return create_extent(
		x, y,
		MAX(get_number(first, "x") + get_number(first, "s"), get_number(second, "x") + get_number(second, "s")) - x,
		MAX(get_number(first, "y") + get_number(first, "T"), get_number(second, "y") + get_number(second, "T")) - y);
}
