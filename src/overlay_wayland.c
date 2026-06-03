/* ysnp Wayland overlay — pure C, wlr-layer-shell + wl_shm + cairo.
 *
 * No EGL/OpenGL: we draw into a shared-memory buffer with cairo and attach it
 * to an unanchored layer-shell surface sized to the image, which the
 * compositor centers on the output.
 */

/* Standard headers first — in particular <stdio.h> must precede <jpeglib.h>,
 * which uses FILE/size_t but does not include the headers that define them. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <cairo/cairo.h>
#include <jpeglib.h>
#include <gif_lib.h>

#include "overlay.h"
#include "log.h"
#include "default_img.h"

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

/* Decoded image frames (one for static images, many for animated GIFs). */
static cairo_surface_t **frames;
static int *frame_delays; /* milliseconds per frame */
static int frame_count;
static int current_frame;
static int is_animated;

static int running = 1;

/* ---- timing ------------------------------------------------------------ */

static uint32_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t next_frame_time;

/* ---- in-memory byte stream --------------------------------------------- */

/* Consumed by the giflib reader (gif_mem_read) when decoding the embedded
 * default GIF straight out of the compiled-in byte array. */
struct mem_stream {
    const unsigned char *data;
    size_t pos;
    size_t len;
};

/* ---- JPEG -------------------------------------------------------------- */

/* Custom libjpeg error manager: longjmp out of a fatal decode instead of
 * letting the default handler call exit() on us. */
struct jpeg_err_ctx {
    struct jpeg_error_mgr pub;
    jmp_buf escape;
};

static void jpeg_on_error(j_common_ptr cinfo) {
    struct jpeg_err_ctx *ctx = (struct jpeg_err_ctx *)cinfo->err;
    longjmp(ctx->escape, 1);
}

static cairo_surface_t *load_jpeg(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }

    struct jpeg_decompress_struct cinfo;
    struct jpeg_err_ctx jerr;
    /* These are touched across setjmp/longjmp, so they must be volatile. */
    cairo_surface_t *volatile surf = NULL;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_on_error;

    if (setjmp(jerr.escape)) {
        /* A fatal libjpeg error jumped here: clean up and signal failure so
         * load_image falls back to the embedded default. */
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        if (surf) {
            cairo_surface_destroy(surf);
        }
        return NULL;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;

    surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        cairo_surface_destroy(surf);
        return NULL;
    }

    unsigned char *dst = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);

    JSAMPARRAY row = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE,
                                                (JDIMENSION)(w * 3), 1);

    while ((int)cinfo.output_scanline < h) {
        int y = (int)cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, row, 1);
        uint32_t *out = (uint32_t *)(dst + y * stride);
        const unsigned char *in = row[0];
        for (int x = 0; x < w; x++) {
            uint32_t r = in[x * 3 + 0];
            uint32_t g = in[x * 3 + 1];
            uint32_t b = in[x * 3 + 2];
            /* cairo ARGB32 is native-endian 0xAARRGGBB, premultiplied alpha. */
            out[x] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);

    cairo_surface_mark_dirty(surf);
    return surf;
}

/* ---- GIF --------------------------------------------------------------- */

/* giflib reader that pulls bytes from an in-memory buffer. */
static int gif_mem_read(GifFileType *gif, GifByteType *buf, int len) {
    struct mem_stream *s = gif->UserData;
    size_t remaining = s->len - s->pos;
    size_t n = (size_t)len < remaining ? (size_t)len : remaining;
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    return (int)n;
}

/* Composite all GIF frames onto a full-canvas ARGB buffer, honoring per-frame
 * offsets, transparency and disposal methods, and capture each as a cairo
 * surface. Delays come from the graphics-control extension (unit: 10ms).
 * Decodes from a file path, or from an in-memory buffer when path is NULL. */
