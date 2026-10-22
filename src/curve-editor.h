#pragma once

#include <gtk/gtk.h>

#include "adjustments.h"

/* Tells the owner that the points changed while dragging */
typedef void (*CurveEditorChanged)(GtkWidget *editor, gpointer data);

/* A Curves graph of one channel: pressing on it adds a point, dragging moves the nearest point between its
 * neighbours, and dragging a point out of the graph removes it while at least two remain */
GtkWidget *create_curve_editor(CurveEditorChanged changed, gpointer data);
void set_curve_editor_points(GtkWidget *editor, const CurvePoint *points, guint count);
/* Shows pixel counts of 256 levels behind the curve, or nothing for NULL */
void set_curve_editor_histogram(GtkWidget *editor, const guint32 *counts);
const GArray *get_curve_editor_points(GtkWidget *editor);
