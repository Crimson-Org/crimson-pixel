#include "adjustments.h"
#include "editor.h"
#include "../lang/en.h"

GMenu *create_modify_menu(void) {
	GMenu *modify = g_menu_new();
	g_menu_append(modify, TEXT_MODIFY_BORDER_ITEM, "win.modify-border");
	g_menu_append(modify, TEXT_MODIFY_SMOOTH_ITEM, "win.modify-smooth");
	g_menu_append(modify, TEXT_MODIFY_EXPAND_ITEM, "win.modify-expand");
	g_menu_append(modify, TEXT_MODIFY_CONTRACT_ITEM, "win.modify-contract");
	g_menu_append(modify, TEXT_MODIFY_FEATHER_ITEM, "win.modify-feather");
	return modify;
}

/* The unported adjustments are left out until their kernels exist */
GMenuModel *create_adjustment_menu(void) {
	GMenu *menu = g_menu_new(), *section = NULL;
	gboolean starts_group;
	const char *key;
	for (guint index = 0; (key = get_ported_adjustment(index, &starts_group)); index++) {
		if (!section || starts_group) {
			section = g_menu_new();
			g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
			g_object_unref(section);
		}
		g_autoptr(GMenuItem) item = g_menu_item_new(get_adjustment_name(key), NULL);
		g_menu_item_set_action_and_target(item, "win.new-adjustment", "s", key);
		g_menu_append_item(section, item);
	}
	return G_MENU_MODEL(menu);
}

