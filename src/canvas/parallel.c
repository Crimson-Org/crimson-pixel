#include "parallel.h"

/* Below this many pixels, giving rows to workers costs more than it saves */
enum { PARALLEL_MINIMUM_PIXELS = 1 << 16, PARALLEL_CHUNKS_PER_THREAD = 4 };

/* One split job, which every thread takes chunks of until none remain */
typedef struct {
	RowWork work;
	gpointer data;
	int first, count, chunk_rows;
	gint next_chunk, chunk_count, remaining_chunks;
	/* Late workers can arrive after the caller returned, so the last thread to leave frees the batch */
	gint references;
	GMutex lock;
	GCond finished;
} RowBatch;

static GThreadPool *workers;
static int worker_count;
static GPrivate inside_batch;

/* ============================== Batches ========================================= */

static void run_batch_chunks(RowBatch *batch) {
	g_private_set(&inside_batch, GINT_TO_POINTER(TRUE));
	int chunk;
	while ((chunk = g_atomic_int_add(&batch->next_chunk, 1)) < batch->chunk_count) {
		int first = batch->first + chunk * batch->chunk_rows;
		batch->work(first, MIN(first + batch->chunk_rows, batch->first + batch->count), batch->data);
		if (!g_atomic_int_dec_and_test(&batch->remaining_chunks))
			continue;
		g_mutex_lock(&batch->lock);
		g_cond_signal(&batch->finished);
		g_mutex_unlock(&batch->lock);
	}
	g_private_set(&inside_batch, GINT_TO_POINTER(FALSE));
}

static void release_batch(RowBatch *batch) {
	if (!g_atomic_int_dec_and_test(&batch->references))
		return;
	g_mutex_clear(&batch->lock);
	g_cond_clear(&batch->finished);
	g_free(batch);
}

static void run_worker(gpointer batch, gpointer unused) {
	run_batch_chunks(batch);
	release_batch(batch);
}

static void start_workers(void) {
	static gsize ready;
	if (!g_once_init_enter(&ready))
		return;
	worker_count = MAX(0, (int)g_get_num_processors() - 1);
	if (worker_count)
		workers = g_thread_pool_new(run_worker, NULL, worker_count, FALSE, NULL);
	g_once_init_leave(&ready, 1);
}

void run_rows_in_parallel(int first, int count, gsize row_pixels, RowWork work, gpointer data) {
	if (count <= 0)
		return;
	start_workers();
	if (!workers || count < 2 || (gsize)count * row_pixels < PARALLEL_MINIMUM_PIXELS || g_private_get(&inside_batch)) {
		work(first, first + count, data);
		return;
	}
	RowBatch *batch = g_new0(RowBatch, 1);
	batch->work = work;
	batch->data = data;
	batch->first = first;
	batch->count = count;
	int wanted = (worker_count + 1) * PARALLEL_CHUNKS_PER_THREAD;
	batch->chunk_rows = MAX(1, (count + wanted - 1) / wanted);
	batch->chunk_count = batch->remaining_chunks = (count + batch->chunk_rows - 1) / batch->chunk_rows;
	int helpers = MIN(worker_count, batch->chunk_count - 1);
	batch->references = helpers + 1;
	g_mutex_init(&batch->lock);
	g_cond_init(&batch->finished);
	for (int helper = 0; helper < helpers; helper++)
		g_thread_pool_push(workers, batch, NULL);
	run_batch_chunks(batch);
	g_mutex_lock(&batch->lock);
	while (g_atomic_int_get(&batch->remaining_chunks) > 0)
		g_cond_wait(&batch->finished, &batch->lock);
	g_mutex_unlock(&batch->lock);
	release_batch(batch);
}
