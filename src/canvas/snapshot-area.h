#pragma once

#include <gtk/gtk.h>

/* A widget that records GPU render nodes instead of drawing with the CPU */
typedef void (*SnapshotAreaDraw)(GtkWidget *widget, GtkSnapshot *snapshot, int width, int height, gpointer data);
typedef void (*SnapshotAreaResize)(GtkWidget *widget, int width, int height, gpointer data);
/* Gives the height an area needs at a width, for content that wraps like a grid */
typedef int (*SnapshotAreaMeasure)(GtkWidget *widget, int width, gpointer data);

GtkWidget *create_snapshot_area(SnapshotAreaDraw draw, SnapshotAreaResize resize, gpointer data);
void set_snapshot_area_measure(GtkWidget *widget, SnapshotAreaMeasure measure);

/* A square centered on a point */
graphene_rect_t square_around(double x, double y, double half_side);
/* A border of one width and colour around a box, with corners rounded by a radius */
void append_outline(GtkSnapshot *snapshot, graphene_rect_t box, float corner, float width, const GdkRGBA *color);