GMenuModel *create_menu_model(Editor *editor) {
	GMenu *bar = g_menu_new();
	GMenu *file = g_menu_new(), *file_open = g_menu_new(), *file_close = g_menu_new();
	g_menu_append(file_open, TEXT_MENU_NEW, "win.new");
	g_menu_append(file_open, TEXT_MENU_OPEN, "win.open");
	g_autoptr(GMenuModel) recent = create_recent_menu(editor);
	g_menu_append_submenu(file_open, TEXT_MENU_OPEN_RECENT, recent);
	GMenu *file_export = g_menu_new();
	g_menu_append(file_export, TEXT_MENU_SAVE, "win.save");
	g_menu_append(file_export, TEXT_MENU_SAVE_AS, "win.save-as");
	g_menu_append(file_export, TEXT_MENU_EXPORT, "win.export");
	g_menu_append(file_close, TEXT_MENU_CLOSE, "win.close");
	g_menu_append(file_close, TEXT_MENU_REOPEN_CLOSED, "win.reopen-closed");
	g_menu_append(file_close, TEXT_MENU_QUIT, "win.quit");
	g_menu_append_section(file, NULL, G_MENU_MODEL(file_open));
	g_menu_append_section(file, NULL, G_MENU_MODEL(file_export));
	g_object_unref(file_export);
	g_menu_append_section(file, NULL, G_MENU_MODEL(file_close));
	g_menu_append_submenu(bar, TEXT_MENU_FILE, G_MENU_MODEL(file));
	GMenu *edit = g_menu_new();
	g_menu_append(edit, TEXT_UNDO, "win.undo");
	g_menu_append(edit, TEXT_REDO, "win.redo");
	GMenu *clipboard = g_menu_new();
	g_menu_append(clipboard, TEXT_CUT, "win.cut");
	g_menu_append(clipboard, TEXT_COPY, "win.copy");
	g_menu_append(clipboard, TEXT_COPY_MERGED, "win.copy-merged");
	g_menu_append(clipboard, TEXT_PASTE, "win.paste");
	g_menu_append(clipboard, TEXT_CLEAR, "win.clear");
	g_menu_append_section(edit, NULL, G_MENU_MODEL(clipboard));
	GMenu *painting = g_menu_new();
	g_menu_append(painting, TEXT_FILL_ITEM, "win.fill");
	g_menu_append(painting, TEXT_STROKE_ITEM, "win.stroke");
	g_menu_append_section(edit, NULL, G_MENU_MODEL(painting));
	g_object_unref(painting);
	g_object_unref(clipboard);
	GMenu *transforming = g_menu_new();
	g_menu_append(transforming, TEXT_FREE_TRANSFORM, "win.free-transform");
	g_menu_append_section(edit, NULL, G_MENU_MODEL(transforming));
	g_object_unref(transforming);
	GMenu *preferences = g_menu_new();
	g_menu_append(preferences, TEXT_PREFERENCES_ITEM, "win.preferences");
	g_menu_append_section(edit, NULL, G_MENU_MODEL(preferences));
	g_object_unref(preferences);
	g_menu_append_submenu(bar, TEXT_MENU_EDIT, G_MENU_MODEL(edit));
	GMenu *view = g_menu_new(), *view_zoom = g_menu_new(), *view_layout = g_menu_new();
	g_menu_append(view_zoom, TEXT_MENU_ZOOM_IN, "win.zoom-in");
	g_menu_append(view_zoom, TEXT_MENU_ZOOM_OUT, "win.zoom-out");
	g_menu_append(view_zoom, TEXT_MENU_FIT, "win.fit");
	g_menu_append(view_zoom, TEXT_MENU_ACTUAL_PIXELS, "win.actual-pixels");
	g_menu_append(view_layout, TEXT_MENU_PIXEL_GRID, "win.grid");
	g_menu_append(view_layout, TEXT_MENU_TILE_PREVIEW, "win.tile-preview");
	GMenu *snapping = g_menu_new(), *snap_to = g_menu_new();
	g_menu_append(snapping, TEXT_MENU_SNAP, "win.snap");
	g_menu_append(snap_to, TEXT_SNAP_LAYERS, "win.snap-layers");
	g_menu_append(snap_to, TEXT_SNAP_BOUNDS, "win.snap-bounds");
	g_menu_append_submenu(snapping, TEXT_MENU_SNAP_TO, G_MENU_MODEL(snap_to));
	g_menu_append_section(view, NULL, G_MENU_MODEL(snapping));
	g_object_unref(snap_to);
	g_object_unref(snapping);
	g_menu_append(view_layout, TEXT_SPLIT_VIEW, "win.split");
	g_menu_append_section(view, NULL, G_MENU_MODEL(view_zoom));
	g_menu_append_section(view, NULL, G_MENU_MODEL(view_layout));
	g_menu_append_submenu(bar, TEXT_MENU_VIEW, G_MENU_MODEL(view));
	GMenu *layer = g_menu_new();
	g_menu_append(layer, TEXT_NEW_LAYER, "win.new-layer");
	g_autoptr(GMenuModel) adjustments = create_adjustment_menu();
	g_menu_append_submenu(layer, TEXT_NEW_ADJUSTMENT_LAYER, adjustments);
	g_menu_append(layer, TEXT_DUPLICATE_LAYER, "win.duplicate-layer");
	g_menu_append(layer, TEXT_LAYER_VIA_COPY, "win.layer-via-copy");
	g_menu_append(layer, TEXT_LAYER_VIA_CUT, "win.layer-via-cut");
	GMenu *arrange = g_menu_new();
	g_menu_append(arrange, TEXT_BRING_TO_FRONT, "win.arrange-front");
	g_menu_append(arrange, TEXT_BRING_FORWARD, "win.arrange-forward");
	g_menu_append(arrange, TEXT_SEND_BACKWARD, "win.arrange-backward");
	g_menu_append(arrange, TEXT_SEND_TO_BACK, "win.arrange-back");
	g_menu_append_submenu(layer, TEXT_ARRANGE, G_MENU_MODEL(arrange));
	g_object_unref(arrange);
	g_menu_append(layer, TEXT_DELETE_LAYER, "win.delete-layer");
	GMenu *structure = g_menu_new();
	g_menu_append(structure, TEXT_GROUP_LAYERS, "win.group-layers");
	g_menu_append(structure, TEXT_UNGROUP_LAYERS, "win.ungroup-layers");
	g_menu_append(structure, TEXT_MERGE_LAYERS, "win.merge-layers");
	g_menu_append(structure, TEXT_MERGE_VISIBLE, "win.merge-visible");
	g_menu_append(structure, TEXT_FLATTEN_IMAGE, "win.flatten-image");
	g_menu_append(structure, TEXT_CLIPPING_MASK, "win.clipping-mask");
	g_menu_append_section(layer, NULL, G_MENU_MODEL(structure));
	g_object_unref(structure);
	g_menu_append_submenu(bar, TEXT_MENU_LAYER, G_MENU_MODEL(layer));
	GMenu *image = g_menu_new();
	GMenu *sizes = g_menu_new();
	g_menu_append(sizes, TEXT_IMAGE_SIZE_ITEM, "win.image-size");
	g_menu_append(sizes, TEXT_CANVAS_SIZE_ITEM, "win.canvas-size");
	g_menu_append_section(image, NULL, G_MENU_MODEL(sizes));
	g_object_unref(sizes);
	GMenu *trimming = g_menu_new();
	g_menu_append(trimming, TEXT_CROP, "win.crop");
	g_menu_append(trimming, TEXT_TRIM_ITEM, "win.trim");
	g_menu_append(trimming, TEXT_REVEAL_ALL, "win.reveal-all");
	g_menu_append_section(image, NULL, G_MENU_MODEL(trimming));
	g_object_unref(trimming);
	g_menu_append_submenu(bar, TEXT_MENU_IMAGE, G_MENU_MODEL(image));
	g_object_unref(image);
	GMenu *select = g_menu_new();
	g_menu_append(select, TEXT_SELECT_ALL, "win.select-all");
	g_menu_append(select, TEXT_DESELECT, "win.deselect");
	g_menu_append(select, TEXT_INVERSE, "win.inverse");
	GMenu *refine = g_menu_new(), *modify = create_modify_menu(), *transform = g_menu_new();
	g_menu_append_submenu(refine, TEXT_MENU_MODIFY, G_MENU_MODEL(modify));
	g_menu_append_section(select, NULL, G_MENU_MODEL(refine));
	g_menu_append(transform, TEXT_TRANSFORM_SELECTION, "win.transform-selection");
	g_menu_append_section(select, NULL, G_MENU_MODEL(transform));
	g_object_unref(refine);
	g_object_unref(modify);
	g_object_unref(transform);
	g_menu_append_submenu(bar, TEXT_MENU_SELECT, G_MENU_MODEL(select));
	g_object_unref(select);
	g_autoptr(GMenuModel) window = create_window_menu(editor);
	g_menu_append_submenu(bar, TEXT_MENU_WINDOW, window);
	g_object_unref(file_open);
	g_object_unref(file_close);
	g_object_unref(file);
	g_object_unref(edit);
	g_object_unref(view_zoom);
	g_object_unref(view_layout);
	g_object_unref(view);
	g_object_unref(layer);
	return G_MENU_MODEL(bar);
}

