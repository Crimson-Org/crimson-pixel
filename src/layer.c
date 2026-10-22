#include "document.h"

static void set_null_fields(Record *record, const char *names) {
	g_auto(GStrv) fields = g_strsplit(names, " ", -1);
	for (guint index = 0; fields[index]; index++)
		set_record(record, fields[index], create_null());
}

static Record *create_numbers(const double *numbers, guint length) {
	Record *array = create_array();
	for (guint index = 0; index < length; index++)
		append_record(array, create_number(numbers[index]));
	return array;
}

static Record *create_layer_cache(void) {
	Record *cache = create_object(RECORD_LAYER_CACHE);
	set_boolean(cache, "Hg", TRUE);
	set_boolean(cache, "n3", TRUE);
	set_record(cache, "qI", create_channels(0, 8));
	set_record(cache, "aPY", create_channels(0, 8));
	set_record(cache, "pz", create_object(RECORD_PLAIN));
	set_null_fields(cache, "ey aJI aKj K7 Bw dM");
	set_boolean(cache, "nW", FALSE);
	return cache;
}

Record *create_layer_record(void) {
	Record *layer = create_object(RECORD_LAYER);
	set_null_fields(layer, "V buffer name tc kz yy E_ LB ava aj3 a9$ dp");
	set_string(layer, "qu", "norm");
	set_number(layer, "nv", 255);
	set_boolean(layer, "jl", FALSE);
	set_boolean(layer, "Xu", FALSE);
	set_record(layer, "wJ", create_numbers((double[]){79, 128, 255}, 3));
	set_number(layer, "NF", 0);
	set_number(layer, "kr", 0);
	set_boolean(layer, "nQ", TRUE);
	Record *ranges = create_array();
	for (guint index = 0; index < 10; index++) {
		append_record(ranges, create_number(0));
		append_record(ranges, create_number(0));
		append_record(ranges, create_number(255));
		append_record(ranges, create_number(255));
	}
	set_record(layer, "hu", ranges);
	set_boolean(layer, "Zd", FALSE);
	Record *modified = create_array();
	append_record(modified, create_boolean(FALSE));
	set_record(layer, "md", modified);
	set_record(layer, "add", create_object(RECORD_PLAIN));
	Record *media = create_object(RECORD_PLAIN);
	set_record(media, "list", create_array());
	set_record(media, "j", create_array());
	set_number(media, "mS", 0);
	set_number(media, "volume", 1);
	set_record(layer, "m", media);
	set_number(layer, "aoz", 30);
	set_number(layer, "A8", 0);
	set_boolean(layer, "WF", FALSE);
	set_record(layer, "CS", create_layer_cache());
	return layer;
}

Record *create_mask(unsigned depth) {
	Record *mask = create_object(RECORD_MASK);
	set_string(mask, "name", "Mask");
	set_boolean(mask, "Hl", FALSE);
	Record *color = create_object(RECORD_PLAIN);
	set_number(color, "l", 255);
	set_number(color, "f", 0);
	set_number(color, "c", 0);
	set_record(mask, "wJ", color);
	set_number(mask, "P2", 50);
	set_number(mask, "hq", 1);
	set_number(mask, "color", 255);
	set_boolean(mask, "WE", TRUE);
	set_boolean(mask, "isEnabled", TRUE);
	set_boolean(mask, "L7", FALSE);
	set_number(mask, "mU", 255);
	set_number(mask, "O1", 0);
	set_record(mask, "V", create_extent(0, 0, 0, 0));
	set_record(mask, "C", create_channels(0, depth));
	set_null_fields(mask, "M$ a2h");
	set_boolean(mask, "cQ", TRUE);
	return mask;
}

Record *create_vector_mask(void) {
	Record *mask = create_object(RECORD_VECTOR_MASK);
	set_boolean(mask, "WE", TRUE);
	set_boolean(mask, "isEnabled", TRUE);
	set_number(mask, "mU", 255);
	set_number(mask, "O1", 0);
	set_number(mask, "jP", 0);
	set_number(mask, "d2", 1);
	Record *path = create_array();
	Record *fill_rule = create_object(RECORD_PLAIN);
	set_number(fill_rule, "J", 6);
	append_record(path, fill_rule);
	Record *initial_fill = create_object(RECORD_PLAIN);
	set_number(initial_fill, "J", 8);
	set_number(initial_fill, "all", 0);
	append_record(path, initial_fill);
	set_record(mask, "h", path);
	set_record(mask, "Hh", create_numbers((double[]){-3, -3}, 2));
	set_boolean(mask, "wi", FALSE);
	set_record(mask, "E_", create_null());
	set_boolean(mask, "cQ", TRUE);
	set_record(mask, "j", create_array());
	set_record(mask, "sW", create_array());
	return mask;
}

