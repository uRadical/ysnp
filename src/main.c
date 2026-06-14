#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overlay.h"
#include "image.h"
#include "giphy.h"

#ifndef YSNP_VERSION
#define YSNP_VERSION "unknown"
#endif

int main(int argc, char **argv) {
    int use_giphy = 0;

    if (argc > 1) {
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
            printf("ysnp %s\n", YSNP_VERSION);
            return 0;
        } else if (strcmp(argv[1], "--giphy") == 0 ||
                   strcmp(argv[1], "-g") == 0) {
            use_giphy = 1;
        } else {
            fprintf(stderr, "usage: ysnp [--version] [--giphy]\n");
            return 2;
        }
    }

    /* In --giphy mode the image is a temp file we own and must clean up;
     * a NULL fetch result falls through to the embedded default. */
    char *fetched = use_giphy ? giphy_fetch_random() : NULL;
    const char *img = use_giphy ? fetched : image_pick();

    overlay_show(img);
    overlay_run();

    if (fetched) {
        remove(fetched);
        free(fetched);
    }
    return 0;
}
