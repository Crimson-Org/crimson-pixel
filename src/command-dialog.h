#pragma once

#include "editor.h"

/* One open command dialog with its Cancel and OK handlers; each dialog's own state starts with it */
typedef struct CommandDialog CommandDialog;

struct CommandDialog {
	Editor *editor;
	GtkWidget *window, *grid, *message;
	gboolean (*accept)(CommandDialog *dialog);
	void (*cancel)(CommandDialog *dialog);
};

CommandDialog *create_command_dialog(Editor *editor, const char *title, gsize size);
GtkWidget *add_number_input(CommandDialog *dialog, const char *name, const char *unit, double minimum, double maximum,
							double value, int row);
void show_dialog_message(CommandDialog *dialog, const GError *error);
void press_ok(GtkButton *button, CommandDialog *dialog);
void refresh_command_page(Editor *editor, gboolean render);
Document *get_command_document(Editor *editor);
