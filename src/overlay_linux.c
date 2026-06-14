/* ysnp Linux overlay dispatcher — native Wayland (wlr-layer-shell) when the
 * compositor supports it, otherwise an X11 override-redirect window, which
 * also covers GNOME via XWayland and plain X11 desktops. */

#include "overlay.h"
#include "overlay_backends.h"
#include "decode.h"

static enum { BACKEND_NONE, BACKEND_WAYLAND, BACKEND_X11 } backend;

void overlay_show(const char *image_path) {
    ysnp_load_image(image_path);

    if (ysnp_wl_try_show()) {
        backend = BACKEND_WAYLAND;
        return;
    }

    ysnp_x11_show();
    backend = BACKEND_X11;
}

void overlay_run(void) {
    if (backend == BACKEND_WAYLAND) {
        ysnp_wl_run();
    } else {
        ysnp_x11_run();
    }
    ysnp_free_frames();
}

void overlay_close(void) {
    /* Backend input handlers call ysnp_wl_close/ysnp_x11_close directly, so
     * this dispatcher can never race backend selection: a close arriving
     * before a backend is chosen (BACKEND_NONE) is a no-op. */
    if (backend == BACKEND_WAYLAND) {
        ysnp_wl_close();
    } else if (backend == BACKEND_X11) {
        ysnp_x11_close();
    }
}
