#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROG "ysnp"

/* mkdir -p: create every component of path, ignoring "already exists". */
static void mkdir_p(const char *path) {
    char tmp[512];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) {
        return;
    }
    memcpy(tmp, path, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Resolve the log directory: $XDG_STATE_HOME/ysnp, else ~/.local/state/ysnp. */
static int log_dir(char *buf, size_t cap) {
    const char *state = getenv("XDG_STATE_HOME");
    if (state && state[0]) {
        snprintf(buf, cap, "%s/ysnp", state);
        return 1;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(buf, cap, "%s/.local/state/ysnp", home);
        return 1;
    }
    return 0;
}

static void write_log_line(const char *line) {
    char dir[512];
    if (!log_dir(dir, sizeof(dir))) {
        return;
    }
    mkdir_p(dir);
    char path[600];
    snprintf(path, sizeof(path), "%s/ysnp.log", dir);
    FILE *f = fopen(path, "a");
    if (!f) {
        return;
    }
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void timestamp(char *buf, size_t cap) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", &tmv);
}

void ysnp_logf(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s: %s\n", PROG, msg);

    char ts[32];
    timestamp(ts, sizeof(ts));
    char line[600];
    snprintf(line, sizeof(line), "[%s] %s: %s", ts, PROG, msg);
    write_log_line(line);
}

/* Run argv via fork/exec with stdout/stderr silenced; 0 if it exited 0. */
static int run_quietly(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
            if (dn > STDERR_FILENO) {
                close(dn);
            }
        }
        execvp(argv[0], argv);
        _exit(127); /* exec failed (e.g. notifier not installed) */
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* retry */
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Best-effort desktop notification. Tries the Linux notifier, then the macOS
 * one; whichever exists and succeeds wins. Both missing → silently give up
 * (the log file still has the message). */
static void notify(const char *msg) {
    /* Sanitize: neutralize characters that would break the AppleScript string
     * literal we build below. We exec directly (no shell), so this is about
     * well-formedness, not shell-injection. */
    char body[256];
    size_t j = 0;
    for (size_t i = 0; msg[i] && j + 1 < sizeof(body); i++) {
        char c = msg[i];
        body[j++] = (c == '"' || c == '\\' || c == '\n' || c == '\r') ? ' ' : c;
    }
    body[j] = '\0';

    char *linux_notify[] = {"notify-send", "-u", "critical", PROG, body, NULL};
    if (run_quietly(linux_notify) == 0) {
        return;
    }

    char script[400];
    snprintf(script, sizeof(script),
             "display notification \"%s\" with title \"%s\"", body, PROG);
    char *mac_notify[] = {"osascript", "-e", script, NULL};
    run_quietly(mac_notify);
}

void ysnp_die(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    ysnp_logf("%s", msg);
    notify(msg);
    exit(1);
}
