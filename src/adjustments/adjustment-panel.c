#include <math.h>
#include <string.h>

#include "adjustment-kernels.h"
#include "color/color-spaces.h"
#include "curve-editor.h"
#include "psd/descriptor.h"
#include "window/editor.h"
#include "document/layer-commands.h"
#include "canvas/snapshot-area.h"
#include "lang/en.h"
#include "common/spacing.h"

/* Input black, gamma, input white, output black and output white of the shown Levels channel */
enum { LEVEL_INPUT_BLACK, LEVEL_GAMMA, LEVEL_INPUT_WHITE, LEVEL_OUTPUT_BLACK, LEVEL_OUTPUT_WHITE };

/* The most sliders one adjustment shows, which Black & White needs for its six hue ranges */
#define ADJUSTMENT_SLOTS GRAY_WEIGHTS_COUNT
#define SLIDER_WIDTH 220
/* Clicking beside a slider moves it this many steps */
#define PAGE_STEPS 10
/* The histogram shows one column per level, with the Levels markers under it */
#define HISTOGRAM_HEIGHT 112
#define MARKER_HEIGHT 8
#define MARKER_HALF_WIDTH 4
#define MARKER_BORDER 1
#define HISTOGRAM_BAR_ALPHA .75
#define GRADIENT_PREVIEW_HEIGHT 24
/* Levels keeps the input white at least this far above the input black, and the gamma marker off both ends */
#define LEVELS_GAP 2
#define GAMMA_SHARE_LIMIT .001

/* One slider: its label, range, step and decimals, the setting it edits with that setting's type, and the value the
 * setting falls back to; sliders whose values a choice picks have no setting */
typedef struct {
	const char *label;
	double minimum, maximum, step;
	int digits;
	const char *key, *type;
	double fallback;
} SliderSpec;

#define PERCENT_SLIDER(label, key) {label, -100, 100, 1, 0, key, "long", 0}
#define HUE_SLIDER(label, key) {label, -180, 180, 1, 0, key, "long", 0}
#define MIXER_SLIDER(label) {label, -200, 200, 1, 0, NULL, NULL, 0}
#define GAMMA_SLIDER(label, key, type) {label, .01, 9.99, .01, 2, key, type, 1}

static const SliderSpec BRIGHTNESS_SLIDERS[] = {{TEXT_BRIGHTNESS, -150, 150, 1, 0, "Brgh", "long", 0},
												{TEXT_CONTRAST, -50, 100, 1, 0, "Cntr", "long", 0}};
static const SliderSpec VIBRANCE_SLIDERS[] = {PERCENT_SLIDER(TEXT_VIBRANCE, "vibrance"),
											  PERCENT_SLIDER(TEXT_SATURATION, "Strt")};
static const SliderSpec EXPOSURE_SLIDERS[] = {{TEXT_ADJUSTMENT_EXPOSURE, -20, 20, .01, 2, "Exps", "doub", 0},
											  {TEXT_EXPOSURE_OFFSET, -.5, .5, .0001, 4, "Ofst", "doub", 0},
											  GAMMA_SLIDER(TEXT_EXPOSURE_GAMMA, "gammaCorrection", "doub")};
static const SliderSpec POSTERIZE_SLIDERS[] = {
	{TEXT_POSTERIZE_LEVELS, 2, G_MAXUINT8, 1, 0, "Lvls", "long", DEFAULT_POSTERIZE_LEVELS}};
static const SliderSpec THRESHOLD_SLIDERS[] = {
	{TEXT_THRESHOLD_LEVEL, 1, G_MAXUINT8, 1, 0, "Lvl", "long", DEFAULT_THRESHOLD}};
static const SliderSpec FILTER_SLIDERS[] = {{TEXT_DENSITY, 0, 100, 1, 0, "Dnst", "long", DEFAULT_FILTER_DENSITY}};
static const SliderSpec REPLACE_SLIDERS[] = {{TEXT_FUZZINESS, 0, 200, 1, 0, "Fzns", "long", DEFAULT_REPLACE_FUZZINESS},
											 HUE_SLIDER(TEXT_HUE, "H"), PERCENT_SLIDER(TEXT_SATURATION, "Strt"),
											 PERCENT_SLIDER(TEXT_LIGHTNESS, "Lght")};
/* In the order of the LEVEL slots */
static const SliderSpec LEVELS_SLIDERS[] = {{TEXT_LEVELS_INPUT_BLACK, 0, G_MAXUINT8 - LEVELS_GAP, 1, 0, NULL, NULL, 0},
											GAMMA_SLIDER(TEXT_LEVELS_GAMMA, NULL, NULL),
											{TEXT_LEVELS_INPUT_WHITE, LEVELS_GAP, G_MAXUINT8, 1, 0, NULL, NULL, G_MAXUINT8},
											{TEXT_LEVELS_OUTPUT_BLACK, 0, G_MAXUINT8, 1, 0, NULL, NULL, 0},
											{TEXT_LEVELS_OUTPUT_WHITE, 0, G_MAXUINT8, 1, 0, NULL, NULL, G_MAXUINT8}};
static const SliderSpec HUE_SLIDERS[] = {HUE_SLIDER(TEXT_HUE, NULL), PERCENT_SLIDER(TEXT_SATURATION, NULL),
										 PERCENT_SLIDER(TEXT_LIGHTNESS, NULL)};
static const SliderSpec BALANCE_SLIDERS[] = {PERCENT_SLIDER(TEXT_BALANCE_CYAN_RED, NULL),
											 PERCENT_SLIDER(TEXT_BALANCE_MAGENTA_GREEN, NULL),
											 PERCENT_SLIDER(TEXT_BALANCE_YELLOW_BLUE, NULL)};
static const SliderSpec MIXER_SLIDERS[] = {MIXER_SLIDER(TEXT_CHANNEL_RED), MIXER_SLIDER(TEXT_CHANNEL_GREEN),
										   MIXER_SLIDER(TEXT_CHANNEL_BLUE), MIXER_SLIDER(TEXT_CONSTANT)};
