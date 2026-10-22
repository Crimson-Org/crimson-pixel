#include <math.h>
#include <string.h>

#include "adjustments.h"
#include "color-spaces.h"
#include "descriptor.h"
#include "gradient.h"
#include "history.h"
#include "layer-commands.h"
#include "pixel-selection.h"
#include "../lang/en.h"

static const char *const ADJUSTMENT_KEYS[] = {"brit", "levl", "curv", "expA", "vibA", "hue2", "blnc", "blwh", "phfl",
											  "mixr", "clrL", "nvrt", "post", "thrs", "grdm", "selc", "rplc"};

static const struct {
	const char *key, *name;
	gboolean starts_group;
} PORTED_ADJUSTMENTS[] = {{"brit", TEXT_ADJUSTMENT_BRIGHTNESS_CONTRAST, FALSE},
						  {"levl", TEXT_ADJUSTMENT_LEVELS, FALSE},
						  {"curv", TEXT_ADJUSTMENT_CURVES, FALSE},
						  {"expA", TEXT_ADJUSTMENT_EXPOSURE, FALSE},
						  {"vibA", TEXT_ADJUSTMENT_VIBRANCE, TRUE},
						  {"hue2", TEXT_ADJUSTMENT_HUE_SATURATION, FALSE},
						  {"blnc", TEXT_ADJUSTMENT_COLOR_BALANCE, FALSE},
						  {"blwh", TEXT_ADJUSTMENT_BLACK_WHITE, FALSE},
						  {"phfl", TEXT_ADJUSTMENT_PHOTO_FILTER, FALSE},
						  {"mixr", TEXT_ADJUSTMENT_CHANNEL_MIXER, FALSE},
						  {"clrL", TEXT_ADJUSTMENT_COLOR_LOOKUP, FALSE},
						  {"nvrt", TEXT_ADJUSTMENT_INVERT, TRUE},
						  {"post", TEXT_ADJUSTMENT_POSTERIZE, FALSE},
						  {"thrs", TEXT_ADJUSTMENT_THRESHOLD, FALSE},
						  {"grdm", TEXT_ADJUSTMENT_GRADIENT_MAP, FALSE},
						  {"selc", TEXT_ADJUSTMENT_SELECTIVE_COLOR, FALSE},
						  {"rplc", TEXT_ADJUSTMENT_REPLACE_COLOR, FALSE}};

static const char *const BALANCE_TONES[] = {"ShdL", "MdtL", "HghL"};
static const char *const MIXER_OUTPUTS[] = {"Rd", "Grn", "Bl"};
static const char *const MIXER_INPUTS[] = {"Rd", "Grn", "Bl", NULL, "Cnst"};

const char *const GRAY_WEIGHTS[6] = {"Rd", "Yllw", "Grn", "Cyn", "Bl", "Mgnt"};

static const double HUE_RANGE_LIMITS[6][4] = {{315, 345, 15, 45},	{15, 45, 75, 105},	  {75, 105, 135, 165},
											  {135, 165, 195, 225}, {195, 225, 255, 285}, {255, 285, 315, 345}};

static const char *const CHANNEL_NAMES[] = {"Cmps", "Rd", "Grn", "Bl"};

static const char *const SELECTIVE_RANGES[] = {"Rds", "Ylws", "Grns", "Cyns", "Bls", "Mgnt", "Whts", "Ntrl", "Blks"};
static const char *const SELECTIVE_INKS[] = {"Cyn", "Mgnt", "Ylw", "Blck"};

/* ============================== Descriptors ===================================== */

const char *find_adjustment_key(const Record *descriptors) {
	for (guint index = 0; index < G_N_ELEMENTS(ADJUSTMENT_KEYS); index++)
		if (has_record(descriptors, ADJUSTMENT_KEYS[index]) &&
			get_record(descriptors, ADJUSTMENT_KEYS[index])->type != RECORD_NULL)
			return ADJUSTMENT_KEYS[index];
	return NULL;
}