static Record *copy_geometry(const Record *record) {
	if (record->class == RECORD_POINT)
		return create_point(get_number(record, "x"), get_number(record, "y"));
	if (record->class == RECORD_EXTENT)
		return create_extent(get_number(record, "x"), get_number(record, "y"), get_number(record, "s"),
							 get_number(record, "T"));
	if (record->class == RECORD_TRANSFORM)
		return create_transform(get_number(record, "_"), get_number(record, "c"), get_number(record, "gy"),
								get_number(record, "kv"), get_number(record, "$q"), get_number(record, "hl"));
	return copy_record(record);
}

static void copy_fields(Record *destination, const Record *source, const char *names, gboolean json) {
	g_auto(GStrv) fields = g_strsplit(names, " ", -1);
	for (guint index = 0; fields[index]; index++) {
		Record *value = get_record(source, fields[index]);
		if (value)
			set_record(destination, fields[index], json ? copy_json_record(value) : copy_geometry(value));
	}
}

Record *copy_mask(const Record *mask) {
	Record *copy = create_mask(8);
	copy_fields(copy, mask, "name Hl P2 hq color WE isEnabled L7 mU O1 V C", FALSE);
	set_record(copy, "wJ", reference_record(get_record(mask, "wJ")));
	return copy;
}

Record *copy_path_records(const Record *path) {
	Record *copy = create_array();
	for (guint index = 0; index < count_records(path); index++) {
		Record *item = get_item(path, index);
		double kind = get_number(item, "J");
		if (kind > 5 || kind == 0 || kind == 3) {
			append_record(copy, copy_json_record(item));
		} else {
			Record *knot = create_object(RECORD_PLAIN);
			copy_fields(knot, item, "J Ck P z_", FALSE);
			append_record(copy, knot);
		}
	}
	return copy;
}

Record *copy_vector_mask(const Record *mask) {
	Record *copy = create_vector_mask();
	copy_fields(copy, mask, "WE isEnabled mU O1 jP d2 wi", FALSE);
	set_record(copy, "Hh", copy_array_shallow(get_record(mask, "Hh")));
	set_record(copy, "j", copy_array_shallow(get_record(mask, "j")));
	set_record(copy, "sW", copy_array_shallow(get_record(mask, "sW")));
	set_record(copy, "h", copy_path_records(get_record(mask, "h")));
	return copy;
}

Record *copy_descriptor(const char *key, const Record *descriptor) {
	if (g_str_equal(key, "vmsk"))
		return copy_vector_mask(descriptor);
	if (g_str_equal(key, "fxrp"))
		return copy_geometry(descriptor);
	if (!g_str_equal(key, "TySh"))
		return copy_json_record(descriptor);
	Record *copy = create_object(RECORD_PLAIN);
	copy_fields(copy, descriptor, "q V5", FALSE);
	copy_fields(copy, descriptor, "ai hp lR", TRUE);
	if (has_record(descriptor, "add")) {
		Record *extra = get_record(descriptor, "add");
		Record *copy_extra = create_object(RECORD_PLAIN);
		set_record(copy_extra, "vmsk", copy_vector_mask(get_record(extra, "vmsk")));
		copy_fields(copy_extra, extra, "vogk", TRUE);
		set_record(copy, "add", copy_extra);
	}
	return copy;
}

Record *copy_layer_record(const Record *layer) {
	Record *copy = create_layer_record();
	copy_fields(copy, layer, "V buffer qu nv jl NF name hu", FALSE);
	copy_fields(copy, layer, "m", TRUE);
	if (has_record(layer, "yy"))
		set_record(copy, "yy", copy_mask(get_record(layer, "yy")));
	if (has_record(layer, "E_"))
		set_record(copy, "E_", copy_mask(get_record(layer, "E_")));
	Record *descriptors = get_record(layer, "add");
	GHashTableIter iterator;
	gpointer key, value;
	g_hash_table_iter_init(&iterator, descriptors->value.object);
	while (g_hash_table_iter_next(&iterator, &key, &value))
		set_record(get_record(copy, "add"), key, copy_descriptor(key, value));
	return copy;
}

void name_layer(Record *layer, const char *name) {
	set_string(layer, "name", name);
	set_string(get_record(layer, "add"), "luni", name);
}

const char *get_layer_name(const Record *layer) {
	const char *name = get_string(get_record(layer, "add"), "luni");
	return name && *name ? name : get_string(layer, "name");
}

gboolean is_layer_visible(const Record *layer) {
	return ((guint32)get_number(layer, "NF") & 2) == 0;
}

gboolean is_layer_group(const Record *layer) {
	double section = get_number(get_record(layer, "add"), "lsct");
	return section == 1 || section == 2;
}

void set_layer_visible(Record *layer, gboolean visible) {
	double flags = get_number(layer, "NF");
	if (visible && !is_layer_visible(layer))
		flags -= 2;
	if (!visible && is_layer_visible(layer))
		flags += 2;
	set_number(layer, "NF", flags);
}
