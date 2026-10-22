#pragma once

#include "record.h"
#include "kernels.h"

typedef struct {
	GString *commands;
	GArray *points;
} SelectionPath;

typedef struct {
	gboolean pressed, dragged, shift, alt, start_shift, start_alt;
	int shift_stage, alt_stage;
} ModifierTracker;

SelectionPath *create_selection_path(void);
void free_selection_path(SelectionPath *path);
SelectionPath *create_ellipse_path(RasterExtent box);
SelectionPath *create_polygon_path(const double *points, guint count);
RasterExtent get_selection_path_bounds(const SelectionPath *path);
Record *rasterize_selection_path(const SelectionPath *path, unsigned depth, gboolean anti_alias, RasterExtent *bounds);
void constrain_polygon_angle(double start_x, double start_y, double *x, double *y);

void begin_modifier_tracking(ModifierTracker *tracker, gboolean shift, gboolean alt);
void update_modifier_tracking(ModifierTracker *tracker, gboolean shift, gboolean alt);
gboolean is_shift_held(const ModifierTracker *tracker);
gboolean is_alt_held(const ModifierTracker *tracker);
gboolean was_shift_pressed_again(const ModifierTracker *tracker);
gboolean was_alt_pressed_again(const ModifierTracker *tracker);
