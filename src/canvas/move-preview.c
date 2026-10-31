#include "move-preview.h"

gboolean fill_move_preview(MovePreview *preview, Document *document, const GArray *layers, Record *floating) {
	clear_move_preview(preview);
	cairo_surface_t *parts[MOVE_PARTS];
	gboolean rendered = render_move_preview(document, layers, floating, &parts[MOVE_PART_BELOW],
											&parts[MOVE_PART_MOVING], &parts[MOVE_PART_ABOVE], &preview->below_extent,
											&preview->moving_extent);
	for (int part = 0; part < MOVE_PARTS; part++) {
		if (rendered && parts[part])
			preview->parts[part] = create_tiled_image(parts[part]);
		if (parts[part])
			cairo_surface_destroy(parts[part]);
	}
	preview->whole = rendered && !parts[MOVE_PART_MOVING];
	return rendered;
}

void clear_move_preview(MovePreview *preview) {
	for (int part = 0; part < MOVE_PARTS; part++)
		g_clear_pointer(&preview->parts[part], free_tiled_image);
	preview->whole = FALSE;
}

gboolean has_move_preview(const MovePreview *preview) {
	return preview->parts[MOVE_PART_MOVING] || preview->whole;
}
