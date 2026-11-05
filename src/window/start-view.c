#include "common/app/identity.h"
#include "editor.h"
#include "lang/en.h"
#include "common/widgets/spacing.h"

#define START_ICON_SIZE 96
#define START_BUTTON_ICON_SIZE 32

/* A start button names its action and shows the same shortcut the menu gives the action */
static GtkWidget *create_start_button(Editor *editor, const char *icon, const char *title, const char *action) {
	GtkWidget *button = gtk_button_new(), *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_SMALL);
	gtk_widget_add_css_class(button, "card");
	gtk_widget_add_css_class(button, "start-button");
	GtkWidget *image = gtk_image_new_from_icon_name(icon);
	gtk_image_set_pixel_size(GTK_IMAGE(image), START_BUTTON_ICON_SIZE);
	gtk_widget_set_margin_bottom(image, SPACING_TIGHT);
	gtk_box_append(GTK_BOX(content), image);
	gtk_box_append(GTK_BOX(content), gtk_label_new(title));
	g_auto(GStrv) accelerators = gtk_application_get_accels_for_action(editor->application, action);
	guint key;
	GdkModifierType modifiers;
	if (accelerators[0] && gtk_accelerator_parse(accelerators[0], &key, &modifiers)) {
		g_autofree char *shortcut = gtk_accelerator_get_label(key, modifiers);
		GtkWidget *hint = gtk_label_new(shortcut);
		gtk_widget_add_css_class(hint, "start-shortcut");
		gtk_box_append(GTK_BOX(content), hint);
	}
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
	GtkWidget *view = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_LARGE);
	gtk_widget_add_css_class(view, "start-view");
	gtk_widget_set_valign(view, GTK_ALIGN_CENTER);
	gtk_widget_set_halign(view, GTK_ALIGN_CENTER);
	GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	GtkWidget *icon = gtk_image_new_from_icon_name(get_application_identity()->id);
	gtk_image_set_pixel_size(GTK_IMAGE(icon), START_ICON_SIZE);
	gtk_widget_add_css_class(icon, "icon-dropshadow");
	gtk_box_append(GTK_BOX(heading), icon);
	GtkWidget *title = gtk_label_new(TEXT_APPLICATION_NAME);
	gtk_widget_add_css_class(title, "title-1");
	gtk_box_append(GTK_BOX(heading), title);
	gtk_box_append(GTK_BOX(view), heading);
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_ROOMY);
	/* Both start buttons share one size whatever their labels */
	gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
	gtk_widget_set_halign(buttons, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(buttons), create_start_button(editor, "document-new-symbolic", TEXT_START_NEW, "win.new"));
	GtkWidget *open = create_start_button(editor, "folder-open-symbolic", TEXT_START_OPEN, "win.open");
	g_object_set_data(G_OBJECT(editor->window), "start-open", open);
	gtk_box_append(GTK_BOX(buttons), open);
	gtk_box_append(GTK_BOX(view), buttons);
	GtkWidget *hint = gtk_label_new(TEXT_START_HINT);
	gtk_widget_add_css_class(hint, "dim-label");
	gtk_box_append(GTK_BOX(view), hint);
	gtk_box_append(GTK_BOX(view), create_recent_list(editor));
	/* A double click on the empty background opens files, like the empty tab bar */
	GtkWidget *area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(area, "start-area");
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
	/* The focus ring waits until the keyboard is used to move around, so Open does not look chosen */
	gtk_window_set_focus_visible(GTK_WINDOW(window), FALSE);
}
