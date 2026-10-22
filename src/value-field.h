#pragma once

#include <gtk/gtk.h>

typedef void (*ValueFieldChanged)(GtkWidget *field, double value, gpointer data);

GtkWidget *create_value_field(const char *label_text, const char *unit, double minimum, double maximum,
							  ValueFieldChanged changed, gpointer data);
void set_value_field(GtkWidget *field, double value);
double get_value_field_value(GtkWidget *field);
void set_value_field_digits(GtkWidget *field, int digits);
