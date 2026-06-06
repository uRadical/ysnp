/* ysnp X11 overlay — override-redirect ARGB window + cairo-xlib.
 *
 * Fallback for compositors without wlr-layer-shell (GNOME, KDE), where it
 * runs under XWayland, and for plain X11 desktops. An override-redirect
 * window is unmanaged: the WM adds no decorations and maps it above regular
 * windows, which is as close to layer-shell's overlay layer as core X11
 * gets. Frames are decoded by decode.c before ysnp_x11_show() is called.
 *
 * Exception: under WSLg, only *managed* top-levels get a Windows-side window
 * (the RAIL shell never surfaces override-redirect windows), so on WSL we
 * create a normal window instead and strip its decorations via Motif hints. */

#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/Xrandr.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include "overlay.h"
#include "overlay_backends.h"
#include "decode.h"
#include "log.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ---- globals ----------------------------------------------------------- */

static Display *dpy;
static Window win;
static cairo_surface_t *win_surface;

static int win_w;
static int win_h;

static Window prev_focus;
static int prev_focus_revert;
static int focused;

static int managed; /* WSLg: normal managed window instead of override-redirect */
static Atom wm_delete_window;

static int current_frame;
static int running = 1;

/* ---- timing ------------------------------------------------------------ */

static uint32_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t next_frame_time;

/* ---- WSL detection ------------------------------------------------------ */

static int running_under_wsl(void) {
    if (getenv("WSL_DISTRO_NAME") || getenv("WSL_INTEROP")) {
        return 1;
    }
    /* Fallback for sessions that scrub the env (e.g. launched from a hook):
     * the WSL kernel always identifies itself in /proc/version. */
    FILE *fp = fopen("/proc/version", "r");
    if (!fp) {
        return 0;
    }
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    (void)n;
    return strstr(buf, "microsoft") != NULL || strstr(buf, "Microsoft") != NULL;
}

/* ---- X error handling --------------------------------------------------- */

/* The default Xlib handler exits the process. Races like focusing a window
 * the WM just unmapped are harmless here, so log and carry on instead. */
static int on_x_error(Display *d, XErrorEvent *e) {
    char text[128];
    XGetErrorText(d, e->error_code, text, sizeof(text));
    ysnp_logf("X error (ignored): %s", text);
    return 0;
}

/* ---- monitor geometry --------------------------------------------------- */

/* Geometry of the primary monitor (or the first one), so the overlay centers
 * on one screen instead of straddling a multi-monitor virtual desktop.
 * Falls back to the full X screen if RandR 1.5 is unavailable. */
static void monitor_geometry(int *mx, int *my, int *mw, int *mh) {
    int screen = DefaultScreen(dpy);
    *mx = 0;
    *my = 0;
    *mw = DisplayWidth(dpy, screen);
    *mh = DisplayHeight(dpy, screen);

    int ev_base, err_base, major, minor;
    if (!XRRQueryExtension(dpy, &ev_base, &err_base) ||
        !XRRQueryVersion(dpy, &major, &minor) ||
        (major * 100 + minor) < 105) {
        return;
    }

    int n = 0;
    XRRMonitorInfo *mons = XRRGetMonitors(dpy, DefaultRootWindow(dpy), True, &n);
    if (!mons) {
        return;
    }
    if (n > 0) {
        int pick = 0;
        for (int i = 0; i < n; i++) {
            if (mons[i].primary) {
                pick = i;
                break;
            }
        }
        *mx = mons[pick].x;
        *my = mons[pick].y;
        *mw = mons[pick].width;
        *mh = mons[pick].height;
    }
    XRRFreeMonitors(mons);
}

/* ---- rendering --------------------------------------------------------- */

static void render_frame(cairo_surface_t *frame) {
    cairo_t *cr = cairo_create(win_surface);

    /* Group: composite the clear + paint offscreen, then blit once, so an
     * animation step never shows a half-drawn window. */
    cairo_push_group(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    int img_width = cairo_image_surface_get_width(frame);
    int img_height = cairo_image_surface_get_height(frame);
    if (img_width > 0 && img_height > 0) {
        double scale = MIN((double)win_w / img_width,
                           (double)win_h / img_height);
        double x = (win_w - img_width * scale) / 2.0;
        double y = (win_h - img_height * scale) / 2.0;

        cairo_translate(cr, x, y);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, frame, 0, 0);
        cairo_paint(cr);
    }

    cairo_pop_group_to_source(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);

    cairo_surface_flush(win_surface);
    XFlush(dpy);
}

/* ---- backend API ------------------------------------------------------- */