gboolean is_adjustment_layer(const Record *layer) {
	return find_adjustment_key(get_record(layer, "add")) != NULL;
}

gboolean is_ported_adjustment(const char *key) {
	return get_adjustment_name(key) != NULL;
}

const char *get_ported_adjustment(guint index, gboolean *starts_group) {
	if (index >= G_N_ELEMENTS(PORTED_ADJUSTMENTS))
		return NULL;
	*starts_group = PORTED_ADJUSTMENTS[index].starts_group;
	return PORTED_ADJUSTMENTS[index].key;
}

const char *get_adjustment_name(const char *key) {
	for (guint index = 0; key && index < G_N_ELEMENTS(PORTED_ADJUSTMENTS); index++)
		if (!strcmp(key, PORTED_ADJUSTMENTS[index].key))
			return PORTED_ADJUSTMENTS[index].name;
	return NULL;
}

double get_descriptor_number(const Record *descriptor, const char *key, double fallback) {
	Record *value = get_record(get_record(descriptor, key), "v");
	return value && value->type == RECORD_NUMBER ? value->value.number : fallback;
}

void set_descriptor_number(Record *descriptor, const char *key, const char *type, double value) {
	set_record(descriptor, key, create_typed_value(type, create_number(value)));
}

gboolean get_descriptor_boolean(const Record *descriptor, const char *key, gboolean fallback) {
	Record *value = get_record(get_record(descriptor, key), "v");
	return value && value->type == RECORD_BOOLEAN ? value->value.boolean : fallback;
}

void set_descriptor_boolean(Record *descriptor, const char *key, gboolean value) {
	set_record(descriptor, key, create_typed_value("bool", create_boolean(value)));
}

static Record *create_enum_value(const char *key, const char *value) {
	Record *kind = create_object(RECORD_PLAIN);
	set_string(kind, key, value);
	return create_typed_value("enum", kind);
}

static Record *create_preset_kind(const char *type) {
	return create_enum_value("presetKindType", type);
}

static Record *create_percent(double value) {
	Record *percent = create_object(RECORD_PLAIN);
	set_string(percent, "type", "#Prc");
	set_number(percent, "val", value);
	return create_typed_value("UntF", percent);
}

static Record *create_long_list(int count) {
	Record *list = create_array();
	for (int index = 0; index < count; index++)
		append_record(list, create_typed_value("long", create_number(0)));
	return create_typed_value("VlLs", list);
}

static Record *create_mixer_channel(const double values[5]) {
	Record *channel = create_object(RECORD_PLAIN);
	set_string(channel, "classID", "ChMx");
	for (int input = 0; input < 5; input++) {
		if (MIXER_INPUTS[input])
			set_record(channel, MIXER_INPUTS[input], create_percent(values[input]));
	}
	return create_typed_value("Objc", channel);
}

