/* libFuzzer harness for the image decoders (decode.c).
 *
 * Build & run (needs clang; Linux):
 *   make fuzz
 *   ASAN_OPTIONS=allocator_may_return_null=1 \
 *       build/fuzz_decode -detect_leaks=0 tests/corpus
 *
 * decode.c's contract is "die on failure" — ysnp_die logs, notifies and
 * exits, which a fuzzer would misread as a crash. The harness overrides
 * ysnp_die/ysnp_logf (instead of linking log.c) to longjmp back here, so
 * designed failures end the iteration and only real memory errors —
 * caught by ASan/UBSan — count as findings. The longjmp path can leak
 * allocations that production would have freed by exiting, hence
 * -detect_leaks=0.
 *
 * allocator_may_return_null=1 is required: giflib allocates RasterBits
 * from header-declared frame sizes before any of our validation runs, so
 * a hostile header can request gigabytes. A failed allocation is handled
 * (DGifSlurp errors out, load_gif returns 0); without the flag ASan
 * aborts on the attempt and reports a non-bug.
 *
 * GIF fuzzes the in-memory path directly; JPEG/PNG loaders take file
 * paths, so those inputs go through one reused temp file. */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static jmp_buf die_escape;
static int in_target;

void ysnp_logf(const char *fmt, ...) {
    (void)fmt;
}

void ysnp_die(const char *fmt, ...) {
    (void)fmt;
    if (in_target) {
        longjmp(die_escape, 1);
    }
    abort(); /* die outside an iteration is a harness bug */
}

/* Include the unit under test directly (same pattern as test_image.c) so
 * the overridden ysnp_die above is the one it calls. */
#include "../src/decode.c"

/* Free whatever decode state an iteration left behind. After a successful
 * decode ysnp_free_frames handles everything; after a longjmp the frame
 * arrays may exist with ysnp_frame_count still 0. */
static void reset_state(void) {
    if (ysnp_frame_count > 0) {
        ysnp_free_frames();
    } else {
        free(ysnp_frames);
        free(ysnp_frame_delays);
        ysnp_frames = NULL;
        ysnp_frame_delays = NULL;
        ysnp_is_animated = 0;
    }
}

static const char *scratch_path(void) {
    static char path[64];
    if (!path[0]) {
        snprintf(path, sizeof(path), "/tmp/ysnp-fuzz-%d", (int)getpid());
    }
    return path;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }

    in_target = 1;
    if (setjmp(die_escape) == 0) {
        if (size >= 4 && memcmp(data, "GIF8", 4) == 0) {
            /* In-memory GIF path: no file round-trip needed. */
            load_gif(NULL, data, size);
        } else {
            /* JPEG/PNG (and GIF-by-extension fallbacks) decode from a
             * path; ysnp_load_image also exercises sniff_format. */
            const char *path = scratch_path();
            FILE *fp = fopen(path, "wb");
            if (fp) {
                fwrite(data, 1, size, fp);
                fclose(fp);
                ysnp_load_image(path);
            }
        }
    }
    in_target = 0;

    reset_state();
    return 0;
}