static int load_gif(const char *path, const unsigned char *mem, size_t mem_len) {
    int err = 0;
    GifFileType *gif = NULL;
    struct mem_stream stream = {mem, 0, mem_len};

    if (path) {
        gif = DGifOpenFileName(path, &err);
    } else {
        gif = DGifOpen(&stream, gif_mem_read, &err);
    }

    if (!gif) {
        return 0;
    }

    if (DGifSlurp(gif) != GIF_OK) {
        DGifCloseFile(gif, &err);
        return 0;
    }

    int canvas_w = gif->SWidth;
    int canvas_h = gif->SHeight;
    int n = gif->ImageCount;
    if (n < 1) {
        DGifCloseFile(gif, &err);
        return 0;
    }

    frames = calloc((size_t)n, sizeof(*frames));
    frame_delays = calloc((size_t)n, sizeof(*frame_delays));
    if (!frames || !frame_delays) {
        ysnp_die("out of memory");
    }

    /* Persistent canvas (ARGB32) accumulated across frames. */
    uint32_t *canvas = calloc((size_t)canvas_w * canvas_h, sizeof(uint32_t));
    uint32_t *prev = calloc((size_t)canvas_w * canvas_h, sizeof(uint32_t));
    if (!canvas || !prev) {
        ysnp_die("out of memory");
    }

    ColorMapObject *global_map = gif->SColorMap;

    for (int i = 0; i < n; i++) {
        SavedImage *si = &gif->SavedImages[i];
        GifImageDesc *desc = &si->ImageDesc;
        ColorMapObject *cmap = desc->ColorMap ? desc->ColorMap : global_map;

        GraphicsControlBlock gcb;
        int transparent = NO_TRANSPARENT_COLOR;
        int disposal = DISPOSAL_UNSPECIFIED;
        int delay_cs = 0;
        if (DGifSavedExtensionToGCB(gif, i, &gcb) == GIF_OK) {
            transparent = gcb.TransparentColor;
            disposal = gcb.DisposalMode;
            delay_cs = gcb.DelayTime;
        }

        /* Snapshot canvas for possible "restore to previous" disposal. */
        memcpy(prev, canvas, (size_t)canvas_w * canvas_h * sizeof(uint32_t));

        /* Paint this frame's pixels onto the canvas. */
        for (int y = 0; y < desc->Height; y++) {
            int cy = desc->Top + y;
            if (cy < 0 || cy >= canvas_h) {
                continue;
            }
            for (int x = 0; x < desc->Width; x++) {
                int cx = desc->Left + x;
                if (cx < 0 || cx >= canvas_w) {
                    continue;
                }
                int idx = si->RasterBits[y * desc->Width + x];
                if (idx == transparent) {
                    continue; /* leave existing canvas pixel */
                }
                uint32_t argb = 0xFF000000u;
                if (cmap && idx < cmap->ColorCount) {
                    GifColorType c = cmap->Colors[idx];
                    argb = (0xFFu << 24) | ((uint32_t)c.Red << 16) |
                           ((uint32_t)c.Green << 8) | (uint32_t)c.Blue;
                }
                canvas[cy * canvas_w + cx] = argb;
            }
        }

        /* Capture current canvas state as this frame's cairo surface. */
        cairo_surface_t *surf =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, canvas_w, canvas_h);
        if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
            ysnp_die("out of memory");
        }
        unsigned char *dst = cairo_image_surface_get_data(surf);
        int stride = cairo_image_surface_get_stride(surf);
        for (int y = 0; y < canvas_h; y++) {
            memcpy(dst + y * stride, canvas + y * canvas_w,
                   (size_t)canvas_w * sizeof(uint32_t));
        }
        cairo_surface_mark_dirty(surf);
        frames[i] = surf;

        int delay_ms = delay_cs * 10;
        if (delay_ms < 20) {
            delay_ms = 20; /* clamp pathological/zero delays */
        }
        frame_delays[i] = delay_ms;

        /* Apply disposal for the next frame. */
        if (disposal == DISPOSE_BACKGROUND) {
            for (int y = 0; y < desc->Height; y++) {
                int cy = desc->Top + y;
                if (cy < 0 || cy >= canvas_h) {
                    continue;
                }
                for (int x = 0; x < desc->Width; x++) {
                    int cx = desc->Left + x;
                    if (cx < 0 || cx >= canvas_w) {
                        continue;
                    }
                    canvas[cy * canvas_w + cx] = 0;
                }
            }
        } else if (disposal == DISPOSE_PREVIOUS) {
            memcpy(canvas, prev, (size_t)canvas_w * canvas_h * sizeof(uint32_t));
        }
    }

    free(canvas);
    free(prev);
    DGifCloseFile(gif, &err);

    frame_count = n;
    is_animated = (n > 1);
    return 1;
}

/* ---- image dispatch by extension --------------------------------------- */

typedef enum { FMT_PNG, FMT_JPEG, FMT_GIF, FMT_UNKNOWN } img_format;

static int ends_with(const char *s, const char *suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcasecmp(s + ls - lf, suffix) == 0;
}

/* Identify the format from the file's magic bytes; the extension is only a
 * fallback, so a mislabeled file still decodes (matching macOS's NSImage). */