void set_menu_shortcuts(Editor *editor) {
	const struct {
		const char *action, *accelerator;
	} shortcuts[] = {{"win.open", "<Control>o"},
					 {"win.export", "<Alt><Shift><Control>w"},
					 {"win.save", "<Control>s"},
					 {"win.save-as", "<Shift><Control>s"},
					 {"win.new", "<Control>n"},
					 {"win.close", "<Control>w"},
					 {"win.reopen-closed", "<Shift><Control>t"},
					 {"win.quit", "<Control>q"},
					 {"win.undo", "<Control>z"},
					 {"win.redo", "<Control><Shift>z"},
					 {"win.fit", "<Control>0"},
					 {"win.actual-pixels", "<Control>1"},
					 {"win.grid", "<Shift><Control>h"},
					 {"win.new-layer", "<Shift><Control>n"},
					 {"win.layer-via-copy", "<Control>j"},
					 {"win.layer-via-cut", "<Shift><Control>j"},
					 {"win.snap", "<Shift><Control>p"},
					 {"win.arrange-front", "<Shift><Control>Home"},
					 {"win.arrange-forward", "<Shift><Control>Up"},
					 {"win.arrange-backward", "<Shift><Control>Down"},
					 {"win.arrange-back", "<Shift><Control>End"},
					 {"win.cut", "<Control>x"},
					 {"win.copy", "<Control>c"},
					 {"win.copy-merged", "<Shift><Control>c"},
					 {"win.paste", "<Control>v"},
					 {"win.clear", "Delete"},
					 {"win.free-transform", "<Control>t"},
					 {"win.select-all", "<Control>a"},
					 {"win.deselect", "<Control>d"},
					 {"win.inverse", "<Shift><Control>i"},
					 {"win.fill", "<Shift>F5"},
					 {"win.modify-feather", "<Shift>F6"},
					 {"win.preferences", "<Control>k"},
					 {"win.image-size", "<Alt><Control>i"},
					 {"win.group-layers", "<Control>g"},
					 {"win.ungroup-layers", "<Shift><Control>g"},
					 {"win.merge-layers", "<Control>e"},
					 {"win.merge-visible", "<Shift><Control>e"},
					 {"win.clipping-mask", "<Alt><Control>g"},
					 {"win.canvas-size", "<Alt><Control>c"},
					 {"win.trim-top-left", "<Control>period"}};

	for (guint index = 0; index < G_N_ELEMENTS(shortcuts); index++)
		gtk_application_set_accels_for_action(editor->application, shortcuts[index].action,
											  (const char *[]){shortcuts[index].accelerator, NULL});
	/* The Photoshop keys that need AltGr on a Nordic keyboard stay as a second way */
	const struct {
		const char *action, *accelerators[3];
	} alternatives[] = {
		{"win.grid", {"<Shift><Control>h", "<Control>apostrophe"}},
		{"win.snap", {"<Shift><Control>p", "<Shift><Control>semicolon"}},
		{"win.arrange-front", {"<Shift><Control>Home", "<Shift><Control>bracketright"}},
		{"win.arrange-forward", {"<Shift><Control>Up", "<Control>bracketright"}},
		{"win.arrange-backward", {"<Shift><Control>Down", "<Control>bracketleft"}},
		{"win.arrange-back", {"<Shift><Control>End", "<Shift><Control>bracketleft"}},
	};
	for (guint index = 0; index < G_N_ELEMENTS(alternatives); index++)
		gtk_application_set_accels_for_action(editor->application, alternatives[index].action,
											  alternatives[index].accelerators);
	gtk_application_set_accels_for_action(editor->application, "win.zoom-in",
										  (const char *[]){"<Control>plus", "<Control>equal", "<Control>KP_Add", NULL});
	gtk_application_set_accels_for_action(editor->application, "win.zoom-out",
										  (const char *[]){"<Control>minus", "<Control>KP_Subtract", NULL});
	gtk_application_set_accels_for_action(editor->application, "win.fill-foreground",
										  (const char *[]){"<Alt>BackSpace", "<Alt>Delete", NULL});
	gtk_application_set_accels_for_action(editor->application, "win.fill-background",
										  (const char *[]){"<Control>BackSpace", "<Control>Delete",
														   "<Control><Alt>BackSpace", "<Control><Alt>Delete", NULL});
}

static gboolean unparent_context_menu(gpointer popover) {
	gtk_widget_unparent(popover);
	g_object_unref(popover);
	return G_SOURCE_REMOVE;
}

/* A closing menu still activates the chosen item through its parent, so it is detached afterwards */
static void close_context_menu(GtkPopover *popover) {
	g_idle_add(unparent_context_menu, g_object_ref(popover));
}

void show_context_menu(GtkWidget *parent, GMenuModel *model, double x, double y) {
	GtkWidget *popover = gtk_popover_menu_new_from_model(model);
	gtk_widget_set_parent(popover, parent);
	gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
	gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
	gtk_widget_set_halign(popover, GTK_ALIGN_START);
	gtk_popover_set_pointing_to(GTK_POPOVER(popover), &(GdkRectangle){(int)x, (int)y, 1, 1});
	g_signal_connect(popover, "closed", G_CALLBACK(close_context_menu), NULL);
	gtk_popover_popup(GTK_POPOVER(popover));
}
