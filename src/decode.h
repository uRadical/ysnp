#ifndef YSNP_DECODE_H
#define YSNP_DECODE_H

#include <cairo/cairo.h>

/* Decoded image frames (one for static images, many for animated GIFs),
 * shared by the Linux overlay backends. */
extern cairo_surface_t **ysnp_frames;
extern int *ysnp_frame_delays; /* milliseconds per frame */
extern int ysnp_frame_count;
extern int ysnp_is_animated;

/* Decode image_path (or the embedded default when NULL) into ysnp_frames.
 * Dies on decode failure. */
void ysnp_load_image(const char *image_path);
void ysnp_free_frames(void);

#endif /* YSNP_DECODE_H */
