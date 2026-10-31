#pragma once

#include <glib.h>

/* Work on rows first up to but not including last */
typedef void (*RowWork)(int first, int last, gpointer data);

/* Splits the rows over the shared workers and the calling thread and returns when all are done, or runs them
 * directly when they are too few pixels to be worth sharing or the caller is already one of the workers */
void run_rows_in_parallel(int first, int count, gsize row_pixels, RowWork work, gpointer data);
