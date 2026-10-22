#include "snapshot-area.h"

#define SNAPSHOT_TYPE_AREA (snapshot_area_get_type())
G_DECLARE_FINAL_TYPE(SnapshotArea, snapshot_area, SNAPSHOT, AREA, GtkWidget)

struct _SnapshotArea {
	GtkWidget parent_instance;
	SnapshotAreaDraw draw;
	SnapshotAreaResize resize;
	SnapshotAreaMeasure measure;
	gpointer data;
};

G_DEFINE_FINAL_TYPE(SnapshotArea, snapshot_area, GTK_TYPE_WIDGET)

static void snapshot_area(GtkWidget *widget, GtkSnapshot *snapshot) {
	SnapshotArea *area = SNAPSHOT_AREA(widget);
	if (area->draw)
		area->draw(widget, snapshot, gtk_widget_get_width(widget), gtk_widget_get_height(widget), area->data);
}

static void allocate_area(GtkWidget *widget, int width, int height, int baseline) {
	SnapshotArea *area = SNAPSHOT_AREA(widget);
	if (area->resize)
		area->resize(widget, width, height, area->data);
	/* Popovers parented to the area, such as context menus, are sized and placed through their parent */
	for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
		if (GTK_IS_POPOVER(child))
			gtk_popover_present(GTK_POPOVER(child));
}

static GtkSizeRequestMode get_request_mode(GtkWidget *widget) {
	return SNAPSHOT_AREA(widget)->measure ? GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH : GTK_SIZE_REQUEST_CONSTANT_SIZE;
}

static void measure_area(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum, int *natural,
						 int *minimum_baseline, int *natural_baseline) {
	SnapshotArea *area = SNAPSHOT_AREA(widget);
	*minimum = *natural = 0;
	if (area->measure && orientation == GTK_ORIENTATION_VERTICAL)
		*minimum = *natural = area->measure(widget, for_size < 0 ? gtk_widget_get_width(widget) : for_size, area->data);
}

static void snapshot_area_class_init(SnapshotAreaClass *class) {
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);
	widget_class->snapshot = snapshot_area;
	widget_class->size_allocate = allocate_area;
	widget_class->get_request_mode = get_request_mode;
	widget_class->measure = measure_area;
}

static void snapshot_area_init(SnapshotArea *area) {
}

GtkWidget *create_snapshot_area(SnapshotAreaDraw draw, SnapshotAreaResize resize, gpointer data) {
	SnapshotArea *area = g_object_new(SNAPSHOT_TYPE_AREA, NULL);
	area->draw = draw;
	area->resize = resize;
	area->data = data;
	return GTK_WIDGET(area);
}

void set_snapshot_area_measure(GtkWidget *widget, SnapshotAreaMeasure measure) {
	SNAPSHOT_AREA(widget)->measure = measure;
	gtk_widget_queue_resize(widget);
}
