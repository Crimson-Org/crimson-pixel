#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "common/key-store.h"
#include "editor.h"
#include "history.h"
#include "psd.h"
#include "../lang/en.h"

#define RECOVERY_INTERVAL_SECONDS (5 * 60)

/* A folder of another session that ended without cleaning up, locked while its documents open */
typedef struct {
	char *directory;
	int lock;
	guint pending;
} DeadSession;

/* One snapshot written by a worker, so editing continues meanwhile */
typedef struct {
	Document *copy;
	char *path;
} RecoveryWrite;

static char *session_directory;
static int session_lock = -1;
static guint next_slot;
/* Recovery files of open documents, and the ones a worker is writing */
static GHashTable *live_paths, *writing_paths;

/* ============================== Session folders ================================= */

static char *get_recovery_root(void) {
	return get_state_file_path("recovery");
}

/* An exclusive lock stays held until the process ends, even when it crashes, so a free lock means a dead session */
static int lock_session_directory(const char *directory) {
	g_autofree char *path = g_build_filename(directory, "lock", NULL);
	int lock = g_open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lock < 0)
		return -1;
	if (flock(lock, LOCK_EX | LOCK_NB) == 0)
		return lock;
	close(lock);
	return -1;
}

static void remove_session_directory(const char *directory) {
	g_autoptr(GDir) entries = g_dir_open(directory, 0, NULL);
	for (const char *name = entries ? g_dir_read_name(entries) : NULL; name; name = g_dir_read_name(entries)) {
		g_autofree char *path = g_build_filename(directory, name, NULL);
		g_unlink(path);
	}
	g_rmdir(directory);
}

static char *create_recovery_path(const char *name) {
	g_autofree char *file = g_strdup_printf("%u %s.psd", ++next_slot, name ? name : TEXT_UNTITLED);
	return g_build_filename(session_directory, file, NULL);
}

/* Recovery files are named by slot and document name its storage by name */
static char *read_recovered_name(const char *path) {
	g_autofree char *file = g_path_get_basename(path);
	const char *name = strchr(file, ' ');
	name = name ? name + 1 : file;
	return g_str_has_suffix(name, ".psd") ? g_strndup(name, strlen(name) - 4) : g_strdup(name);
}

/* ============================== Writing ========================================= */

static void free_recovery_write(RecoveryWrite *write) {
	free_document(write->copy);
	g_free(write->path);
	g_free(write);
}

static void write_recovery_file(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	RecoveryWrite *write = data;
	GError *error = NULL;
	if (!rebuild_layer_tree(write->copy, &error)) {
		g_task_return_error(task, error);
		return;
	}
	prepare_document_for_save(write->copy);
	g_autoptr(GByteArray) bytes = write_psd_document(write->copy, FALSE, FALSE, &error);
	/* Setting the contents writes a temporary file and renames it, so a crash keeps the last whole snapshot */
	if (!bytes || !g_file_set_contents(write->path, (const char *)bytes->data, bytes->len, &error))
		g_task_return_error(task, error);
	else
		g_task_return_boolean(task, TRUE);
}

static void finish_recovery_write(GObject *source, GAsyncResult *result, gpointer data) {
	RecoveryWrite *write = g_task_get_task_data(G_TASK(result));
	g_autoptr(GError) error = NULL;
	if (!g_task_propagate_boolean(G_TASK(result), &error))
		g_message("Recovery write failed: %s", error->message);
	g_hash_table_remove(writing_paths, write->path);
	/* A document closed while its snapshot was written leaves nothing behind */
	if (!g_hash_table_contains(live_paths, write->path))
		g_unlink(write->path);
}

static void keep_document(PresentedDocument *presented) {
	Document *document = presented->document;
	Record *record = document->record;
	if (get_number(record, "depth") != 8 || presented->recovered_revision == document->revision)
		return;
	if (presented->recovery_path && g_hash_table_contains(writing_paths, presented->recovery_path))
		return;
	if (document->unsupported_features && document->unsupported_features->len)
		return;
	presented->recovered_revision = document->revision;
	if (!presented->recovery_path) {
		presented->recovery_path = create_recovery_path(get_string(record, "name"));
		g_hash_table_add(live_paths, g_strdup(presented->recovery_path));
	}
	/* Native improvement: a saved document needs no snapshot, so its file goes instead of going stale */
	if (!is_document_modified(document, 0)) {
		g_unlink(presented->recovery_path);
		return;
	}
	RecoveryWrite *write = g_new0(RecoveryWrite, 1);
	write->copy = copy_document_for_save(document);
	write->path = g_strdup(presented->recovery_path);
	g_hash_table_add(writing_paths, g_strdup(write->path));
	g_autoptr(GTask) task = g_task_new(NULL, NULL, finish_recovery_write, NULL);
	g_task_set_task_data(task, write, (GDestroyNotify)free_recovery_write);
	g_task_run_in_thread(task, write_recovery_file);
}