static const SliderSpec SELECTIVE_SLIDERS[] = {PERCENT_SLIDER(TEXT_INK_CYAN, NULL), PERCENT_SLIDER(TEXT_INK_MAGENTA, NULL),
											   PERCENT_SLIDER(TEXT_INK_YELLOW, NULL), PERCENT_SLIDER(TEXT_INK_BLACK, NULL)};
/* Black & White has one of these for each hue range, editing that range's weight */
static const SliderSpec GRAY_SLIDER = {NULL, -200, 300, 1, 0, NULL, "long", DEFAULT_GRAY_WEIGHT};
/* The mixer settings each slider edits */
static const int MIXER_PARTS_SHOWN[] = {MIXER_RED, MIXER_GREEN, MIXER_BLUE, MIXER_CONSTANT};

/* One open settings popover, editing its layer live */
typedef struct {
	Editor *editor;
	Document *document;
	/* The choice picks the Levels channel, hue range, tone or output channel the sliders show, and the option is
	 * the adjustment's switch such as Colorize */
	GtkWidget *popover, *grid, *choice, *option, *histogram;
	/* A second switch, up to two color buttons, the Curves graph, the gradient preview and the lookup file name */
	GtkWidget *second_option, *colors[2], *curve, *preview, *file_label;
	int layer_index;
	const char *key;
	Record *settings;
	GtkAdjustment *values[ADJUSTMENT_SLOTS];
	SliderSpec specs[ADJUSTMENT_SLOTS];
	int slider_count;
	/* Pixel counts of the luminance and the red, green and blue channels of the layers below */
	guint32 counts[TONE_CHANNELS][CURVE_LEVELS];
	gboolean has_histogram, updating;
	/* The pending canvas redraw, which slider moves made while a redraw ran fold into */
	guint redraw;
	int dragged_marker;
} AdjustmentPanel;

/* ============================== Histogram ======================================= */

/* The layers below the adjustment, which are what it changes */
static void count_backdrop(AdjustmentPanel *panel, Document *document) {
	if (panel->layer_index < 1)
		return;
	g_autoptr(Record) below = create_number(panel->layer_index - 1);
	RasterExtent canvas = {0, 0, document_width(document), document_height(document)};
	g_autoptr(Record) pixels = compose_layer_region(document, below, canvas, NULL);
	if (!pixels)
		return;
	const guint8 *data = pixels->value.buffer.data;
	for (gsize index = 0; index + ALPHA_CHANNEL < pixels->value.buffer.length; index += RGBA_CHANNELS) {
		if (!data[index + ALPHA_CHANNEL])
			continue;
		panel->counts[0][(int)(get_luma(data[index], data[index + 1], data[index + 2]) + .5)]++;
		for (int channel = 0; channel < RGB_CHANNELS; channel++)
			panel->counts[channel + 1][data[index + channel]]++;
	}
	panel->has_histogram = TRUE;
}

static int get_shown_channel(AdjustmentPanel *panel) {
	return panel->choice ? (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(panel->choice)) : 0;
}

/* Levels places the gamma marker where the input range maps to the middle */
static double get_marker_level(AdjustmentPanel *panel, int marker) {
	double black = gtk_adjustment_get_value(panel->values[LEVEL_INPUT_BLACK]);
	double white = gtk_adjustment_get_value(panel->values[LEVEL_INPUT_WHITE]);
	if (marker == LEVEL_INPUT_BLACK)
		return black;
	if (marker == LEVEL_INPUT_WHITE)
		return white;
	return black + (white - black) * pow(.5, gtk_adjustment_get_value(panel->values[LEVEL_GAMMA]));
}

static void draw_histogram(GtkWidget *widget, GtkSnapshot *snapshot, int width, int height, gpointer data) {
	AdjustmentPanel *panel = data;
	GdkRGBA color;
	gtk_widget_get_color(widget, &color);
	const guint32 *counts = panel->counts[!g_strcmp0(panel->key, "levl") ? get_shown_channel(panel) : 0];
	guint32 peak = 1;
	for (int level = 0; level < CURVE_LEVELS; level++)
		peak = MAX(peak, counts[level]);
	const int marker_height = panel->values[LEVEL_INPUT_BLACK] ? MARKER_HEIGHT : 0;
	double bar_height = height - marker_height, column = (double)width / CURVE_LEVELS;
	GdkRGBA bar = color;
	bar.alpha *= HISTOGRAM_BAR_ALPHA;
	for (int level = 0; level < CURVE_LEVELS; level++) {
		/* A square root scale keeps small counts visible beside large peaks */
		double size = round(sqrt((double)counts[level] / peak) * bar_height);
		gtk_snapshot_append_color(snapshot, &bar,
								  &GRAPHENE_RECT_INIT(level * column, bar_height - size, ceil(column), size));
	}
	if (!marker_height)
		return;
	const GdkRGBA shades[] = {{0, 0, 0, 1}, {.5, .5, .5, 1}, {1, 1, 1, 1}};
	for (int marker = LEVEL_INPUT_BLACK; marker <= LEVEL_INPUT_WHITE; marker++) {
		double x = byte_to_unit(get_marker_level(panel, marker)) * width;
		graphene_rect_t box = GRAPHENE_RECT_INIT(x - MARKER_HALF_WIDTH, bar_height, 2 * MARKER_HALF_WIDTH, marker_height);
		GskRoundedRect outline;
		gsk_rounded_rect_init_from_rect(&outline, &box, MARKER_BORDER);
		gtk_snapshot_push_rounded_clip(snapshot, &outline);
		gtk_snapshot_append_color(snapshot, &color, &box);
		graphene_rect_t inside;
		graphene_rect_inset_r(&box, MARKER_BORDER, MARKER_BORDER, &inside);
		gtk_snapshot_append_color(snapshot, &shades[marker], &inside);
		gtk_snapshot_pop(snapshot);
	}
}

static double get_histogram_level(GtkWidget *histogram, double x) {
	return CLAMP(unit_to_byte(x / MAX(1, gtk_widget_get_width(histogram))), 0, G_MAXUINT8);
}