static img_format sniff_format(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return FMT_UNKNOWN;
    }
    unsigned char b[8] = {0};
    size_t n = fread(b, 1, sizeof(b), fp);
    fclose(fp);

    if (n >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
        return FMT_PNG;
    }
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
        return FMT_JPEG;
    }
    if (n >= 6 && memcmp(b, "GIF8", 4) == 0) {
        return FMT_GIF;
    }

    if (ends_with(path, ".gif")) return FMT_GIF;
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg")) return FMT_JPEG;
    return FMT_PNG; /* default assumption */
}

static void store_single(cairo_surface_t *surf) {
    if (!surf || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        ysnp_die("failed to decode image");
    }
    frames = malloc(sizeof(*frames));
    frame_delays = malloc(sizeof(*frame_delays));
    if (!frames || !frame_delays) {
        ysnp_die("out of memory");
    }
    frames[0] = surf;
    frame_delays[0] = 0;
    frame_count = 1;
    is_animated = 0;
}

static void load_image(const char *image_path) {
    if (!image_path) {
        /* Embedded default is an animated GIF. */
        if (!load_gif(NULL, default_img_data, default_img_len)) {
            ysnp_die("failed to decode embedded default image");
        }
        return;
    }

    switch (sniff_format(image_path)) {
    case FMT_GIF:
        if (!load_gif(image_path, NULL, 0)) {
            ysnp_die("failed to decode GIF");
        }
        return;
    case FMT_JPEG:
        store_single(load_jpeg(image_path));
        return;
    case FMT_PNG:
    case FMT_UNKNOWN:
    default:
        store_single(cairo_image_surface_create_from_png(image_path));
        return;
    }
}

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
    render_frame(frames[current_frame]);
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
        overlay_close();
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
        overlay_close();
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
        next_frame_time = get_time_ms() + (uint32_t)frame_delays[current_frame];
    }
}

static void layer_closed(void *d, struct zwlr_layer_surface_v1 *ls) {
    (void)d; (void)ls;
    overlay_close();
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

/* ---- public API -------------------------------------------------------- */

void overlay_show(const char *image_path) {
    load_image(image_path);

    display = wl_display_connect(NULL);
    if (!display) {
        ysnp_die("cannot connect to Wayland display");
    }

    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display); /* bind globals */

    if (!compositor) ysnp_die("no wl_compositor");
    if (!layer_shell) ysnp_die("no zwlr_layer_shell_v1 (need a wlroots compositor)");
    if (!shm) ysnp_die("no wl_shm");

    surface = wl_compositor_create_surface(compositor);
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "ysnp");
    if (!layer_surface) {
        ysnp_die("failed to create layer surface");
    }

    /* Size the surface to the image's own dimensions; with no edge anchors a
     * wlroots compositor centers the layer surface on the output. */
    surface_width = cairo_image_surface_get_width(frames[0]);
    surface_height = cairo_image_surface_get_height(frames[0]);

    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
    zwlr_layer_surface_v1_set_anchor(layer_surface, 0); /* unanchored = centered */
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, 1);
    zwlr_layer_surface_v1_set_size(layer_surface,
                                   (uint32_t)surface_width,
                                   (uint32_t)surface_height);

    wl_surface_commit(surface);
    wl_display_roundtrip(display); /* wait for configure */
}

void overlay_run(void) {
    int wl_fd = wl_display_get_fd(display);

    /* Deliberately the simple single-threaded dispatch loop: we poll one fd
     * and dispatch only on POLLIN, so the wl_display_prepare_read machinery
     * (which exists for multi-threaded dispatch) isn't needed here. A failed
     * flush is non-fatal for a one-shot overlay with negligible output. */
    while (running) {
        wl_display_flush(display);

        if (configured && is_animated) {
            uint32_t now_ms = get_time_ms();
            if (now_ms >= next_frame_time) {
                current_frame = (current_frame + 1) % frame_count;
                commit_frame();
                next_frame_time = now_ms + (uint32_t)frame_delays[current_frame];
            }
        }

        int timeout = -1;
        if (configured && is_animated) {
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
    for (int i = 0; i < frame_count; i++) {
        cairo_surface_destroy(frames[i]);
    }
    free(frames);
    free(frame_delays);

    destroy_buffer();
    if (layer_surface) zwlr_layer_surface_v1_destroy(layer_surface);
    if (surface) wl_surface_destroy(surface);
    wl_display_disconnect(display);
}

void overlay_close(void) {
    running = 0;
}
