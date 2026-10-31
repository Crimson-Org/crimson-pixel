#pragma once

#include "document/record.h"
#include "tools/kernels.h"

typedef struct {
	GString *commands;
	GArray *points;
} SelectionPath;

/* A modifier held when a drag starts is let go and can then be pressed again, which changes what it does */
typedef enum { MODIFIER_HELD, MODIFIER_LET_GO, MODIFIER_PRESSED_AGAIN } ModifierStage;

typedef struct {
	gboolean pressed, dragged, shift, alt, start_shift, start_alt;
	ModifierStage shift_stage, alt_stage;
} ModifierTracker;

SelectionPath *create_selection_path(void);
void free_selection_path(SelectionPath *path);
SelectionPath *create_ellipse_path(RasterExtent box);
SelectionPath *create_polygon_path(const double *points, guint count);
RasterExtent get_selection_path_bounds(const SelectionPath *path);
Record *rasterize_selection_path(const SelectionPath *path, unsigned depth, gboolean anti_alias, RasterExtent *bounds);

void begin_modifier_tracking(ModifierTracker *tracker, gboolean shift, gboolean alt);
void update_modifier_tracking(ModifierTracker *tracker, gboolean shift, gboolean alt);
gboolean is_shift_held(const ModifierTracker *tracker);
gboolean is_alt_held(const ModifierTracker *tracker);
gboolean was_shift_pressed_again(const ModifierTracker *tracker);
gboolean was_alt_pressed_again(const ModifierTracker *tracker);
