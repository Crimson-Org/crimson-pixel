#include <math.h>

#include "curve-spline.h"

static void solve_matrix(double *matrix, int count, double *solution) {
	const int width = count + 1;
	for (int column = 0; column < count; column++) {
		int pivot = 0;
		double largest = -INFINITY;
		for (int row = column; row < count; row++)
			if (fabs(matrix[row * width + column]) > largest) {
				pivot = row;
				largest = fabs(matrix[row * width + column]);
			}
		for (int field = 0; field < width; field++) {
			double swapped = matrix[column * width + field];
			matrix[column * width + field] = matrix[pivot * width + field];
			matrix[pivot * width + field] = swapped;
		}
		for (int row = column + 1; row < count; row++) {
			if (matrix[column * width + column] == 0)
				return;
			double factor = matrix[row * width + column] / matrix[column * width + column];
			for (int field = column; field < width; field++)
				matrix[row * width + field] -= matrix[column * width + field] * factor;
		}
	}
	for (int row = count - 1; row >= 0; row--) {
		if (matrix[row * width + row] == 0)
			return;
		double value = matrix[row * width + count] / matrix[row * width + row];
		solution[row] = value;
		for (int above = row - 1; above >= 0; above--) {
			matrix[above * width + count] -= matrix[above * width + row] * value;
			matrix[above * width + row] = 0;
		}
	}
}

static void solve_spline_slopes(const double *x, const double *y, int count, double *slopes) {
	const int last = count - 1, width = count + 1;
	g_autofree double *matrix = g_new0(double, (gsize)count *width);
	for (int index = 1; index < last; index++) {
		double before = x[index] - x[index - 1], after = x[index + 1] - x[index];
		double *row = matrix + index * width;
		row[index - 1] = 1 / before;
		row[index] = 2 * (1 / before + 1 / after);
		row[index + 1] = 1 / after;
		row[last + 1] =
			3 * ((y[index] - y[index - 1]) / (before * before) + (y[index + 1] - y[index]) / (after * after));
	}
	matrix[0] = 2 / (x[1] - x[0]);
	matrix[1] = 1 / (x[1] - x[0]);
	matrix[last + 1] = 3 * (y[1] - y[0]) / ((x[1] - x[0]) * (x[1] - x[0]));
	double *end = matrix + last * width;
	end[last - 1] = 1 / (x[last] - x[last - 1]);
	end[last] = 2 / (x[last] - x[last - 1]);
	end[last + 1] = 3 * (y[last] - y[last - 1]) / ((x[last] - x[last - 1]) * (x[last] - x[last - 1]));
	for (int index = 0; index <= last; index++)
		slopes[index] = NAN;
	solve_matrix(matrix, count, slopes);
}

static double evaluate_spline_run(double position, const double *x, const double *y, const double *slopes, int count) {
	if (position <= x[0])
		return y[0];
	if (position >= x[count - 1])
		return y[count - 1];
	int upper = 1;
	while (x[upper] < position)
		upper++;
	double right = x[upper], left = x[upper - 1], top = y[upper], bottom = y[upper - 1];
	double share = (position - left) / (right - left);
	double rise = slopes[upper - 1] * (right - left) - (top - bottom);
	double fall = -slopes[upper] * (right - left) + (top - bottom);
	return (1 - share) * bottom + share * top + share * (1 - share) * (rise * (1 - share) + fall * share);
}

void sample_spline(const CurvePoint *points, int count, float *curve, int length) {
	g_autofree double *x = g_new(double, count), *y = g_new(double, count), *slopes = g_new(double, count);
	g_autofree int *run_of = g_new(int, count), *run_start = g_new(int, count), *run_count = g_new(int, count);
	for (int index = 0; index < count; index++) {
		x[index] = points[index].input;
		y[index] = points[index].output;
	}
	int run = 0, start = 0;
	run_of[0] = 0;
	for (int index = 1; index < count - 1; index++) {
		if (!points[index].smooth) {
			run_start[run] = start;
			run_count[run++] = index - start + 1;
			start = index;
		}
		run_of[index] = run;
	}
	run_start[run] = start;
	run_count[run] = count - start;
	run_of[count - 1] = run;
	/* A corner point ends one run and starts the next, so each run solves its own copy of the slopes */
	g_autofree double *run_slopes = g_new(double, (gsize)(run + 1) * count);
	for (int index = 0; index <= run; index++)
		solve_spline_slopes(x + run_start[index], y + run_start[index], run_count[index], run_slopes + index * count);
	double step = 255.0 / (length - 1);
	for (int index = 0; index < length; index++) {
		double position = index * step, value;
		if (position <= x[0])
			value = y[0];
		else if (position >= x[count - 1])
			value = y[count - 1];
		else {
			int upper = 1;
			while (x[upper] < position)
				upper++;
			int found = run_of[upper - 1];
			value = evaluate_spline_run(position, x + run_start[found], y + run_start[found],
										run_slopes + found * count, run_count[found]);
		}
		curve[index] = (1 / 255.0) * (isnan(value) ? value : MAX(0, MIN(255, value)));
	}
}