static void move_marker(AdjustmentPanel *panel, double level) {
	GtkAdjustment **values = panel->values;
	if (panel->dragged_marker != LEVEL_GAMMA) {
		gtk_adjustment_set_value(values[panel->dragged_marker], round(level));
		return;
	}
	double black = gtk_adjustment_get_value(values[LEVEL_INPUT_BLACK]);
	double white = gtk_adjustment_get_value(values[LEVEL_INPUT_WHITE]);
	double share = CLAMP((level - black) / MAX(1, white - black), GAMMA_SHARE_LIMIT, 1 - GAMMA_SHARE_LIMIT);
	gtk_adjustment_set_value(values[LEVEL_GAMMA], log(share) / log(.5));
}

static void begin_marker_drag(GtkGestureDrag *gesture, double x, double y, AdjustmentPanel *panel) {
	double level = get_histogram_level(panel->histogram, x), nearest = G_MAXDOUBLE;
	for (int marker = LEVEL_INPUT_BLACK; marker <= LEVEL_INPUT_WHITE; marker++) {
		double distance = fabs(get_marker_level(panel, marker) - level);
		if (distance < nearest) {
			nearest = distance;
			panel->dragged_marker = marker;
		}
	}
	move_marker(panel, level);
}

static void update_marker_drag(GtkGestureDrag *gesture, double x, double y, AdjustmentPanel *panel) {
	double start_x, start_y;
	gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
	move_marker(panel, get_histogram_level(panel->histogram, start_x + x));
}

/* ============================== Settings ======================================== */

/* Composes the canvas again into the shown surface, which keeps its tiles instead of being rebuilt */
static void redraw_canvas(AdjustmentPanel *panel) {
	g_clear_handle_id(&panel->redraw, g_source_remove);
	Editor *editor = panel->editor;
	GtkWidget *page = get_current_page(editor);
	PresentedDocument *presented = get_page_document(page);
	if (!presented || presented->document != panel->document)
		return;
	Document *document = presented->document;
	update_document_region(editor, presented,
						   (RasterExtent){0, 0, document_width(document), document_height(document)});
	refresh_document_page(editor, page, FALSE);
}

static gboolean redraw_canvas_later(gpointer data) {
	AdjustmentPanel *panel = data;
	panel->redraw = 0;
	redraw_canvas(panel);
	return G_SOURCE_REMOVE;
}

static void apply_panel_settings(AdjustmentPanel *panel) {
	Record *layer = get_item(get_record(panel->document->record, "Z"), panel->layer_index);
	if (get_page_document(get_current_page(panel->editor))->document != panel->document || !layer ||
		g_strcmp0(find_adjustment_key(get_record(layer, "add")), panel->key)) {
		gtk_popover_popdown(GTK_POPOVER(panel->popover));
		return;
	}
	if (!edit_adjustment_layer(panel->document, panel->layer_index, panel->settings, FALSE, NULL))
		return;
	if (!panel->redraw)
		panel->redraw = g_idle_add(redraw_canvas_later, panel);
}

static void read_levels_channel(AdjustmentPanel *panel) {
	double levels[LEVELS_VALUES];
	get_levels_channel(panel->settings, get_shown_channel(panel), levels);
	const int order[LEVELS_VALUES] = {LEVEL_INPUT_BLACK, LEVEL_INPUT_WHITE, LEVEL_OUTPUT_BLACK, LEVEL_OUTPUT_WHITE,
									  LEVEL_GAMMA};
	panel->updating = TRUE;
	for (int field = 0; field < LEVELS_VALUES; field++)
		gtk_adjustment_set_value(panel->values[order[field]],
								 field == LEVELS_GAMMA ? levels[field] / LEVELS_GAMMA_SCALE : levels[field]);
	panel->updating = FALSE;
}

static void store_levels_channel(AdjustmentPanel *panel) {
	GtkAdjustment **values = panel->values;
	/* Photoshop keeps the input white at least two levels above the input black */
	double black = gtk_adjustment_get_value(values[LEVEL_INPUT_BLACK]);
	double white = gtk_adjustment_get_value(values[LEVEL_INPUT_WHITE]);
	if (white < black + LEVELS_GAP) {
		panel->updating = TRUE;
		black = MIN(black, G_MAXUINT8 - LEVELS_GAP);
		gtk_adjustment_set_value(values[LEVEL_INPUT_BLACK], black);
		gtk_adjustment_set_value(values[LEVEL_INPUT_WHITE], white = black + LEVELS_GAP);
		panel->updating = FALSE;
	}
	const double levels[LEVELS_VALUES] = {black, white, gtk_adjustment_get_value(values[LEVEL_OUTPUT_BLACK]),
										  gtk_adjustment_get_value(values[LEVEL_OUTPUT_WHITE]),
										  round(gtk_adjustment_get_value(values[LEVEL_GAMMA]) * LEVELS_GAMMA_SCALE)};
	set_levels_channel(panel->settings, get_shown_channel(panel), levels);
}

static gboolean is_option_active(AdjustmentPanel *panel) {
	return gtk_check_button_get_active(GTK_CHECK_BUTTON(panel->option));
}

/* The mixer edits its gray output while monochrome, whatever channel the list shows */
static int get_mixer_output(AdjustmentPanel *panel) {
	return is_option_active(panel) ? 0 : get_shown_channel(panel);
}

/* Curves mapping levels directly come from files and have no points to drag, so their graph cannot be dragged */
static void read_curve_channel(AdjustmentPanel *panel) {
	double levels[CURVE_LEVELS];
	int channel = get_shown_channel(panel);
	g_autoptr(GArray) points = get_curve_points(panel->settings, channel, levels);
	gtk_widget_set_sensitive(panel->curve, points != NULL);
	if (points)
		set_curve_editor_points(panel->curve, (const CurvePoint *)points->data, points->len);
	set_curve_editor_histogram(panel->curve, panel->has_histogram ? panel->counts[channel] : NULL);
}

static void change_curve(GtkWidget *curve, AdjustmentPanel *panel) {
	const GArray *points = get_curve_editor_points(curve);
	set_curve_points(panel->settings, get_shown_channel(panel), (const CurvePoint *)points->data, points->len);
	apply_panel_settings(panel);
}

