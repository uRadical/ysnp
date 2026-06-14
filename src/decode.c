/* ysnp image decoding — JPEG (libjpeg), GIF (giflib, animated) and PNG
 * (cairo) into an array of cairo image surfaces shared by the Linux overlay
 * backends. */

/* Standard headers first — in particular <stdio.h> must precede <jpeglib.h>,
 * which uses FILE/size_t but does not include the headers that define them. */
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <cairo/cairo.h>
#include <jpeglib.h>
#include <gif_lib.h>

#include "decode.h"
#include "log.h"
#include "default_img.h"

/* Reject absurd image dimensions early: no display needs them, and they
 * would otherwise make allocation sizes attacker-controlled (a hostile GIF
 * header can claim 65535x65535, which overcommit may happily grant). */
#define YSNP_MAX_DIM 16384

/* ---- shared frame storage ---------------------------------------------- */

cairo_surface_t **ysnp_frames;
int *ysnp_frame_delays;
int ysnp_frame_count;
int ysnp_is_animated;

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
         * ysnp_load_image falls back to the embedded default. */
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
    if (w <= 0 || h <= 0 || w > YSNP_MAX_DIM || h > YSNP_MAX_DIM) {
        jpeg_destroy_decompress(&cinfo);
        fclose(fp);
        return NULL;
    }

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
        uint32_t *out = (uint32_t *)(dst + (size_t)y * (size_t)stride);
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
    if (n < 1 || canvas_w <= 0 || canvas_h <= 0 ||
        canvas_w > YSNP_MAX_DIM || canvas_h > YSNP_MAX_DIM) {
        DGifCloseFile(gif, &err);
        return 0;
    }

    /* Validate every frame before compositing: a hostile file can declare a
     * 65535x65535 frame on a tiny canvas, which would otherwise send the
     * paint loop through ~4 billion clamp-and-skip iterations (CPU DoS). */
    for (int i = 0; i < n; i++) {
        GifImageDesc *d = &gif->SavedImages[i].ImageDesc;
        if (d->Width <= 0 || d->Height <= 0 ||
            d->Width > YSNP_MAX_DIM || d->Height > YSNP_MAX_DIM ||
            !gif->SavedImages[i].RasterBits) {
            DGifCloseFile(gif, &err);
            return 0;
        }
    }

    ysnp_frames = calloc((size_t)n, sizeof(*ysnp_frames));
    ysnp_frame_delays = calloc((size_t)n, sizeof(*ysnp_frame_delays));
    if (!ysnp_frames || !ysnp_frame_delays) {
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
                /* size_t arithmetic: with huge attacker-controlled frame
                 * dimensions (and calloc succeeding via overcommit), the
                 * int product could overflow into a negative index. */
                int idx = si->RasterBits[(size_t)y * (size_t)desc->Width + (size_t)x];
                if (idx == transparent) {
                    continue; /* leave existing canvas pixel */
                }
                uint32_t argb = 0xFF000000u;
                if (cmap && idx < cmap->ColorCount) {
                    GifColorType c = cmap->Colors[idx];
                    argb = (0xFFu << 24) | ((uint32_t)c.Red << 16) |
                           ((uint32_t)c.Green << 8) | (uint32_t)c.Blue;
                }
                canvas[(size_t)cy * (size_t)canvas_w + (size_t)cx] = argb;
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
            memcpy(dst + (size_t)y * (size_t)stride,
                   canvas + (size_t)y * (size_t)canvas_w,
                   (size_t)canvas_w * sizeof(uint32_t));
        }
        cairo_surface_mark_dirty(surf);
        ysnp_frames[i] = surf;

        int delay_ms = delay_cs * 10;
        if (delay_ms < 20) {
            delay_ms = 20; /* clamp pathological/zero delays */
        }
        ysnp_frame_delays[i] = delay_ms;

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
                    canvas[(size_t)cy * (size_t)canvas_w + (size_t)cx] = 0;
                }
            }
        } else if (disposal == DISPOSE_PREVIOUS) {
            memcpy(canvas, prev, (size_t)canvas_w * canvas_h * sizeof(uint32_t));
        }
    }

    free(canvas);
    free(prev);
    DGifCloseFile(gif, &err);

    ysnp_frame_count = n;
    ysnp_is_animated = (n > 1);
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
    ysnp_frames = malloc(sizeof(*ysnp_frames));
    ysnp_frame_delays = malloc(sizeof(*ysnp_frame_delays));
    if (!ysnp_frames || !ysnp_frame_delays) {
        ysnp_die("out of memory");
    }
    ysnp_frames[0] = surf;
    ysnp_frame_delays[0] = 0;
    ysnp_frame_count = 1;
    ysnp_is_animated = 0;
}

/* ---- public API -------------------------------------------------------- */

void ysnp_load_image(const char *image_path) {
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

void ysnp_free_frames(void) {
    for (int i = 0; i < ysnp_frame_count; i++) {
        cairo_surface_destroy(ysnp_frames[i]);
    }
    free(ysnp_frames);
    free(ysnp_frame_delays);
    ysnp_frames = NULL;
    ysnp_frame_delays = NULL;
    ysnp_frame_count = 0;
    ysnp_is_animated = 0;
}
