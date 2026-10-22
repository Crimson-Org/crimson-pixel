#include "editor.h"
#include "../lang/en.h"

static GtkWidget *create_start_button(const char *icon, const char *title, const char *action) {
	GtkWidget *button = gtk_button_new(), *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *image = gtk_image_new_from_icon_name(icon);
	gtk_image_set_pixel_size(GTK_IMAGE(image), 48);
	gtk_box_append(GTK_BOX(content), image);
	gtk_box_append(GTK_BOX(content), gtk_label_new(title));
	gtk_widget_set_margin_start(content, 24);
	gtk_widget_set_margin_end(content, 24);
	gtk_widget_set_margin_top(content, 18);
	gtk_widget_set_margin_bottom(content, 18);
	gtk_button_set_child(GTK_BUTTON(button), content);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
	return button;
}

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

GtkWidget *create_start_view(Editor *editor) {
	GtkWidget *view = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
	gtk_widget_set_valign(view, GTK_ALIGN_CENTER);
	gtk_widget_set_halign(view, GTK_ALIGN_CENTER);
	GtkWidget *title = gtk_label_new(TEXT_APPLICATION_NAME);
	gtk_widget_add_css_class(title, "title-1");
	gtk_box_append(GTK_BOX(view), title);
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	/* Both start buttons share one size whatever their labels */
	gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
	gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(buttons), create_start_button("document-new-symbolic", TEXT_START_NEW, "win.new"));
	GtkWidget *open = create_start_button("document-open-symbolic", TEXT_START_OPEN, "win.open");
	g_object_set_data(G_OBJECT(editor->window), "start-open", open);
	gtk_box_append(GTK_BOX(buttons), open);
	gtk_box_append(GTK_BOX(view), buttons);
	GtkWidget *hint = gtk_label_new(TEXT_START_HINT);
	gtk_widget_add_css_class(hint, "dim-label");
	gtk_box_append(GTK_BOX(view), hint);
	gtk_box_append(GTK_BOX(view), create_recent_list(editor));
	/* A double click on the empty background opens files, like the empty tab bar */
	GtkWidget *area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_hexpand(view, TRUE);
	gtk_widget_set_vexpand(view, TRUE);
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
	gtk_widget_grab_focus(g_object_get_data(G_OBJECT(window), "start-open"));
}