static void get_button_color(GtkWidget *button, double rgb[RGB_CHANNELS]) {
	const GdkRGBA *color = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button));
	rgb[0] = round(unit_to_byte(color->red));
	rgb[1] = round(unit_to_byte(color->green));
	rgb[2] = round(unit_to_byte(color->blue));
}

static Record *create_button_lab(GtkWidget *button) {
	double rgb[RGB_CHANNELS], lab[RGB_CHANNELS];
	get_button_color(button, rgb);
	convert_rgb_to_lab(rgb[0], rgb[1], rgb[2], lab);
	return create_typed_value("Objc", create_lab_descriptor(lab[0], lab[1], lab[2]));
}

static gboolean is_second_option_active(AdjustmentPanel *panel) {
	return gtk_check_button_get_active(GTK_CHECK_BUTTON(panel->second_option));
}

/* Gradient Map's two buttons edit the colors of its first and last stops */
static void store_gradient_map(AdjustmentPanel *panel) {
	Record *record = get_record(get_record(panel->settings, "Grad"), "v");
	g_autoptr(Gradient) gradient = read_gradient_record(record);
	if (!gradient)
		return;
	GradientColorStop *stops = (GradientColorStop *)gradient->colors;
	int first = 0, last = 0;
	for (int index = 1; index < gradient->color_count; index++) {
		if (stops[index].location < stops[first].location)
			first = index;
		if (stops[index].location >= stops[last].location)
			last = index;
	}
	int ends[] = {first, last};
	for (guint end = 0; end < G_N_ELEMENTS(ends); end++) {
		double rgb[RGB_CHANNELS];
		get_button_color(panel->colors[end], rgb);
		stops[ends[end]] = (GradientColorStop){
			stops[ends[end]].location, stops[ends[end]].midpoint, GRADIENT_USER_COLOR, rgb[0], rgb[1], rgb[2]};
	}
	g_autofree char *name = g_strdup(get_string(get_record(record, "Nm"), "v"));
	set_record(panel->settings, "Grad", create_typed_value("Objc", create_gradient_record(gradient, name ? name : "")));
	set_gradient_map_method(panel->settings, get_shown_channel(panel));
	set_descriptor_boolean(panel->settings, "Rvrs", is_option_active(panel));
	set_descriptor_boolean(panel->settings, "Dthr", is_second_option_active(panel));
	gtk_widget_queue_draw(panel->preview);
}

/* Shows the values of the chosen range, tone or output channel in the sliders */
static void read_choice(AdjustmentPanel *panel) {
	double values[MIXER_VALUES];
	int count = RGB_CHANNELS;
	if (!strcmp(panel->key, "levl")) {
		read_levels_channel(panel);
		return;
	}
	if (!strcmp(panel->key, "curv")) {
		read_curve_channel(panel);
		return;
	}
	if (!strcmp(panel->key, "grdm"))
		return;
	if (!strcmp(panel->key, "selc")) {
		get_selective_color(panel->settings, get_shown_channel(panel), values);
		count = SELECTIVE_INKS_COUNT;
	} else if (!strcmp(panel->key, "hue2")) {
		get_hue_range(panel->settings, get_shown_channel(panel), values);
		memmove(values, values + HUE_SHIFT, (HUE_RANGE_VALUES - HUE_SHIFT) * sizeof *values);
	} else if (!strcmp(panel->key, "blnc"))
		get_color_balance(panel->settings, get_shown_channel(panel), values);
	else {
		double mixer[MIXER_VALUES];
		get_channel_mixer(panel->settings, mixer);
		for (guint part = 0; part < G_N_ELEMENTS(MIXER_PARTS_SHOWN); part++)
			values[part] = mixer[get_mixer_output(panel) * MIXER_INPUTS_COUNT + MIXER_PARTS_SHOWN[part]];
		count = G_N_ELEMENTS(MIXER_PARTS_SHOWN);
	}
	panel->updating = TRUE;
	for (int slot = 0; slot < count; slot++)
		gtk_adjustment_set_value(panel->values[slot], values[slot]);
	panel->updating = FALSE;
}

static void store_choice(AdjustmentPanel *panel) {
	GtkAdjustment **slots = panel->values;
	if (!strcmp(panel->key, "selc")) {
		double inks[SELECTIVE_INKS_COUNT];
		for (int ink = 0; ink < SELECTIVE_INKS_COUNT; ink++)
			inks[ink] = gtk_adjustment_get_value(slots[ink]);
		set_selective_color(panel->settings, get_shown_channel(panel), inks);
		set_absolute_selective_color(panel->settings, is_option_active(panel));
	} else if (!strcmp(panel->key, "hue2")) {
		double values[HUE_RANGE_VALUES];
		int range = get_shown_channel(panel);
		get_hue_range(panel->settings, range, values);
		for (int field = HUE_SHIFT; field < HUE_RANGE_VALUES; field++)
			values[field] = gtk_adjustment_get_value(slots[field - HUE_SHIFT]);
		set_hue_range(panel->settings, range, values);
		set_descriptor_boolean(panel->settings, "Clrz", is_option_active(panel));
	} else if (!strcmp(panel->key, "blnc")) {
		const double values[RGB_CHANNELS] = {gtk_adjustment_get_value(slots[0]), gtk_adjustment_get_value(slots[1]),
											 gtk_adjustment_get_value(slots[2])};
		set_color_balance(panel->settings, get_shown_channel(panel), values);
		set_descriptor_boolean(panel->settings, "PrsL", is_option_active(panel));
	} else {
		double values[MIXER_VALUES];
		get_channel_mixer(panel->settings, values);
		for (guint part = 0; part < G_N_ELEMENTS(MIXER_PARTS_SHOWN); part++)
			values[get_mixer_output(panel) * MIXER_INPUTS_COUNT + MIXER_PARTS_SHOWN[part]] =
				gtk_adjustment_get_value(slots[part]);
		set_channel_mixer(panel->settings, is_option_active(panel), values);
	}
}

/* Sliders that edit a setting of their own write it straight back */
static void store_keyed_sliders(AdjustmentPanel *panel) {
	for (int slot = 0; slot < panel->slider_count; slot++) {
		const SliderSpec *spec = &panel->specs[slot];
		if (spec->key)
			set_descriptor_number(panel->settings, spec->key, spec->type, gtk_adjustment_get_value(panel->values[slot]));
	}
}