void ysnp_x11_show(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        ysnp_die("cannot connect to Wayland (no layer-shell) or X11 display");
    }
    XSetErrorHandler(on_x_error);

    int screen = DefaultScreen(dpy);
    Window root = DefaultRootWindow(dpy);

    /* 32-bit ARGB visual for a translucent window; fall back to the default
     * visual (opaque background) on servers without one. */
    XVisualInfo vinfo;
    if (!XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo)) {
        vinfo.visual = DefaultVisual(dpy, screen);
        vinfo.depth = DefaultDepth(dpy, screen);
    }

    int mx, my, mw, mh;
    monitor_geometry(&mx, &my, &mw, &mh);

    /* If the image is larger than the monitor, scale it down to fit while
     * preserving aspect ratio; otherwise show it at native size. */
    int iw = cairo_image_surface_get_width(ysnp_frames[0]);
    int ih = cairo_image_surface_get_height(ysnp_frames[0]);
    if (iw <= 0 || ih <= 0) {
        ysnp_die("invalid image dimensions");
    }
    double scale = 1.0;
    if (iw > mw || ih > mh) {
        scale = MIN((double)mw / iw, (double)mh / ih);
    }
    win_w = (int)(iw * scale);
    win_h = (int)(ih * scale);

    int x = mx + (mw - win_w) / 2;
    int y = my + (mh - win_h) / 2;

    managed = running_under_wsl();

    XSetWindowAttributes attrs;
    attrs.override_redirect = managed ? False : True;
    attrs.colormap = XCreateColormap(dpy, root, vinfo.visual, AllocNone);
    attrs.background_pixel = 0; /* transparent under a compositor */
    attrs.border_pixel = 0;     /* required: depth differs from the parent */
    attrs.event_mask = ExposureMask | ButtonPressMask | KeyPressMask;

    win = XCreateWindow(dpy, root, x, y, (unsigned)win_w, (unsigned)win_h, 0,
                        vinfo.depth, InputOutput, vinfo.visual,
                        CWOverrideRedirect | CWColormap | CWBackPixel |
                            CWBorderPixel | CWEventMask,
                        &attrs);

    XStoreName(dpy, win, "ysnp");
    XClassHint class_hint = {(char *)"ysnp", (char *)"ysnp"};
    XSetClassHint(dpy, win, &class_hint);

    if (managed) {
        /* Undecorated: Motif hints (flags = MWM_HINTS_DECORATIONS,
         * decorations = 0) — ancient, but the one hint every WM and WSLg's
         * RAIL shell honor. */
        Atom motif = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
        long hints[5] = {2, 0, 0, 0, 0};
        XChangeProperty(dpy, win, motif, motif, 32, PropModeReplace,
                        (unsigned char *)hints, 5);

        /* Splash type + above state approximate the overlay layer. */
        Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
        Atom splash = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
        XChangeProperty(dpy, win, type, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)&splash, 1);
        Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
        Atom above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
        XChangeProperty(dpy, win, state, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)&above, 1);

        /* Pin the geometry so the WM honors our position and can't resize
         * the window out from under the cairo surface. */
        XSizeHints *size = XAllocSizeHints();
        if (size) {
            size->flags = PPosition | PMinSize | PMaxSize;
            size->x = x;
            size->y = y;
            size->min_width = size->max_width = win_w;
            size->min_height = size->max_height = win_h;
            XSetWMNormalHints(dpy, win, size);
            XFree(size);
        }

        /* Dismiss instead of dying when the close button / Alt-F4 hits us. */
        wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(dpy, win, &wm_delete_window, 1);
    }

    win_surface = cairo_xlib_surface_create(dpy, win, vinfo.visual, win_w, win_h);
    if (cairo_surface_status(win_surface) != CAIRO_STATUS_SUCCESS) {
        ysnp_die("failed to create cairo X11 surface");
    }

    XMapRaised(dpy, win);
    XFlush(dpy);

    next_frame_time = get_time_ms() + (uint32_t)ysnp_frame_delays[current_frame];
}

static void take_focus(void) {
    /* Grab keyboard focus so Escape dismisses (matching layer-shell's
     * keyboard_interactivity), remembering the old focus to restore on exit.
     * Done on first Expose: the window is certainly viewable then, and
     * XSetInputFocus on an unviewable window is a BadMatch. Managed windows
     * (WSLg) get focus from the WM on activation; stealing it manually would
     * fight the RAIL shell's Windows-side focus tracking. */
    if (focused || managed) {
        return;
    }
    XGetInputFocus(dpy, &prev_focus, &prev_focus_revert);
    XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
    focused = 1;
}

void ysnp_x11_run(void) {
    int x_fd = ConnectionNumber(dpy);

    /* Same single-fd poll loop as the Wayland backend: dispatch X events,
     * advance GIF animation on a timeout. */
    while (running) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose:
                if (ev.xexpose.count == 0) {
                    render_frame(ysnp_frames[current_frame]);
                    take_focus();
                }
                break;
            case ButtonPress:
                overlay_close();
                break;
            case KeyPress:
                if (XLookupKeysym(&ev.xkey, 0) == XK_Escape) {
                    overlay_close();
                }
                break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_delete_window) {
                    overlay_close();
                }
                break;
            }
        }
        if (!running) {
            break;
        }

        int timeout = -1;
        if (ysnp_is_animated) {
            uint32_t now_ms = get_time_ms();
            if (now_ms >= next_frame_time) {
                current_frame = (current_frame + 1) % ysnp_frame_count;
                render_frame(ysnp_frames[current_frame]);
                next_frame_time = now_ms + (uint32_t)ysnp_frame_delays[current_frame];
            }
            now_ms = get_time_ms();
            timeout = (next_frame_time > now_ms)
                          ? (int)(next_frame_time - now_ms)
                          : 0;
        }

        struct pollfd pfd = {x_fd, POLLIN, 0};
        poll(&pfd, 1, timeout);
    }

    /* Cleanup. */
    if (focused) {
        XSetInputFocus(dpy, prev_focus, prev_focus_revert, CurrentTime);
    }
    cairo_surface_destroy(win_surface);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
}

void ysnp_x11_close(void) {
    running = 0;
}
