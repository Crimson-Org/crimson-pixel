#pragma once

#include <gtk/gtk.h>

/* A widget that records GPU render nodes instead of drawing with the CPU */
typedef void (*SnapshotAreaDraw)(GtkWidget *widget, GtkSnapshot *snapshot, int width, int height, gpointer data);
typedef void (*SnapshotAreaResize)(GtkWidget *widget, int width, int height, gpointer data);
/* Gives the height an area needs at a width, for content that wraps like a grid */
typedef int (*SnapshotAreaMeasure)(GtkWidget *widget, int width, gpointer data);

GtkWidget *create_snapshot_area(SnapshotAreaDraw draw, SnapshotAreaResize resize, gpointer data);
void set_snapshot_area_measure(GtkWidget *widget, SnapshotAreaMeasure measure);