static void change_value(GtkAdjustment *adjustment, AdjustmentPanel *panel) {
	if (panel->updating)
		return;
	store_keyed_sliders(panel);
	if (!strcmp(panel->key, "levl"))
		store_levels_channel(panel);
	else if (!strcmp(panel->key, "hue2") || !strcmp(panel->key, "blnc") || !strcmp(panel->key, "mixr") ||
			 !strcmp(panel->key, "selc"))
		store_choice(panel);
	else if (!strcmp(panel->key, "grdm"))
		store_gradient_map(panel);
	else if (!strcmp(panel->key, "blwh")) {
		set_descriptor_boolean(panel->settings, "useTint", is_option_active(panel));
		double rgb[RGB_CHANNELS];
		get_button_color(panel->colors[0], rgb);
		set_record(panel->settings, "tintColor",
				   create_typed_value("Objc", create_rgb_descriptor(rgb[0], rgb[1], rgb[2])));
	} else if (!strcmp(panel->key, "phfl")) {
		set_record(panel->settings, "Clr", create_button_lab(panel->colors[0]));
		set_descriptor_boolean(panel->settings, "PrsL", is_option_active(panel));
	} else if (!strcmp(panel->key, "rplc")) {
		set_record(panel->settings, "Mnm", create_button_lab(panel->colors[0]));
		set_record(panel->settings, "Mxm", create_button_lab(panel->colors[0]));
	} else if (!strcmp(panel->key, "brit"))
		set_descriptor_boolean(panel->settings, "useLegacy", is_option_active(panel));
	if (panel->histogram)
		gtk_widget_queue_draw(panel->histogram);
	apply_panel_settings(panel);
}

static void change_choice(GObject *choice, GParamSpec *property, AdjustmentPanel *panel) {
	if (panel->updating)
		return;
	read_choice(panel);
	if (!strcmp(panel->key, "grdm"))
		change_value(NULL, panel);
	if (panel->histogram)
		gtk_widget_queue_draw(panel->histogram);
}

/* Colorize works on the master values, so the sliders show them, and a monochrome mixer keeps the shown output's
 * values as its gray output */
static void toggle_option(GtkCheckButton *button, AdjustmentPanel *panel) {
	gboolean active = is_option_active(panel);
	if (!strcmp(panel->key, "hue2") || !strcmp(panel->key, "mixr")) {
		gtk_widget_set_sensitive(panel->choice, !active);
		panel->updating = !strcmp(panel->key, "mixr");
		if (active)
			gtk_drop_down_set_selected(GTK_DROP_DOWN(panel->choice), 0);
		panel->updating = FALSE;
	}
	change_value(NULL, panel);
}

static void attach_wide(AdjustmentPanel *panel, GtkWidget *widget) {
	add_grid_row(panel->grid, widget, LABEL_COLUMN);
}

/* A label, a slider and a number box sharing one value, which a setting of its own gives or a choice sets later */
static void add_slider(AdjustmentPanel *panel, const SliderSpec *spec) {
	int slot = panel->slider_count++;
	panel->specs[slot] = *spec;
	double value = spec->key ? get_descriptor_number(panel->settings, spec->key, spec->fallback) : spec->fallback;
	GtkAdjustment *adjustment =
		gtk_adjustment_new(value, spec->minimum, spec->maximum, spec->step, spec->step * PAGE_STEPS, 0);
	panel->values[slot] = adjustment;
	GtkWidget *scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adjustment);
	gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
	gtk_widget_set_size_request(scale, SLIDER_WIDTH, -1);
	gtk_widget_set_hexpand(scale, TRUE);
	add_dialog_input(panel->grid, spec->label, scale);
	GtkWidget *number = gtk_spin_button_new(adjustment, spec->step, spec->digits);
	gtk_accessible_update_property(GTK_ACCESSIBLE(number), GTK_ACCESSIBLE_PROPERTY_LABEL, spec->label, -1);
	gtk_grid_attach(GTK_GRID(panel->grid), number, EXTRA_COLUMN, get_grid_row(panel->grid), 1, 1);
	g_signal_connect(adjustment, "value-changed", G_CALLBACK(change_value), panel);
}

static void add_sliders(AdjustmentPanel *panel, const SliderSpec *specs, guint count) {
	for (guint index = 0; index < count; index++)
		add_slider(panel, &specs[index]);
}

static void change_color(GObject *button, GParamSpec *property, AdjustmentPanel *panel) {
	change_value(NULL, panel);
}

static void toggle_second_option(GtkCheckButton *button, AdjustmentPanel *panel) {
	change_value(NULL, panel);
}

static void add_color(AdjustmentPanel *panel, int slot, const char *name, const double rgb[RGB_CHANNELS]) {
	GtkWidget *button = gtk_color_dialog_button_new(gtk_color_dialog_new());
	gtk_color_dialog_set_with_alpha(gtk_color_dialog_button_get_dialog(GTK_COLOR_DIALOG_BUTTON(button)), FALSE);
	gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(button),
									 &(GdkRGBA){byte_to_unit(rgb[0]), byte_to_unit(rgb[1]), byte_to_unit(rgb[2]), 1});
	gtk_widget_set_halign(button, GTK_ALIGN_START);
	panel->colors[slot] = button;
	add_dialog_input(panel->grid, name, button);
	g_signal_connect(button, "notify::rgba", G_CALLBACK(change_color), panel);
}

static void add_second_option(AdjustmentPanel *panel, const char *name, gboolean active) {
	panel->second_option = gtk_check_button_new_with_label(name);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(panel->second_option), active);
	attach_wide(panel, panel->second_option);
	g_signal_connect(panel->second_option, "toggled", G_CALLBACK(toggle_second_option), panel);
}

static void read_color(const Record *settings, const char *key, double rgb[RGB_CHANNELS]) {
	rgb[0] = rgb[1] = rgb[2] = 0;
	read_descriptor_color(get_record(get_record(settings, key), "v"), rgb);
}

