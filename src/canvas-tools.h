#pragma once

#include "editor.h"

/* Canvas interactions split by tool family, shared between the view and its tool files */

void draw_selection(GtkSnapshot *snapshot, DocumentView *view, double x, double y, const graphene_rect_t *visible);
void begin_selection_drag(DocumentView *view, double x, double y, GdkModifierType modifiers);
void update_selection_drag(DocumentView *view, double x, double y, GdkModifierType modifiers);
void finish_selection_drag(DocumentView *view, double x, double y, GdkModifierType modifiers);
void finish_wand_selection(DocumentView *view);
void drag_wand_selection(DocumentView *view, double x);
void show_canvas_menu(DocumentView *view, double x, double y);
void begin_move_drag(DocumentView *view, double x, double y, GdkModifierType modifiers);
void update_move_drag(DocumentView *view, double x, double y, GdkModifierType modifiers);
void finish_move_drag(DocumentView *view);
