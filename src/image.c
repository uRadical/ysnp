#include "image.h"
#include "log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

/* Subdirectory under $HOME holding user-supplied overlay images. */
#define YSNP_IMAGE_SUBDIR "/.config/ysnp/images"

static const char *const EXTENSIONS[] = {".png", ".jpg", ".jpeg", ".gif"};
static const size_t EXTENSION_COUNT = sizeof(EXTENSIONS) / sizeof(EXTENSIONS[0]);

/* Case-insensitive check that name ends with one of the accepted extensions. */
static int has_image_extension(const char *name) {
    size_t name_len = strlen(name);
    for (size_t i = 0; i < EXTENSION_COUNT; i++) {
        size_t ext_len = strlen(EXTENSIONS[i]);
        if (name_len >= ext_len &&
            strcasecmp(name + name_len - ext_len, EXTENSIONS[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Seed rand() from CLOCK_MONOTONIC nanoseconds if available, else time(). */
static void seed_random(void) {
    unsigned int seed;
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        seed = (unsigned int)(ts.tv_nsec ^ ts.tv_sec);
    } else {
        seed = (unsigned int)time(NULL);
    }
#else
    seed = (unsigned int)time(NULL);
#endif
    srand(seed);
}

const char *image_pick(void) {
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        return NULL;
    }

    size_t dir_len = strlen(home) + strlen(YSNP_IMAGE_SUBDIR) + 1;
    char *dir_path = malloc(dir_len);
    if (!dir_path) {
        ysnp_die("out of memory");
    }
    snprintf(dir_path, dir_len, "%s%s", home, YSNP_IMAGE_SUBDIR);

    DIR *dir = opendir(dir_path);
    if (!dir) {
        free(dir_path);
        return NULL;
    }

    char **paths = NULL;
    size_t count = 0;
    size_t capacity = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!has_image_extension(entry->d_name)) {
            continue;
        }

        if (count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 8;
            char **grown = realloc(paths, new_capacity * sizeof(*paths));
            if (!grown) {
                closedir(dir);
                ysnp_die("out of memory");
            }
            paths = grown;
            capacity = new_capacity;
        }

        size_t full_len = strlen(dir_path) + 1 + strlen(entry->d_name) + 1;
        char *full = malloc(full_len);
        if (!full) {
            closedir(dir);
            ysnp_die("out of memory");
        }
        snprintf(full, full_len, "%s/%s", dir_path, entry->d_name);

        /* Skip anything that isn't a regular file (e.g. a directory or a
         * dangling symlink named "art.gif"). stat() follows symlinks, so a
         * symlink to a real image is still accepted. */
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(full);
            continue;
        }

        paths[count++] = full;
    }

    closedir(dir);
    free(dir_path);

    if (count == 0) {
        free(paths);
        return NULL;
    }

    seed_random();
    size_t chosen = (size_t)rand() % count;
    char *result = paths[chosen];

    /* Free every path except the one we return. */
    for (size_t i = 0; i < count; i++) {
        if (i != chosen) {
            free(paths[i]);
        }
    }
    free(paths);

    return result;
}
