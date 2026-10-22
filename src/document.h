#pragma once

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

#include "kernels.h"
#include "record.h"

#define DOCUMENT_MAX_PIXELS (64ULL * 1024 * 1024)
#define DOCUMENT_MAX_BYTES (512ULL * 1024 * 1024)

/* A tree node indexes the original bottom-to-top layer record stream */
typedef struct LayerTree LayerTree;

struct LayerTree {
	int depth, index, closing_index;
	Record *layer, *closing;
	LayerTree *parent;
	GPtrArray *children;
};

/* The product mode preserves source creation and composition gates */
typedef enum { DOCUMENT_RASTER, DOCUMENT_VECTOR } DocumentMode;

/* Native ownership keeps the original document record and its derived tree together */
typedef struct {
	Record *record;
	DocumentMode mode;
	LayerTree *root;
	GPtrArray *lookup;
	Record *root_layer, *root_closing;
	GHashTable *history_operations;
	GPtrArray *unsupported_features;
	gboolean preview;
	Record *grown_layer;
	/* The pixels an undo or redo changed, so the display redraws only them; empty means everything */
	RasterExtent changed;
	/* Set when an undo or redo changed the selection, and when that was all it changed */
	gboolean selection_changed, selection_only;
	guint history_limit;
	/* Counts history steps, undos and redos, since NP stops moving once the history is full */
	guint revision;
	/* Steps trimmed from the front of the history, so NP plus this counts every step and the saved step stays put */
	guint trimmed_steps;
	gint64 saved_step;
} Document;

Record *create_point(double x, double y);
Record *create_extent(double x, double y, double width, double height);
Record *create_transform(double xx, double yx, double xy, double yy, double x, double y);
Record *create_identity_transform(void);
void concatenate_transform(Record *transform, const Record *after);
void invert_transform(Record *transform);
Record *transform_point(const Record *transform, const Record *point);
Record *intersect_extents(const Record *first, const Record *second);
Record *union_extents(const Record *first, const Record *second);
Record *create_layer_record(void);
Record *create_mask(unsigned depth);
Record *create_vector_mask(void);
Record *copy_mask(const Record *mask);
Record *copy_vector_mask(const Record *mask);
Record *copy_path_records(const Record *path);
Record *copy_descriptor(const char *key, const Record *descriptor);
Record *copy_layer_record(const Record *layer);
void name_layer(Record *layer, const char *name);
const char *get_layer_name(const Record *layer);
gboolean is_layer_visible(const Record *layer);
gboolean is_layer_group(const Record *layer);
void set_layer_visible(Record *layer, gboolean visible);
Document *create_document_record(const char *name, DocumentMode mode);
Document *create_document_from_pixbuf(GdkPixbuf *pixels, const char *name, GError **error);
void free_document(Document *document);
Record *create_document_layer(Document *document, gboolean without_id);
Record *create_group_closing(Document *document, gboolean without_id);
double next_layer_id(Document *document);
gboolean rebuild_layer_tree(Document *document, GError **error);
gboolean replace_document_layers(Document *document, Record *layers, GError **error);
Record *get_active_layer(const Document *document);
gboolean get_valid_document_dimensions(const Document *document, int *width, int *height);
int document_width(const Document *document);
int document_height(const Document *document);
Document *create_transparent_document(int width, int height, unsigned depth, DocumentMode mode, const char *name,
									  GError **error);
Document *load_document(GFile *file, GCancellable *cancellable, GError **error);
RasterExtent get_tree_extent(const Document *document, const LayerTree *tree);
Record *compose_layer_region(const Document *document, const Record *layers, RasterExtent region, GError **error);
gboolean compose_raster_document(const Document *document, const Record *dirty, const Record *selection,
								 GError **error);
cairo_surface_t *render_surface(Document *document, GError **error);
gboolean render_move_preview(Document *document, const GArray *moving, Record *floating, cairo_surface_t **below,
							 cairo_surface_t **moving_surface, cairo_surface_t **above, RasterExtent *below_extent,
							 RasterExtent *moving_extent);
GdkPixbuf *create_document_pixbuf(Document *document, gboolean opaque, GError **error);
guint32 premultiply_pixel(guint32 pixel);
guint32 unpremultiply_pixel(guint32 pixel);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Document, free_document)