Record *create_adjustment_descriptor(const char *key) {
	Record *descriptor = create_object(RECORD_PLAIN);
	if (!strcmp(key, "brit")) {
		set_string(descriptor, "classID", "BrgC");
		set_descriptor_number(descriptor, "Brgh", "long", 0);
		set_descriptor_number(descriptor, "Cntr", "long", 0);
		set_descriptor_boolean(descriptor, "useLegacy", FALSE);
	} else if (!strcmp(key, "vibA")) {
		set_string(descriptor, "classID", "vibrance");
		set_descriptor_number(descriptor, "vibrance", "long", 0);
		set_descriptor_number(descriptor, "Strt", "long", 0);
	} else if (!strcmp(key, "hue2")) {
		set_string(descriptor, "classID", "HStr");
		set_record(descriptor, "presetKind", create_preset_kind("presetKindCustom"));
		set_descriptor_boolean(descriptor, "Clrz", FALSE);
		set_record(descriptor, "Adjs", create_typed_value("VlLs", create_array()));
	} else if (!strcmp(key, "blnc")) {
		set_string(descriptor, "classID", "ClrB");
		for (int tone = 0; tone < 3; tone++)
			set_record(descriptor, BALANCE_TONES[tone], create_long_list(3));
		set_descriptor_boolean(descriptor, "PrsL", TRUE);
	} else if (!strcmp(key, "mixr")) {
		set_string(descriptor, "classID", "ChnM");
		set_record(descriptor, "presetKind", create_preset_kind("presetKindDefault"));
		set_descriptor_boolean(descriptor, "Mnch", FALSE);
		for (int output = 0; output < 3; output++) {
			Record *channel = create_object(RECORD_PLAIN);
			set_string(channel, "classID", "ChMx");
			set_record(channel, MIXER_OUTPUTS[output], create_percent(100));
			set_record(descriptor, MIXER_OUTPUTS[output], create_typed_value("Objc", channel));
		}
	} else if (!strcmp(key, "curv")) {
		set_string(descriptor, "classID", "Crvs");
		set_record(descriptor, "presetKind", create_preset_kind("presetKindCustom"));
		set_record(descriptor, "Adjs", create_typed_value("VlLs", create_array()));
	} else if (!strcmp(key, "blwh")) {
		set_string(descriptor, "classID", "BanW");
		set_record(descriptor, "presetKind", create_preset_kind("presetKindCustom"));
		const double weights[] = {40, 85, 204, 60, 20, 80};
		for (int index = 0; index < 6; index++)
			set_descriptor_number(descriptor, GRAY_WEIGHTS[index], "long", weights[index]);
		set_descriptor_boolean(descriptor, "useTint", TRUE);
		set_record(descriptor, "tintColor", create_typed_value("Objc", create_rgb_descriptor(0, 0, 0)));
	} else if (!strcmp(key, "phfl")) {
		set_string(descriptor, "classID", "photoFilter");
		set_record(descriptor, "Clr", create_typed_value("Objc", create_lab_descriptor(67.06, 32, 120)));
		set_descriptor_number(descriptor, "Dnst", "long", 80);
		set_descriptor_boolean(descriptor, "PrsL", TRUE);
	} else if (!strcmp(key, "grdm")) {
		set_string(descriptor, "classID", "GrMp");
		set_record(descriptor, "gs99", create_enum_value("gradientInterpolationMethodType", "Gcls"));
		set_descriptor_boolean(descriptor, "Dthr", FALSE);
		set_descriptor_boolean(descriptor, "Rvrs", FALSE);
		set_record(
			descriptor, "Grad",
			create_typed_value("Objc", create_gradient_record(get_two_color_gradient(), GRADIENT_TWO_COLOR_NAME)));
	} else if (!strcmp(key, "clrL"))
		set_string(descriptor, "classID", "colorLookup");
	else if (!strcmp(key, "selc")) {
		set_string(descriptor, "classID", "SlcC");
		set_record(descriptor, "presetKind", create_preset_kind("presetKindCustom"));
		set_record(descriptor, "Mthd", create_enum_value("CrcM", "Rltv"));
		set_record(descriptor, "ClrC", create_typed_value("VlLs", create_array()));
	} else if (!strcmp(key, "rplc")) {
		set_string(descriptor, "classID", "RplC");
		set_descriptor_number(descriptor, "Fzns", "long", 55);
		set_record(descriptor, "Mnm", create_typed_value("Objc", create_lab_descriptor(73.1, 23.95, 8.03)));
		set_record(descriptor, "Mxm", create_typed_value("Objc", create_lab_descriptor(73, 23, 8)));
		set_descriptor_number(descriptor, "H", "long", -22);
		set_descriptor_number(descriptor, "Strt", "long", 0);
		set_descriptor_number(descriptor, "Lght", "long", 0);
	} else if (!strcmp(key, "levl")) {
		set_string(descriptor, "classID", "Lvls");
		set_record(descriptor, "presetKind", create_preset_kind("presetKindCustom"));
		set_record(descriptor, "Adjs", create_typed_value("VlLs", create_array()));
	} else if (!strcmp(key, "expA")) {
		set_string(descriptor, "classID", "Exps");
		set_record(descriptor, "presetKind", create_preset_kind("presetKindCustom"));
		set_descriptor_number(descriptor, "Exps", "doub", 0);
		set_descriptor_number(descriptor, "Ofst", "doub", 0);
		set_descriptor_number(descriptor, "gammaCorrection", "doub", 1);
	} else if (!strcmp(key, "post")) {
		set_string(descriptor, "classID", "Pstr");
		set_descriptor_number(descriptor, "Lvls", "long", 3);
	} else if (!strcmp(key, "thrs")) {
		set_string(descriptor, "classID", "Thrs");
		set_descriptor_number(descriptor, "Lvl", "long", 128);
	}
	/* Invert has no settings, and et.Bm keeps an empty object when aZ.S7 has no default */
	return descriptor;
}

