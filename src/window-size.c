#include "window-size.h"

void choose_window_size(int monitor_width, int monitor_height, int *width, int *height) {
	/* Whole units keep both dimensions inside the monitor at an exact 16 to 9 ratio */
	int units_from_width = monitor_width * 9 / 160;
	int units_from_height = monitor_height / 10;
	int units = units_from_width < units_from_height ? units_from_width : units_from_height;
	if (units < 1)
		units = 1;
	*width = units * 16;
	*height = units * 9;
}