static void add_choice(AdjustmentPanel *panel, const char *name, const char *const *labels) {
	panel->choice = gtk_drop_down_new_from_strings(labels);
	add_dialog_input(panel->grid, name, panel->choice);
	g_signal_connect(panel->choice, "notify::selected", G_CALLBACK(change_choice), panel);
}

static void add_option(AdjustmentPanel *panel, const char *name, gboolean active) {
	panel->option = gtk_check_button_new_with_label(name);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(panel->option), active);
	attach_wide(panel, panel->option);
	g_signal_connect(panel->option, "toggled", G_CALLBACK(toggle_option), panel);
}

static void add_histogram(AdjustmentPanel *panel) {
	if (!panel->has_histogram)
		return;
	panel->histogram = create_snapshot_area(draw_histogram, NULL, panel);
	gtk_widget_set_size_request(panel->histogram, CURVE_LEVELS, HISTOGRAM_HEIGHT);
	gtk_widget_add_css_class(panel->histogram, "dim-label");
	attach_wide(panel, panel->histogram);
	if (strcmp(panel->key, "levl"))
		return;
	GtkGesture *drag = gtk_gesture_drag_new();
	g_signal_connect(drag, "drag-begin", G_CALLBACK(begin_marker_drag), panel);
	g_signal_connect(drag, "drag-update", G_CALLBACK(update_marker_drag), panel);
	gtk_widget_add_controller(panel->histogram, GTK_EVENT_CONTROLLER(drag));
}

/* The gradient as Gradient Map applies it, from the darkest level on the left */
static void draw_gradient_preview(GtkWidget *widget, GtkSnapshot *snapshot, int width, int height, gpointer data) {
	AdjustmentPanel *panel = data;
	g_autofree Adjustment *adjustment = g_new(Adjustment, 1);
	if (!build_adjustment("grdm", panel->settings, adjustment))
		return;
	for (int x = 0; x < width; x++) {
		int entry = MIN(adjustment->curve_length - 1, x * adjustment->curve_length / MAX(1, width));
		GdkRGBA color = {adjustment->red[entry], adjustment->green[entry], adjustment->blue[entry], 1};
		gtk_snapshot_append_color(snapshot, &color, &GRAPHENE_RECT_INIT(x, 0, 1, height));
	}
}

static void finish_loading_lut(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GtkWidget) popover = data;
	AdjustmentPanel *panel = g_object_get_data(G_OBJECT(popover), "adjustment-panel");
	g_autoptr(GError) error = NULL;
	g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
	g_autofree char *contents = NULL;
	gsize length = 0;
	if (!file || !g_file_load_contents(file, NULL, &contents, &length, NULL, &error))
		return;
	g_autoptr(GBytes) profile = convert_lut_file((const guint8 *)contents, length);
	g_autofree Adjustment *adjustment = g_new(Adjustment, 1);
	g_autofree char *name = g_file_get_basename(file);
	g_autoptr(Record) settings = copy_record(panel->settings);
	if (profile)
		set_color_lookup_file(settings, profile, name);
	if (!profile || !build_adjustment("clrL", settings, adjustment)) {
		gtk_label_set_text(GTK_LABEL(panel->editor->status), TEXT_LUT_UNREADABLE);
		return;
	}
	free_record(panel->settings);
	panel->settings = g_steal_pointer(&settings);
	gtk_label_set_text(GTK_LABEL(panel->file_label), name);
	apply_panel_settings(panel);
	/* The popover may already be closed and go away with its last reference, so the canvas updates now */
	if (panel->redraw)
		redraw_canvas(panel);
}

/* The popover closes while the file dialog is open, so a reference keeps the panel until the dialog answers */
static void load_lut(GtkButton *button, AdjustmentPanel *panel) {
	GtkFileDialog *dialog = gtk_file_dialog_new();
	GtkFileFilter *filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, TEXT_LUT_FILTER);
	const char *patterns[] = {"*.cube", "*.CUBE", "*.3dl", "*.3DL", "*.look", "*.icc", "*.icm"};
	for (guint index = 0; index < G_N_ELEMENTS(patterns); index++)
		gtk_file_filter_add_pattern(filter, patterns[index]);
	g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	g_list_store_append(filters, filter);
	gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
	g_object_unref(filter);
	gtk_file_dialog_set_modal(dialog, TRUE);
	GtkRoot *window = gtk_widget_get_root(panel->editor->layer_list);
	gtk_file_dialog_open(dialog, GTK_WINDOW(window), NULL, finish_loading_lut, g_object_ref(panel->popover));
	g_object_unref(dialog);
}

static void add_curves_rows(AdjustmentPanel *panel) {
	add_choice(panel, TEXT_CHANNEL,
			   (const char *[]){TEXT_CHANNEL_RGB, TEXT_CHANNEL_RED, TEXT_CHANNEL_GREEN, TEXT_CHANNEL_BLUE, NULL});
	panel->curve = create_curve_editor((CurveEditorChanged)change_curve, panel);
	attach_wide(panel, panel->curve);
	read_curve_channel(panel);
}

static void add_gradient_map_rows(AdjustmentPanel *panel) {
	const Record *settings = panel->settings;
	panel->preview = create_snapshot_area(draw_gradient_preview, NULL, panel);
	gtk_widget_set_size_request(panel->preview, CURVE_LEVELS, GRADIENT_PREVIEW_HEIGHT);
	attach_wide(panel, panel->preview);
	add_choice(panel, TEXT_GRADIENT_METHOD,
			   (const char *[]){TEXT_GRADIENT_CLASSIC, TEXT_GRADIENT_LINEAR, TEXT_GRADIENT_PERCEPTUAL,
								TEXT_GRADIENT_SMOOTH, TEXT_GRADIENT_STRIPES, NULL});
	gtk_drop_down_set_selected(GTK_DROP_DOWN(panel->choice), get_gradient_map_method(settings));
	g_autoptr(Gradient) gradient = read_gradient_record(get_record(get_record(settings, "Grad"), "v"));
	/* Stops in the first half color the start and the others the end, black to white without a gradient */
	double colors[2][RGB_CHANNELS] = {{0, 0, 0}, {G_MAXUINT8, G_MAXUINT8, G_MAXUINT8}};
	for (int index = 0; gradient && index < gradient->color_count; index++) {
		const GradientColorStop *stop = &gradient->colors[index];
		int end = stop->location >= GRADIENT_LOCATION_RANGE / 2;
		colors[end][0] = stop->red;
		colors[end][1] = stop->green;
		colors[end][2] = stop->blue;
	}
	add_color(panel, 0, TEXT_GRADIENT_START, colors[0]);
	add_color(panel, 1, TEXT_GRADIENT_END, colors[1]);
	add_option(panel, TEXT_REVERSE, get_descriptor_boolean(settings, "Rvrs", FALSE));
	add_second_option(panel, TEXT_DITHER, get_descriptor_boolean(settings, "Dthr", FALSE));
}