static int find_channel_entry(const Record *entries, int channel) {
	for (guint index = 0; index < count_records(entries); index++) {
		Record *entry = get_record(get_item(entries, index), "v");
		Record *reference = get_record(get_item(get_record(get_record(entry, "Chnl"), "v"), 0), "v");
		const char *name = get_string(reference, "enum");
		int found = !g_strcmp0(name, "Trgt") ? 0 : -1;
		for (int known = 0; known < 4; known++)
			if (!g_strcmp0(name, CHANNEL_NAMES[known]))
				found = known;
		if (found == channel)
			return index;
	}
	return -1;
}

void get_levels_channel(const Record *descriptor, int channel, double values[5]) {
	const double defaults[5] = {0, 255, 0, 255, 100};
	memcpy(values, defaults, sizeof defaults);
	Record *entries = get_record(get_record(descriptor, "Adjs"), "v");
	int found = find_channel_entry(entries, channel);
	if (found < 0)
		return;
	Record *entry = get_record(get_item(entries, found), "v");
	Record *input = get_record(get_record(entry, "Inpt"), "v"), *output = get_record(get_record(entry, "Otpt"), "v");
	if (input) {
		values[0] = get_number(get_item(input, 0), "v");
		values[1] = get_number(get_item(input, 1), "v");
	}
	if (output) {
		values[2] = get_number(get_item(output, 0), "v");
		values[3] = get_number(get_item(output, 1), "v");
	}
	if (has_record(entry, "Gmm"))
		values[4] = round(get_descriptor_number(entry, "Gmm", 1) * 100);
}

static Record *create_long_pair(double first, double second) {
	Record *pair = create_array();
	append_record(pair, create_typed_value("long", create_number(first)));
	append_record(pair, create_typed_value("long", create_number(second)));
	return create_typed_value("VlLs", pair);
}

static Record *create_channel_entry(const char *class_name, int channel) {
	Record *entry = create_object(RECORD_PLAIN);
	set_string(entry, "classID", class_name);
	Record *reference = create_object(RECORD_PLAIN);
	set_string(reference, "classID", "Chnl");
	set_string(reference, "typeID", "Chnl");
	set_string(reference, "enum", CHANNEL_NAMES[channel]);
	Record *references = create_array();
	append_record(references, create_typed_value("Enmr", reference));
	set_record(entry, "Chnl", create_typed_value("obj ", references));
	return entry;
}

/* Replaces the channel's entry in the Adjs list or adds it, like bC.sn and ks.sn */
static void store_channel_entry(Record *descriptor, int channel, Record *entry) {
	Record *list = get_record(descriptor, "Adjs");
	if (!list || list->type != RECORD_OBJECT) {
		list = create_typed_value("VlLs", create_array());
		set_record(descriptor, "Adjs", list);
	}
	Record *entries = get_record(list, "v");
	int found = find_channel_entry(entries, channel);
	Record *item = create_typed_value("Objc", entry);
	if (found < 0)
		append_record(entries, item);
	else {
		free_record(entries->value.array->pdata[found]);
		entries->value.array->pdata[found] = item;
	}
}

