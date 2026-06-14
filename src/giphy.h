#ifndef YSNP_GIPHY_H
#define YSNP_GIPHY_H

/* Pick a random "you shall not pass"-flavoured search term, ask Giphy for a
 * random matching GIF, and download it to a temp file.
 *
 * Returns a malloc'd path to the downloaded GIF (caller frees and should
 * remove() it when done), or NULL on any failure — the caller can then fall
 * back to the embedded default so a flaky network never breaks a git hook. */
char *giphy_fetch_random(void);

#endif /* YSNP_GIPHY_H */