static void add_black_white_rows(AdjustmentPanel *panel) {
	const char *names[GRAY_WEIGHTS_COUNT] = {TEXT_HUE_REDS,	 TEXT_HUE_YELLOWS, TEXT_HUE_GREENS,
											 TEXT_HUE_CYANS, TEXT_HUE_BLUES,   TEXT_HUE_MAGENTAS};
	for (int index = 0; index < GRAY_WEIGHTS_COUNT; index++) {
		SliderSpec spec = GRAY_SLIDER;
		spec.label = names[index];
		spec.key = GRAY_WEIGHTS[index];
		add_slider(panel, &spec);
	}
	add_option(panel, TEXT_TINT, get_descriptor_boolean(panel->settings, "useTint", FALSE));
	double tint[RGB_CHANNELS];
	read_color(panel->settings, "tintColor", tint);
	add_color(panel, 0, TEXT_TINT_COLOR, tint);
}

static void add_color_lookup_rows(AdjustmentPanel *panel) {
	const char *name = get_color_lookup_name(panel->settings);
	panel->file_label = gtk_label_new(name ? name : TEXT_LUT_NONE);
	gtk_label_set_ellipsize(GTK_LABEL(panel->file_label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_xalign(GTK_LABEL(panel->file_label), 0);
	attach_wide(panel, panel->file_label);
	GtkWidget *load = gtk_button_new_with_label(TEXT_LOAD_LUT);
	gtk_widget_set_halign(load, GTK_ALIGN_START);
	g_signal_connect(load, "clicked", G_CALLBACK(load_lut), panel);
	attach_wide(panel, load);
}

static void add_settings_rows(AdjustmentPanel *panel) {
	const Record *settings = panel->settings;
	const char *key = panel->key;
	if (!strcmp(key, "curv"))
		add_curves_rows(panel);
	else if (!strcmp(key, "grdm"))
		add_gradient_map_rows(panel);
	else if (!strcmp(key, "blwh"))
		add_black_white_rows(panel);
	else if (!strcmp(key, "clrL"))
		add_color_lookup_rows(panel);
	else if (!strcmp(key, "phfl")) {
		double color[RGB_CHANNELS];
		read_color(settings, "Clr", color);
		add_color(panel, 0, TEXT_FILTER_COLOR, color);
		add_sliders(panel, FILTER_SLIDERS, G_N_ELEMENTS(FILTER_SLIDERS));
		add_option(panel, TEXT_PRESERVE_LUMINOSITY, get_descriptor_boolean(settings, "PrsL", TRUE));
	} else if (!strcmp(key, "selc")) {
		add_choice(panel, TEXT_SELECTIVE_COLORS,
				   (const char *[]){TEXT_HUE_REDS, TEXT_HUE_YELLOWS, TEXT_HUE_GREENS, TEXT_HUE_CYANS, TEXT_HUE_BLUES,
									TEXT_HUE_MAGENTAS, TEXT_SELECTIVE_WHITES, TEXT_SELECTIVE_NEUTRALS,
									TEXT_SELECTIVE_BLACKS, NULL});
		add_sliders(panel, SELECTIVE_SLIDERS, G_N_ELEMENTS(SELECTIVE_SLIDERS));
		add_option(panel, TEXT_ABSOLUTE, is_absolute_selective_color(settings));
		read_choice(panel);
	} else if (!strcmp(key, "rplc")) {
		double color[RGB_CHANNELS];
		read_color(settings, "Mnm", color);
		add_color(panel, 0, TEXT_REPLACE_COLOR_SAMPLE, color);
		add_sliders(panel, REPLACE_SLIDERS, G_N_ELEMENTS(REPLACE_SLIDERS));
	} else if (!strcmp(key, "levl")) {
		add_choice(panel, TEXT_CHANNEL,
				   (const char *[]){TEXT_CHANNEL_RGB, TEXT_CHANNEL_RED, TEXT_CHANNEL_GREEN, TEXT_CHANNEL_BLUE, NULL});
		add_histogram(panel);
		add_sliders(panel, LEVELS_SLIDERS, G_N_ELEMENTS(LEVELS_SLIDERS));
		read_levels_channel(panel);
	} else if (!strcmp(key, "brit")) {
		add_sliders(panel, BRIGHTNESS_SLIDERS, G_N_ELEMENTS(BRIGHTNESS_SLIDERS));
		add_option(panel, TEXT_USE_LEGACY, get_descriptor_boolean(settings, "useLegacy", FALSE));
	} else if (!strcmp(key, "vibA"))
		add_sliders(panel, VIBRANCE_SLIDERS, G_N_ELEMENTS(VIBRANCE_SLIDERS));
	else if (!strcmp(key, "hue2")) {
		add_choice(panel, TEXT_HUE_RANGE,
				   (const char *[]){TEXT_HUE_MASTER, TEXT_HUE_REDS, TEXT_HUE_YELLOWS, TEXT_HUE_GREENS, TEXT_HUE_CYANS,
									TEXT_HUE_BLUES, TEXT_HUE_MAGENTAS, NULL});
		add_sliders(panel, HUE_SLIDERS, G_N_ELEMENTS(HUE_SLIDERS));
		add_option(panel, TEXT_COLORIZE, get_descriptor_boolean(settings, "Clrz", FALSE));
		gtk_widget_set_sensitive(panel->choice, !is_option_active(panel));
		read_choice(panel);
	} else if (!strcmp(key, "blnc")) {
		add_choice(panel, TEXT_TONE, (const char *[]){TEXT_TONE_SHADOWS, TEXT_TONE_MIDTONES, TEXT_TONE_HIGHLIGHTS, NULL});
		add_sliders(panel, BALANCE_SLIDERS, G_N_ELEMENTS(BALANCE_SLIDERS));
		add_option(panel, TEXT_PRESERVE_LUMINOSITY, get_descriptor_boolean(settings, "PrsL", TRUE));
		/* Color Balance opens on the midtones, and the choice shows their values */
		gtk_drop_down_set_selected(GTK_DROP_DOWN(panel->choice), BALANCE_MIDTONES);
	} else if (!strcmp(key, "mixr")) {
		add_choice(panel, TEXT_OUTPUT_CHANNEL,
				   (const char *[]){TEXT_CHANNEL_RED, TEXT_CHANNEL_GREEN, TEXT_CHANNEL_BLUE, NULL});
		add_sliders(panel, MIXER_SLIDERS, G_N_ELEMENTS(MIXER_SLIDERS));
		double values[MIXER_VALUES];
		add_option(panel, TEXT_MONOCHROME, get_channel_mixer(settings, values));
		gtk_widget_set_sensitive(panel->choice, !is_option_active(panel));
		read_choice(panel);
	} else if (!strcmp(key, "expA"))
		add_sliders(panel, EXPOSURE_SLIDERS, G_N_ELEMENTS(EXPOSURE_SLIDERS));
	else if (!strcmp(key, "post"))
		add_sliders(panel, POSTERIZE_SLIDERS, G_N_ELEMENTS(POSTERIZE_SLIDERS));
	else if (!strcmp(key, "thrs")) {
		add_histogram(panel);
		add_sliders(panel, THRESHOLD_SLIDERS, G_N_ELEMENTS(THRESHOLD_SLIDERS));
	}
}

/* ============================== Panel =========================================== */

static void toggle_clip(GtkCheckButton *button, AdjustmentPanel *panel) {
	if (panel->updating)
		return;
	g_autoptr(GError) error = NULL;
	if (!toggle_clipping_mask(panel->document, FALSE, &error)) {
		gtk_label_set_text(GTK_LABEL(panel->editor->status), error->message);
		return;
	}
	redraw_canvas(panel);
}

static void close_panel(GtkPopover *popover, AdjustmentPanel *panel) {
	if (panel->redraw)
		redraw_canvas(panel);
	g_idle_add_once((GSourceOnceFunc)gtk_widget_unparent, popover);
}

static void free_panel(AdjustmentPanel *panel) {
	g_clear_handle_id(&panel->redraw, g_source_remove);
	free_record(panel->settings);
	g_free(panel);
}

void show_adjustment_panel(Editor *editor, int layer_index) {
	PresentedDocument *presented = get_page_document(get_current_page(editor));
	Document *document = presented ? presented->document : NULL;
	Record *layer = document ? get_item(get_record(document->record, "Z"), layer_index) : NULL;
	const char *key = layer ? find_adjustment_key(get_record(layer, "add")) : NULL;
	if (!is_ported_adjustment(key) || document->preview)
		return;
	/* The clip switch works on the selected layer, so the adjusted layer becomes the selection */
	Record *selected = get_record(document->record, "j");
	if (count_records(selected) != 1 || get_item(selected, 0)->value.number != layer_index) {
		select_layer(document, layer_index, FALSE, FALSE, NULL);
		refresh_document_page(editor, get_current_page(editor), FALSE);
	}
	AdjustmentPanel *panel = g_new0(AdjustmentPanel, 1);
	panel->editor = editor;
	panel->document = document;
	panel->layer_index = layer_index;
	panel->key = key;
	panel->settings = copy_record(get_record(get_record(layer, "add"), key));
	if (!strcmp(key, "levl") || !strcmp(key, "thrs") || !strcmp(key, "curv"))
		count_backdrop(panel, document);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_SNUG);
	GtkWidget *title = gtk_label_new(get_adjustment_name(key));
	gtk_widget_add_css_class(title, "heading");
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	gtk_box_append(GTK_BOX(box), title);
	panel->grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(panel->grid), SPACING_SNUG);
	gtk_grid_set_column_spacing(GTK_GRID(panel->grid), SPACING_NORMAL);
	gtk_box_append(GTK_BOX(box), panel->grid);
	add_settings_rows(panel);
	GtkWidget *clip = gtk_check_button_new_with_label(TEXT_CLIP_TO_LAYER_BELOW);
	gtk_check_button_set_active(GTK_CHECK_BUTTON(clip), get_boolean(layer, "jl"));
	gtk_widget_set_sensitive(clip, get_boolean(layer, "jl") || can_clip_layer(document, layer_index));
	g_signal_connect(clip, "toggled", G_CALLBACK(toggle_clip), panel);
	attach_wide(panel, clip);
	gtk_widget_set_margin_start(box, SPACING_SMALL);
	gtk_widget_set_margin_end(box, SPACING_SMALL);
	gtk_widget_set_margin_top(box, SPACING_SMALL);
	gtk_widget_set_margin_bottom(box, SPACING_SMALL);
	panel->popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(panel->popover), box);
	/* The rows rebuild as layers change, so the popover hangs from the scroll area around them */
	GtkWidget *anchor = gtk_widget_get_ancestor(editor->layer_list, GTK_TYPE_SCROLLED_WINDOW);
	gtk_widget_set_parent(panel->popover, anchor);
	gtk_popover_set_position(GTK_POPOVER(panel->popover), GTK_POS_LEFT);
	GdkRectangle bounds;
	if (get_layer_row_bounds(editor, layer_index, anchor, &bounds))
		gtk_popover_set_pointing_to(GTK_POPOVER(panel->popover), &bounds);
	g_signal_connect(panel->popover, "closed", G_CALLBACK(close_panel), panel);
	g_object_set_data_full(G_OBJECT(panel->popover), "adjustment-panel", panel, (GDestroyNotify)free_panel);
	gtk_popover_popup(GTK_POPOVER(panel->popover));
}