void set_levels_channel(Record *descriptor, int channel, const double values[5]) {
	Record *entry = create_channel_entry("LvlA", channel);
	set_record(entry, "Inpt", create_long_pair(values[0], values[1]));
	set_record(entry, "Otpt", create_long_pair(values[2], values[3]));
	set_descriptor_number(entry, "Gmm", "doub", values[4] / 100);
	store_channel_entry(descriptor, channel, entry);
}

static GArray *create_default_points(gboolean automatic) {
	GArray *points = g_array_new(FALSE, FALSE, sizeof(CurvePoint));
	g_array_append_val(points, ((CurvePoint){0, 0, TRUE}));
	if (automatic)
		g_array_append_val(points, ((CurvePoint){128, 160, TRUE}));
	g_array_append_val(points, ((CurvePoint){255, 255, TRUE}));
	return points;
}

static GArray *read_curve_points(const Record *list) {
	GArray *points = g_array_new(FALSE, FALSE, sizeof(CurvePoint));
	for (guint index = 0; index < count_records(list); index++) {
		Record *point = get_record(get_item(list, index), "v");
		CurvePoint value = {get_descriptor_number(point, "Hrzn", NAN), get_descriptor_number(point, "Vrtc", NAN),
							get_descriptor_boolean(point, "Cnty", TRUE)};
		g_array_append_val(points, value);
	}
	return points;
}

GArray *get_curve_points(const Record *descriptor, int channel, double levels[256]) {
	Record *entries = get_record(get_record(descriptor, "Adjs"), "v");
	int found = find_channel_entry(entries, channel);
	if (found < 0) {
		Record *first = get_record(get_item(entries, 0), "v");
		if (!count_records(entries) || has_record(first, "Crv"))
			return create_default_points(FALSE);
		if (has_record(first, "autoMachineLearning") || has_record(first, "AuCo"))
			return create_default_points(TRUE);
		for (int level = 0; level < 256; level++)
			levels[level] = level;
		return NULL;
	}
	Record *entry = get_record(get_item(entries, found), "v");
	if (has_record(entry, "Crv"))
		return read_curve_points(get_record(get_record(entry, "Crv"), "v"));
	if (!has_record(entry, "Mpng"))
		return create_default_points(FALSE);
	Record *mapping = get_record(get_record(entry, "Mpng"), "v");
	for (int level = 0; level < 256; level++)
		levels[level] = get_number(get_item(mapping, level), "v");
	return NULL;
}

void set_curve_points(Record *descriptor, int channel, const CurvePoint *points, guint count) {
	Record *entry = create_channel_entry("CrvA", channel), *list = create_array();
	for (guint index = 0; index < count; index++) {
		Record *point = create_object(RECORD_PLAIN);
		set_string(point, "classID", "CrPt");
		set_descriptor_number(point, "Hrzn", "doub", points[index].input);
		set_descriptor_number(point, "Vrtc", "doub", points[index].output);
		set_descriptor_boolean(point, "Cnty", points[index].smooth);
		append_record(list, create_typed_value("Objc", point));
	}
	set_record(entry, "Crv", create_typed_value("VlLs", list));
	store_channel_entry(descriptor, channel, entry);
}

void set_curve_levels(Record *descriptor, int channel, const double levels[256]) {
	Record *entry = create_channel_entry("CrvA", channel), *list = create_array();
	for (int level = 0; level < 256; level++)
		append_record(list, create_typed_value("long", create_number(levels[level])));
	set_record(entry, "Mpng", create_typed_value("VlLs", list));
	store_channel_entry(descriptor, channel, entry);
}

static int find_selective_entry(const Record *entries, int range) {
	for (guint index = 0; index < count_records(entries); index++) {
		Record *colors = get_record(get_record(get_item(entries, index), "v"), "Clrs");
		if (!g_strcmp0(get_string(get_record(colors, "v"), "Clrs"), SELECTIVE_RANGES[range]))
			return index;
	}
	return -1;
}

