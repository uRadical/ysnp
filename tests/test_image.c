/* Unit tests for the pure, portable parts of image.c.
 *
 * We #include the .c directly so we can exercise its static helpers
 * (has_image_extension) as well as the public image_pick(). Built and run via
 * `make test` under AddressSanitizer + UBSan. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/log.c"
#include "../src/image.c"

static void test_extension_matching(void) {
    assert(has_image_extension("foo.png"));
    assert(has_image_extension("foo.PNG"));
    assert(has_image_extension("foo.JpG"));
    assert(has_image_extension("a.jpeg"));
    assert(has_image_extension("a.JPEG"));
    assert(has_image_extension("clip.gif"));

    assert(!has_image_extension("noextension"));
    assert(!has_image_extension("notes.txt"));
    assert(!has_image_extension("archive.tar.gz"));
    assert(!has_image_extension("png"));   /* no dot */
    assert(!has_image_extension(""));
    printf("  ok: extension matching\n");
}

/* Build a private HOME with a config images dir; return its path (static).
 * The template is rewritten each call so repeated invocations get fresh dirs. */
static const char *make_temp_home(void) {
    static char home[32];
    strcpy(home, "/tmp/ysnp-test-XXXXXX");
    char *p = mkdtemp(home);
    assert(p != NULL);
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/.config/ysnp/images", home);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    assert(system(cmd) == 0);
    setenv("HOME", home, 1);
    return home;
}

static void touch(const char *home, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/.config/ysnp/images/%s", home, name);
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    fputc('x', f);
    fclose(f);
}

static void test_pick_empty_dir_returns_null(void) {
    const char *home = make_temp_home();
    const char *pick = image_pick();
    assert(pick == NULL); /* no images yet */
    printf("  ok: empty dir -> NULL\n");
    (void)home;
}

static void test_pick_finds_image(void) {
    const char *home = make_temp_home();
    touch(home, "only.png");
    const char *pick = image_pick();
    assert(pick != NULL);
    assert(strstr(pick, "only.png") != NULL);
    free((void *)pick);
    printf("  ok: single image picked\n");
}

static void test_pick_skips_non_image_and_dirs(void) {
    const char *home = make_temp_home();
    touch(home, "notes.txt");           /* wrong extension */
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/.config/ysnp/images/album.gif", home);
    assert(mkdir(subdir, 0755) == 0);   /* a directory named like an image */

    const char *pick = image_pick();
    assert(pick == NULL); /* nothing selectable: txt ignored, dir skipped */
    printf("  ok: non-images and directories skipped\n");
}

static void test_pick_only_real_image_among_decoys(void) {
    const char *home = make_temp_home();
    touch(home, "notes.txt");
    touch(home, "real.jpeg");
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/.config/ysnp/images/fake.png", home);
    assert(mkdir(subdir, 0755) == 0);

    const char *pick = image_pick();
    assert(pick != NULL);
    assert(strstr(pick, "real.jpeg") != NULL);
    free((void *)pick);
    printf("  ok: real image chosen over decoys\n");
}

int main(void) {
    printf("test_image:\n");
    test_extension_matching();
    test_pick_empty_dir_returns_null();
    test_pick_finds_image();
    test_pick_skips_non_image_and_dirs();
    test_pick_only_real_image_among_decoys();
    printf("ALL PASS\n");
    return 0;
}
