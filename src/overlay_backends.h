#ifndef YSNP_OVERLAY_BACKENDS_H
#define YSNP_OVERLAY_BACKENDS_H

/* Internal Linux backend entry points, dispatched by overlay_linux.c.
 * Frames must already be decoded (ysnp_load_image) before *_show is called. */

/* Returns 1 with the overlay mapped on success; 0 when Wayland is unusable
 * (no display, or the compositor lacks wlr-layer-shell) so the caller can
 * fall back to X11. */
int ysnp_wl_try_show(void);
void ysnp_wl_run(void);
void ysnp_wl_close(void);

/* Dies on failure: X11 is the last resort. */
void ysnp_x11_show(void);
void ysnp_x11_run(void);
void ysnp_x11_close(void);

#endif /* YSNP_OVERLAY_BACKENDS_H */
