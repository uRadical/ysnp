#include "overlay.h"
#include "image.h"

int main(void) {
    const char *img = image_pick();
    overlay_show(img);
    overlay_run();
    return 0;
}
