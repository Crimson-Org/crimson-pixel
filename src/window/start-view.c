#include "common/app/identity.h"
#include "editor.h"
#include "lang/en.h"
#include "common/widgets/start-view.h"

/* The two ways to start, in the order the cards show them, with Open taking the keyboard */
#define OPEN_CHOICE 1
static const StartChoice START_CHOICES[] = {
	{"document-new-symbolic", TEXT_START_NEW, "win.new"},
	{"folder-open-symbolic", TEXT_START_OPEN, "win.open"},
};

/* With no document open the workspace offers the two ways to start */
static void press_start_background(GtkGestureClick *gesture, int presses, double x, double y, Editor *editor) {
	if (presses != 2)
		return;
	GtkWidget *area = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	GtkWidget *picked = gtk_widget_pick(area, x, y, GTK_PICK_DEFAULT);
	if (picked && (GTK_IS_BUTTON(picked) || gtk_widget_get_ancestor(picked, GTK_TYPE_BUTTON)))
		return;
	show_open_dialog(editor);
}

static void open_recent_item(const char *uri, gpointer data) {
	open_recent_uri(data, uri);
}


static void forget_recent_item(const char *uri, gpointer data) {
	forget_recent_file(uri);
}


GtkWidget *create_start_area(Editor *editor) {
	const StartViewSetup setup = {START_CHOICES, G_N_ELEMENTS(START_CHOICES), TEXT_START_HINT, TEXT_START_RECENT,
		TEXT_RECENT_REMOVE, open_recent_item, forget_recent_item, editor};
	GtkWidget *view = create_start_view(&setup);
	editor->start_view = view;
	/* A double click on the empty background opens files, like the empty tab bar */
	GtkWidget *area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(area, "start-area");
	gtk_box_append(GTK_BOX(area), view);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "pressed", G_CALLBACK(press_start_background), editor);
	gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));
	return area;
}

/* GTK focuses the first focusable control when nothing has focus, which was an option field, so the start view's
 * Open button takes it while that view shows */
void focus_start_view(GtkWidget *window, Editor *editor) {
	if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(editor->documents)), "start"))
		return;
	gtk_widget_grab_focus(get_start_view_choice(editor->start_view, OPEN_CHOICE));
	/* The focus ring waits until the keyboard is used to move around, so Open does not look chosen */
	gtk_window_set_focus_visible(GTK_WINDOW(window), FALSE);
}