void get_selective_color(const Record *descriptor, int range, double values[4]) {
	values[0] = values[1] = values[2] = values[3] = 0;
	Record *entries = get_record(get_record(descriptor, "ClrC"), "v");
	int found = find_selective_entry(entries, range);
	if (found < 0)
		return;
	Record *entry = get_record(get_item(entries, found), "v");
	for (int ink = 0; ink < 4; ink++)
		if (has_record(entry, SELECTIVE_INKS[ink]))
			values[ink] = get_number(get_record(get_record(entry, SELECTIVE_INKS[ink]), "v"), "val");
}

void set_selective_color(Record *descriptor, int range, const double values[4]) {
	Record *entry = create_object(RECORD_PLAIN), *kind = create_object(RECORD_PLAIN);
	set_string(entry, "classID", "ClrC");
	set_string(kind, "Clrs", SELECTIVE_RANGES[range]);
	set_record(entry, "Clrs", create_typed_value("enum", kind));
	for (int ink = 0; ink < 4; ink++)
		set_record(entry, SELECTIVE_INKS[ink], create_percent(values[ink]));
	Record *list = get_record(descriptor, "ClrC");
	if (!list || list->type != RECORD_OBJECT) {
		list = create_typed_value("VlLs", create_array());
		set_record(descriptor, "ClrC", list);
	}
	Record *entries = get_record(list, "v");
	int found = find_selective_entry(entries, range);
	Record *item = create_typed_value("Objc", entry);
	if (found < 0)
		append_record(entries, item);
	else {
		free_record(entries->value.array->pdata[found]);
		entries->value.array->pdata[found] = item;
	}
}

static const char *const GRADIENT_METHODS[] = {"Gcls", "Lnr", "Perc", "Smoo", "stripes"};

GradientInterpolation get_gradient_map_method(const Record *descriptor) {
	const char *method = get_string(get_record(get_record(descriptor, "gs99"), "v"), "gradientInterpolationMethodType");
	for (guint index = 0; index < G_N_ELEMENTS(GRADIENT_METHODS); index++)
		if (!g_strcmp0(method, GRADIENT_METHODS[index]))
			return index;
	return GRADIENT_CLASSIC;
}

void set_gradient_map_method(Record *descriptor, GradientInterpolation method) {
	set_record(descriptor, "gs99", create_enum_value("gradientInterpolationMethodType", GRADIENT_METHODS[method]));
}

GBytes *get_color_lookup_profile(const Record *descriptor) {
	Record *profile = get_record(get_record(descriptor, "profile"), "v");
	if (!profile || profile->type != RECORD_BUFFER || !profile->value.buffer.length)
		return NULL;
	return g_bytes_new(profile->value.buffer.data, profile->value.buffer.length);
}

const char *get_color_lookup_name(const Record *descriptor) {
	return get_string(get_record(descriptor, "Nm"), "v");
}

void set_color_lookup_file(Record *descriptor, GBytes *profile, const char *name) {
	gsize length;
	const guint8 *data = g_bytes_get_data(profile, &length);
	set_string(descriptor, "classID", "null");
	set_descriptor_boolean(descriptor, "Dthr", TRUE);
	set_record(descriptor, "Nm", create_typed_value("TEXT", create_string(name)));
	set_record(descriptor, "lookupType", create_enum_value("colorLookupType", "abstractProfile"));
	set_record(descriptor, "profile", create_typed_value("tdta", create_bytes(data, length)));
}

gboolean is_absolute_selective_color(const Record *descriptor) {
	return !g_strcmp0(get_string(get_record(get_record(descriptor, "Mthd"), "v"), "CrcM"), "Absl");
}

void set_absolute_selective_color(Record *descriptor, gboolean absolute) {
	set_record(descriptor, "Mthd", create_enum_value("CrcM", absolute ? "Absl" : "Rltv"));
}

static int find_hue_entry(const Record *entries, int range) {
	for (guint index = 0; index < count_records(entries); index++) {
		Record *entry = get_record(get_item(entries, index), "v");
		Record *local = get_record(entry, "LclR");
		if ((range == 0 && !local) || (local && get_descriptor_number(entry, "LclR", -1) == range))
			return index;
	}
	return -1;
}

