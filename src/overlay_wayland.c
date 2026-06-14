/* ysnp Wayland overlay — pure C, wlr-layer-shell + wl_shm + cairo.
 *
 * No EGL/OpenGL: we draw into a shared-memory buffer with cairo and attach it
 * to an unanchored layer-shell surface sized to the image, which the
 * compositor centers on the output.
 *
 * Image decoding lives in decode.c; frames are already loaded when
 * ysnp_wl_try_show() is called. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <cairo/cairo.h>

#include "overlay_backends.h"
#include "decode.h"
#include "log.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ---- globals ----------------------------------------------------------- */

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;

static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;

static struct wl_buffer *buffer;
static void *buffer_data;
static int buffer_stride;
static size_t buffer_size; /* bytes mmap'd, retained so cleanup can munmap */

static int32_t surface_width;
static int32_t surface_height;
static int configured;

static int current_frame;

static int running = 1;

/* ---- timing ------------------------------------------------------------ */

static uint32_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t next_frame_time;

/* ---- shm buffer -------------------------------------------------------- */

static int create_shm_file(size_t size) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir) {
        dir = "/tmp";
    }
    size_t name_len = strlen(dir) + strlen("/ysnp-XXXXXX") + 1;
    char *name = malloc(name_len);
    if (!name) {
        ysnp_die("out of memory");
    }
    snprintf(name, name_len, "%s/ysnp-XXXXXX", dir);

    int fd = mkstemp(name);
    if (fd < 0) {
        free(name);
        ysnp_die("mkstemp failed");
    }
    unlink(name);
    free(name);

    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        ysnp_die("ftruncate failed");
    }
    return fd;
}

static void destroy_buffer(void) {
    if (buffer) {
        wl_buffer_destroy(buffer);
        buffer = NULL;
    }
    if (buffer_data && buffer_data != MAP_FAILED) {
        munmap(buffer_data, buffer_size);
        buffer_data = NULL;
    }
    buffer_size = 0;
}

static void create_buffer(void) {
    if (surface_width <= 0 || surface_height <= 0) {
        ysnp_die("invalid surface dimensions");
    }
    /* Compute in size_t throughout so a large image can't overflow the
     * intermediate int multiply. */
    size_t stride = (size_t)surface_width * 4;
    buffer_stride = (int)stride;
    buffer_size = stride * (size_t)surface_height;

    int fd = create_shm_file(buffer_size);
    buffer_data = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer_data == MAP_FAILED) {
        close(fd);
        ysnp_die("mmap failed");
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)buffer_size);
    buffer = wl_shm_pool_create_buffer(pool, 0, surface_width, surface_height,
                                       buffer_stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
}

/* ---- rendering --------------------------------------------------------- */

static void render_frame(cairo_surface_t *frame) {
    cairo_surface_t *target = cairo_image_surface_create_for_data(
        buffer_data, CAIRO_FORMAT_ARGB32, surface_width, surface_height,
        buffer_stride);
    cairo_t *cr = cairo_create(target);

    /* Transparent background (surface is sized to the image, so this only
     * matters for any letterbox margin or GIF transparency). */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    int img_width = cairo_image_surface_get_width(frame);
    int img_height = cairo_image_surface_get_height(frame);
    if (img_width <= 0 || img_height <= 0) {
        /* Malformed frame: leave the cleared (transparent) buffer as-is. */
        cairo_destroy(cr);
        cairo_surface_destroy(target);
        return;
    }

    double scale = MIN((double)surface_width / img_width,
                       (double)surface_height / img_height);
    double x = (surface_width - img_width * scale) / 2.0;
    double y = (surface_height - img_height * scale) / 2.0;

    cairo_translate(cr, x, y);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, frame, 0, 0);
    cairo_paint(cr);

    cairo_destroy(cr);
    cairo_surface_destroy(target);
}

static void commit_frame(void) {
    render_frame(ysnp_frames[current_frame]);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(surface);
}

/* ---- keyboard listener ------------------------------------------------- */

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int fd, uint32_t sz) {
    (void)d; (void)k; (void)fmt; (void)sz;
    close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf,
                     struct wl_array *keys) {
    (void)d; (void)k; (void)s; (void)sf; (void)keys;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) {
    (void)d; (void)k; (void)s; (void)sf;
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t,
                   uint32_t key, uint32_t state) {
    (void)d; (void)k; (void)s; (void)t;
    /* key code 1 == Escape (Linux evdev). */
    if (key == 1 && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        ysnp_wl_close();
    }
}
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t s, uint32_t md,
                         uint32_t ml, uint32_t lk, uint32_t grp) {
    (void)d; (void)k; (void)s; (void)md; (void)ml; (void)lk; (void)grp;
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)d; (void)k; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat,
};

/* ---- pointer listener -------------------------------------------------- */

static void pt_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf,
                     wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)s; (void)sf; (void)x; (void)y;
}
static void pt_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf) {
    (void)d; (void)p; (void)s; (void)sf;
}
static void pt_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)p; (void)t; (void)x; (void)y;
}
static void pt_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t,
                      uint32_t button, uint32_t state) {
    (void)d; (void)p; (void)s; (void)t; (void)button;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        ysnp_wl_close();
    }
}
static void pt_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {
    (void)d; (void)p; (void)t; (void)axis; (void)v;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pt_enter,
    .leave = pt_leave,
    .motion = pt_motion,
    .button = pt_button,
    .axis = pt_axis,
};