static gboolean keep_documents(gpointer data) {
	for (GList *window = gtk_application_get_windows(data); window; window = window->next) {
		Editor *editor = g_object_get_data(G_OBJECT(window->data), "editor");
		if (!editor || editor->closing)
			continue;
		GtkNotebook *notebook = GTK_NOTEBOOK(editor->notebook);
		for (int index = 0; index < gtk_notebook_get_n_pages(notebook); index++) {
			PresentedDocument *presented = get_page_document(gtk_notebook_get_nth_page(notebook, index));
			if (presented)
				keep_document(presented);
		}
	}
	return G_SOURCE_CONTINUE;
}

void forget_recovery_file(PresentedDocument *presented) {
	if (!presented->recovery_path)
		return;
	g_hash_table_remove(live_paths, presented->recovery_path);
	if (!g_hash_table_contains(writing_paths, presented->recovery_path))
		g_unlink(presented->recovery_path);
	g_clear_pointer(&presented->recovery_path, g_free);
}

/* ============================== Recovering ====================================== */

static void release_dead_session(DeadSession *session) {
	if (--session->pending)
		return;
	remove_session_directory(session->directory);
	close(session->lock);
	g_free(session->directory);
	g_free(session);
}

void adopt_recovered_document(Editor *editor, GFile *file, PresentedDocument *presented) {
	DeadSession *session = g_object_get_data(G_OBJECT(file), "dead-session");
	if (!presented) {
		release_dead_session(session);
		return;
	}
	g_autofree char *old_path = g_file_get_path(file);
	g_autofree char *name = read_recovered_name(old_path);
	Record *record = presented->document->record;
	set_string(record, "name", name);
	mark_document_unsaved(presented->document);
	presented->recovered_revision = presented->document->revision;
	g_clear_object(&presented->file);
	presented->format = NULL;
	presented->recovery_path = create_recovery_path(name);
	g_hash_table_add(live_paths, g_strdup(presented->recovery_path));
	if (g_rename(old_path, presented->recovery_path) != 0)
		presented->recovered_revision--;
	release_dead_session(session);
	gtk_label_set_text(GTK_LABEL(editor->status), TEXT_RECOVERED_DOCUMENT);
}

static void recover_session(Editor *editor, const char *directory) {
	int lock = lock_session_directory(directory);
	if (lock < 0)
		return;
	DeadSession *session = g_new0(DeadSession, 1);
	*session = (DeadSession){g_strdup(directory), lock, 1};
	g_autoptr(GDir) entries = g_dir_open(directory, 0, NULL);
	for (const char *name = entries ? g_dir_read_name(entries) : NULL; name; name = g_dir_read_name(entries)) {
		if (!g_str_has_suffix(name, ".psd"))
			continue;
		g_autofree char *path = g_build_filename(directory, name, NULL);
		g_autoptr(GFile) file = g_file_new_for_path(path);
		g_object_set_data(G_OBJECT(file), "dead-session", session);
		session->pending++;
		open_recovered_file(editor, file);
	}
	release_dead_session(session);
}

/* Starts this process's session once and opens what crashed sessions left */
void start_document_recovery(Editor *editor) {
	if (session_directory)
		return;
	g_autofree char *root = get_recovery_root();
	g_autofree char *template = g_build_filename(root, "XXXXXX", NULL);
	if (g_mkdir_with_parents(root, 0700) != 0 || !g_mkdtemp(template)) {
		g_message("Recovery is unavailable: %s", g_strerror(errno));
		return;
	}
	session_lock = lock_session_directory(template);
	if (session_lock < 0) {
		g_rmdir(template);
		return;
	}
	session_directory = g_steal_pointer(&template);
	live_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	writing_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_timeout_add_seconds(RECOVERY_INTERVAL_SECONDS, keep_documents, editor->application);
	g_autoptr(GDir) sessions = g_dir_open(root, 0, NULL);
	for (const char *name = sessions ? g_dir_read_name(sessions) : NULL; name; name = g_dir_read_name(sessions)) {
		g_autofree char *directory = g_build_filename(root, name, NULL);
		if (strcmp(directory, session_directory) && g_file_test(directory, G_FILE_TEST_IS_DIR))
			recover_session(editor, directory);
	}
}

void stop_document_recovery(void) {
	if (!session_directory)
		return;
	remove_session_directory(session_directory);
	close(session_lock);
	g_clear_pointer(&session_directory, g_free);
}
