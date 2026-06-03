#ifndef YSNP_LOG_H
#define YSNP_LOG_H

/* Append a printf-style, timestamped line to the log file and to stderr. */
void ysnp_logf(const char *fmt, ...);

/* Log the message, fire a best-effort desktop notification (so the failure is
 * visible even when ysnp was launched from a git hook with no terminal in
 * view), then exit(1). */
void ysnp_die(const char *fmt, ...);

#endif /* YSNP_LOG_H */