/* ---- seat listener ----------------------------------------------------- */

static void seat_capabilities(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *name) {
    (void)d; (void)s; (void)name;
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ---- layer surface listener -------------------------------------------- */

static void layer_configure(void *d, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t w, uint32_t h) {
    (void)d;
    /* A configure with 0 dimensions means "you choose"; keep our requested
     * image size in that case. */
    int32_t new_w = (w > 0) ? (int32_t)w : surface_width;
    int32_t new_h = (h > 0) ? (int32_t)h : surface_height;

    zwlr_layer_surface_v1_ack_configure(ls, serial);

    /* The compositor may reconfigure us (output resize, scale change, …).
     * Only rebuild the buffer when the geometry actually changes — otherwise
     * we'd leak the old mmap/wl_buffer on every reconfigure. */
    if (configured && new_w == surface_width && new_h == surface_height) {
        return;
    }

    surface_width = new_w;
    surface_height = new_h;
    destroy_buffer();
    create_buffer();
    commit_frame();

    if (!configured) {
        configured = 1;
        next_frame_time = get_time_ms() + (uint32_t)ysnp_frame_delays[current_frame];
    }
}

static void layer_closed(void *d, struct zwlr_layer_surface_v1 *ls) {
    (void)d; (void)ls;
    ysnp_wl_close();
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

/* ---- registry listener ------------------------------------------------- */

static void registry_global(void *d, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
    (void)d; (void)version;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        /* Bind at version 1: a higher-version wl_pointer/wl_keyboard emits
         * events (frame, axis_source, repeat_info, …) that libwayland would
         * dispatch to our NULL listener slots and crash. v1 emits only the
         * events we actually handle (enter/leave/motion/button/axis, key). */
        seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}
static void registry_remove(void *d, struct wl_registry *reg, uint32_t name) {
    (void)d; (void)reg; (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

/* ---- backend API ------------------------------------------------------- */

/* Tear down a partial connection so the X11 backend starts from a clean
 * slate. wl_display_disconnect releases everything server-side. */
static void disconnect_display(void) {
    wl_display_disconnect(display);
    display = NULL;
    registry = NULL;
    compositor = NULL;
    layer_shell = NULL;
    shm = NULL;
    seat = NULL;
    pointer = NULL;
    keyboard = NULL;
}

int ysnp_wl_try_show(void) {
    running = 1;

    display = wl_display_connect(NULL);
    if (!display) {
        return 0; /* no Wayland at all — X11 session or bare console */
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display); /* bind globals */

    /* Missing layer-shell is the expected GNOME/KDE case; missing
     * wl_compositor or wl_shm would be a bizarre compositor, but the X11
     * fallback is still worth a shot there. */
    if (!compositor || !layer_shell || !shm) {
        disconnect_display();
        return 0;
    }

    surface = wl_compositor_create_surface(compositor);
    if (!surface) {
        disconnect_display();
        return 0;
    }
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "ysnp");
    if (!layer_surface) {
        ysnp_die("failed to create layer surface");
    }

    /* Size the surface to the image's own dimensions; with no edge anchors a
     * wlroots compositor centers the layer surface on the output. */
    surface_width = cairo_image_surface_get_width(ysnp_frames[0]);
    surface_height = cairo_image_surface_get_height(ysnp_frames[0]);

    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
    zwlr_layer_surface_v1_set_anchor(layer_surface, 0); /* unanchored = centered */
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 1);
    zwlr_layer_surface_v1_set_size(layer_surface,
                                   (uint32_t)surface_width,
                                   (uint32_t)surface_height);

    wl_surface_commit(surface);
    wl_display_roundtrip(display); /* wait for configure */
    return 1;
}

void ysnp_wl_run(void) {
    int wl_fd = wl_display_get_fd(display);

    /* Deliberately the simple single-threaded dispatch loop: we poll one fd
     * and dispatch only on POLLIN, so the wl_display_prepare_read machinery
     * (which exists for multi-threaded dispatch) isn't needed here. A failed
     * flush is non-fatal for a one-shot overlay with negligible output. */
    while (running) {
        wl_display_flush(display);

        if (configured && ysnp_is_animated) {
            uint32_t now_ms = get_time_ms();
            if (now_ms >= next_frame_time) {
                current_frame = (current_frame + 1) % ysnp_frame_count;
                commit_frame();
                next_frame_time = now_ms + (uint32_t)ysnp_frame_delays[current_frame];
            }
        }

        int timeout = -1;
        if (configured && ysnp_is_animated) {
            uint32_t now_ms = get_time_ms();
            timeout = (next_frame_time > now_ms)
                          ? (int)(next_frame_time - now_ms)
                          : 0;
        }

        struct pollfd pfd = {wl_fd, POLLIN, 0};
        if (poll(&pfd, 1, timeout) < 0 && errno != EINTR) {
            break;
        }

        if (pfd.revents & POLLIN) {
            if (wl_display_dispatch(display) < 0) {
                break;
            }
        }
    }

    /* Cleanup. */
    destroy_buffer();
    if (layer_surface) zwlr_layer_surface_v1_destroy(layer_surface);
    if (surface) wl_surface_destroy(surface);
    disconnect_display();
}

void ysnp_wl_close(void) {
    running = 0;
}
