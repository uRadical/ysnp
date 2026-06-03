#import <Cocoa/Cocoa.h>

#include "overlay.h"
#include "log.h"
#include "default_img.h"

/* NSImageView subclass that dismisses the overlay on click or Escape. */
@interface YSNPView : NSImageView
@end

@implementation YSNPView
- (void)mouseDown:(NSEvent *)e {
    (void)e;
    [NSApp terminate:nil];
}
- (void)keyDown:(NSEvent *)e {
    if (e.keyCode == 53) { /* Escape */
        [NSApp terminate:nil];
    }
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
@end

/* Strong, process-lifetime references. Under ARC the local setup variables
 * would otherwise be released when overlay_show() returns; pinning them here
 * keeps the window, view and image alive until the app terminates. */
static NSWindow *g_window;
static YSNPView *g_view;
static NSImage *g_image;

void overlay_show(const char *image_path) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    NSImage *img = nil;
    if (image_path) {
        img = [[NSImage alloc]
            initWithContentsOfFile:[NSString stringWithUTF8String:image_path]];
        if (!img) {
            ysnp_logf("could not load %s, using embedded default", image_path);
        }
    }
    if (!img) {
        /* NULL path, or the user file failed to load: fall back to embedded. */
        NSData *data = [NSData dataWithBytes:default_img_data
                                     length:default_img_len];
        img = [[NSImage alloc] initWithData:data];
    }
    if (!img) {
        ysnp_die("failed to load image");
    }

    /* Use the image's own pixel dimensions as the base size. */
    NSImageRep *rep = [[img representations] firstObject];
    CGFloat iw = rep ? (CGFloat)rep.pixelsWide : img.size.width;
    CGFloat ih = rep ? (CGFloat)rep.pixelsHigh : img.size.height;
    if (iw <= 0 || ih <= 0) {
        iw = img.size.width;
        ih = img.size.height;
    }

    /* If the image is larger than the screen, scale it down to fit while
     * preserving aspect ratio; otherwise show it at native size. */
    NSRect screen = [[NSScreen mainScreen] frame];
    CGFloat scale = 1.0;
    if (iw > screen.size.width || ih > screen.size.height) {
        scale = MIN(screen.size.width / iw, screen.size.height / ih);
    }
    CGFloat dw = iw * scale;
    CGFloat dh = ih * scale;

    /* Center the (possibly scaled) rect on the main screen. */
    NSRect frame = NSMakeRect(screen.origin.x + (screen.size.width - dw) / 2.0,
                              screen.origin.y + (screen.size.height - dh) / 2.0,
                              dw, dh);

    NSWindow *win = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:NSWindowStyleMaskBorderless
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    [win setOpaque:NO];
    [win setBackgroundColor:[NSColor clearColor]];
    [win setLevel:NSScreenSaverWindowLevel];
    [win setReleasedWhenClosed:NO];

    YSNPView *view = [[YSNPView alloc] initWithFrame:NSMakeRect(0, 0, dw, dh)];
    [view setImage:img];
    [view setImageScaling:NSImageScaleProportionallyUpOrDown];
    [win setContentView:view];

    /* Pin against premature release under ARC. */
    g_image = img;
    g_window = win;
    g_view = view;

    [win makeKeyAndOrderFront:nil];
    [win makeFirstResponder:view];
    [NSApp activateIgnoringOtherApps:YES];
}

void overlay_run(void) {
    [NSApp run];
}

void overlay_close(void) {
    [NSApp terminate:nil];
}