void get_hue_range(const Record *descriptor, int range, double values[7]) {
	for (int field = 0; field < 4; field++)
		values[field] = range ? HUE_RANGE_LIMITS[range - 1][field] : 0;
	values[4] = values[5] = values[6] = 0;
	Record *entries = get_record(get_record(descriptor, "Adjs"), "v");
	int found = find_hue_entry(entries, range);
	if (found < 0)
		return;
	Record *entry = get_record(get_item(entries, found), "v");
	const char *fields[] = {"BgnR", "BgnS", "EndS", "EndR", "H", "Strt", "Lght"};
	for (int field = range ? 0 : 4; field < 7; field++)
		values[field] = get_descriptor_number(entry, fields[field], 0);
}

void set_hue_range(Record *descriptor, int range, const double values[7]) {
	Record *entry = create_object(RECORD_PLAIN);
	set_string(entry, "classID", "Hst2");
	set_descriptor_number(entry, "H", "long", values[4]);
	set_descriptor_number(entry, "Strt", "long", values[5]);
	set_descriptor_number(entry, "Lght", "long", values[6]);
	if (range) {
		set_descriptor_number(entry, "LclR", "long", range);
		const char *limits[] = {"BgnR", "BgnS", "EndS", "EndR"};
		for (int field = 0; field < 4; field++)
			set_descriptor_number(entry, limits[field], "long", values[field]);
	}
	Record *list = get_record(descriptor, "Adjs");
	if (!list || list->type != RECORD_OBJECT) {
		list = create_typed_value("VlLs", create_array());
		set_record(descriptor, "Adjs", list);
	}
	Record *entries = get_record(list, "v");
	int found = find_hue_entry(entries, range);
	Record *item = create_typed_value("Objc", entry);
	if (found < 0)
		append_record(entries, item);
	else {
		free_record(entries->value.array->pdata[found]);
		entries->value.array->pdata[found] = item;
	}
}

void get_color_balance(const Record *descriptor, int tone, double values[3]) {
	Record *shifts = get_record(get_record(descriptor, BALANCE_TONES[tone]), "v");
	for (int channel = 0; channel < 3; channel++)
		values[channel] = get_number(get_item(shifts, channel), "v");
}

void set_color_balance(Record *descriptor, int tone, const double values[3]) {
	Record *list = create_array();
	for (int channel = 0; channel < 3; channel++)
		append_record(list, create_typed_value("long", create_number(values[channel])));
	set_record(descriptor, BALANCE_TONES[tone], create_typed_value("VlLs", list));
}

gboolean get_channel_mixer(const Record *descriptor, double values[20]) {
	memset(values, 0, 20 * sizeof *values);
	gboolean monochrome = get_descriptor_boolean(descriptor, "Mnch", FALSE);
	for (int output = 0; output < (monochrome ? 1 : 3); output++) {
		Record *channel = get_record(get_record(descriptor, monochrome ? "Gry" : MIXER_OUTPUTS[output]), "v");
		for (int input = 0; input < 5; input++)
			if (MIXER_INPUTS[input] && has_record(channel, MIXER_INPUTS[input]))
				values[output * 5 + input] =
					get_number(get_record(get_record(channel, MIXER_INPUTS[input]), "v"), "val");
	}
	return monochrome;
}

void set_channel_mixer(Record *descriptor, gboolean monochrome, const double values[20]) {
	set_descriptor_boolean(descriptor, "Mnch", monochrome);
	if (monochrome)
		set_record(descriptor, "Gry", create_mixer_channel(values));
	else
		for (int output = 0; output < 3; output++)
			set_record(descriptor, MIXER_OUTPUTS[output], create_mixer_channel(values + output * 5));
}

/* ============================== Commands ======================================== */

