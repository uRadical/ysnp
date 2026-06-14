/* Fetch a random GIF from Giphy and stage it as a temp file for the overlay.
 *
 * The flow: pick a random rejection-themed tag, GET the Giphy "random GIF"
 * endpoint into memory, scrape the original GIF URL out of the JSON (a tiny
 * hand-rolled extractor — not worth a JSON dependency for one field), then
 * stream that URL down to a temp file whose path we hand back to the overlay.
 *
 * Any failure (no key, no network, no matching GIF, parse error) is silent —
 * we just return NULL so the caller falls back to the embedded default. A
 * missing GIF is an expected, unremarkable outcome that must never block a
 * push or clutter the log. */

#include "giphy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>

/* The default API key is baked in at compile time from the GIPHY_API_KEY make
 * variable (the release workflow feeds it from a GitHub secret), so the key
 * never lives in source or git history. It defaults to empty: a build with no
 * key, and no GIPHY_API_KEY in the environment at runtime, simply skips the
 * fetch and falls back to the embedded default image. The runtime env var
 * always wins, so any build's key can be overridden or rotated. */
#ifndef GIPHY_DEFAULT_API_KEY
#define GIPHY_DEFAULT_API_KEY ""
#endif

/* Rejection-flavoured search tags; one is picked at random per invocation.
 * These are deliberately iconic reaction-GIF phrases (a hard "no", being
 * blocked, "you shall not pass") rather than generic words like "failed",
 * which Giphy matches to loosely-related clips. Specific phrases land on the
 * well-known reaction GIFs and stay on-theme for a blocked push. */
static const char *const TERMS[] = {
    "you shall not pass",
    "nope",
    "no",
    "no no no",
    "no way",
    "denied",
    "access denied",
    "permission denied",
    "rejected",
    "not today",
    "absolutely not",
    "computer says no",
    "talk to the hand",
    "blocked",
    "stop right there",
};
static const size_t TERM_COUNT = sizeof(TERMS) / sizeof(TERMS[0]);

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

/* ---- in-memory download buffer ----------------------------------------- */

struct membuf {
    char *data;
    size_t len;
};

static size_t write_mem(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    struct membuf *m = userdata;
    char *grown = realloc(m->data, m->len + total + 1);
    if (!grown) {
        return 0; /* signals an error to libcurl, aborting the transfer */
    }
    m->data = grown;
    memcpy(m->data + m->len, ptr, total);
    m->len += total;
    m->data[m->len] = '\0';
    return total;
}

/* Extract data.images.original.url from the Giphy JSON response. Giphy escapes
 * forward slashes as "\/" in its JSON, so unescape \/ and \\ as we copy.
 * Returns a malloc'd URL string, or NULL if the field could not be found. */
static char *extract_original_url(const char *json) {
    const char *orig = strstr(json, "\"original\":");
    if (!orig) {
        return NULL;
    }
    const char *url = strstr(orig, "\"url\":\"");
    if (!url) {
        return NULL;
    }
    url += strlen("\"url\":\"");

    /* Worst case the unescaped URL is as long as the escaped source. */
    char *out = malloc(strlen(url) + 1);
    if (!out) {
        return NULL;
    }

    size_t o = 0;
    for (const char *p = url; *p && *p != '"'; p++) {
        if (*p == '\\' && (p[1] == '/' || p[1] == '\\')) {
            p++; /* drop the backslash, keep the next char */
        }
        out[o++] = *p;
    }
    out[o] = '\0';

    if (o == 0) {
        free(out);
        return NULL;
    }
    return out;
}

/* GET url into a freshly-allocated, NUL-terminated buffer. Caller frees. */
static char *http_get(CURL *curl, const char *url) {
    struct membuf buf = {0};
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ysnp");

    if (curl_easy_perform(curl) != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    return buf.data;
}

/* Download url into a temp file. Returns the malloc'd path, or NULL. */
static char *download_to_temp(CURL *curl, const char *url) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) {
        tmpdir = "/tmp";
    }

    size_t tlen = strlen(tmpdir) + strlen("/ysnp-giphyXXXXXX") + 1;
    char *path = malloc(tlen);
    if (!path) {
        return NULL;
    }
    snprintf(path, tlen, "%s/ysnp-giphyXXXXXX", tmpdir);

    int fd = mkstemp(path);
    if (fd < 0) {
        free(path);
        return NULL;
    }

    FILE *fp = fdopen(fd, "wb");
    if (!fp) {
        close(fd);
        remove(path);
        free(path);
        return NULL;
    }

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp); /* default writer: fwrite */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ysnp");

    CURLcode rc = curl_easy_perform(curl);
    fclose(fp);

    if (rc != CURLE_OK) {
        remove(path);
        free(path);
        return NULL;
    }
    return path;
}

char *giphy_fetch_random(void) {
    const char *key = getenv("GIPHY_API_KEY");
    if (!key || !*key) {
        key = GIPHY_DEFAULT_API_KEY;
    }
    if (!*key) {
        /* No key configured: silently fall back to the embedded default. */
        return NULL;
    }

    seed_random();
    const char *tag = TERMS[(size_t)rand() % TERM_COUNT];

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return NULL;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        curl_global_cleanup();
        return NULL;
    }

    char *result = NULL;
    char *json = NULL;
    char *gif_url = NULL;

    char *tag_esc = curl_easy_escape(curl, tag, 0);
    if (!tag_esc) {
        goto done;
    }

    char api_url[512];
    snprintf(api_url, sizeof(api_url),
             "https://api.giphy.com/v1/gifs/random"
             "?api_key=%s&tag=%s&rating=pg-13",
             key, tag_esc);
    curl_free(tag_esc);

    json = http_get(curl, api_url);
    if (!json) {
        goto done;
    }

    gif_url = extract_original_url(json);
    if (!gif_url) {
        goto done;
    }

    result = download_to_temp(curl, gif_url);

done:
    free(gif_url);
    free(json);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return result;
}