gboolean add_adjustment_layer(Document *document, const char *key, gboolean extended_history, GError **error) {
	if (!is_ported_adjustment(key)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, TEXT_RENDER_UNAVAILABLE);
		return FALSE;
	}
	Record *record = document->record, *layers = get_record(record, "Z"), *selected = get_record(record, "j");
	set_boolean(record, "di", TRUE);
	Record *layer = create_document_layer(document, FALSE);
	set_number(layer, "NF", (guint)get_number(layer, "NF") | 16);
	set_record(get_record(layer, "add"), key, create_adjustment_descriptor(key));
	Record *mask = create_mask(get_number(record, "depth")), *selection = get_document_selection(document);
	if (selection) {
		set_number(mask, "color", 0);
		set_record(mask, "C", copy_record(get_record(selection, "C")));
		set_record(mask, "V", copy_record(get_record(selection, "V")));
	}
	set_record(layer, "yy", mask);
	set_number(layer, "A8", 1);
	const char *label = get_adjustment_name(key);
	g_autofree char *prefix = g_strdup_printf("%s ", label);
	g_autofree char *name = g_strdup_printf("%s%d", prefix, find_highest_suffix(layers, prefix) + 1);
	name_layer(layer, name);
	int last = count_records(selected) ? get_item(selected, count_records(selected) - 1)->value.number
									   : (int)count_records(layers) - 1;
	int position = last + 1;
	Record *anchor = get_item(layers, last);
	if (anchor && get_number(get_record(anchor, "add"), "lsct") == 1)
		position--;
	position = CLAMP(position, 0, (int)count_records(layers));
	Record *next = copy_array_shallow(layers);
	g_ptr_array_insert(next->value.array, position, layer);
	Record *data = create_layer_list_data(document, next, copy_array_shallow(selected), create_index_list(position));
	if (selection) {
		Record *selections = create_array();
		append_record(selections, reference_record(selection));
		append_record(selections, create_null());
		set_record(data, "Ra", selections);
	}
	commit_layer_list(document, data, create_history_name(6, 1), extended_history);
	return TRUE;
}

static void apply_adjustment_edit(Record *data, Document *document, gboolean redo) {
	Record *layer = get_item(get_record(document->record, "Z"), get_number(data, "z"));
	Record *descriptors = get_record(layer, "add");
	set_record(descriptors, find_adjustment_key(descriptors), copy_record(get_record(data, redo ? "zQ" : "t5")));
	set_boolean(document->record, "di", TRUE);
	invalidate_document(document);
}

static void undo_adjustment_edit(Record *data, Document *document) {
	apply_adjustment_edit(data, document, FALSE);
}

static void redo_adjustment_edit(Record *data, Document *document) {
	apply_adjustment_edit(data, document, TRUE);
}

static double get_adjustment_history_size(const Record *data) {
	return 0;
}

static const HistoryOperation adjustment_operation = {"230", undo_adjustment_edit, redo_adjustment_edit,
													  get_adjustment_history_size};

gboolean edit_adjustment_layer(Document *document, int layer_index, const Record *descriptor, gboolean extended_history,
							   GError **error) {
	Record *layer = get_item(get_record(document->record, "Z"), layer_index);
	const char *key = layer ? find_adjustment_key(get_record(layer, "add")) : NULL;
	if (!key) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, TEXT_INVALID_LAYER_SELECTION);
		return FALSE;
	}
	Record *entry = get_latest_history_entry(document), *data = get_record(entry, "data");
	if (!data || g_strcmp0(get_string(get_record(entry, "U"), "id"), "230") || !has_record(data, "t5") ||
		get_number(data, "z") != layer_index) {
		entry = create_history_entry(create_history_name(6, 39), "230", FALSE, g_get_real_time() / 1000);
		data = create_object(RECORD_PLAIN);
		set_number(data, "z", layer_index);
		set_record(data, "t5", copy_record(get_record(get_record(layer, "add"), key)));
		set_record(entry, "data", data);
		append_history_entry(document, entry, &adjustment_operation, extended_history);
	}
	set_record(data, "zQ", copy_record(descriptor));
	redo_adjustment_edit(data, document);
	return TRUE;
}
