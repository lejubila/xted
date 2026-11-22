/* The system clipboard on POSIX (sys_clip_set, sys_clip_get of sys.h): a graphical session's,
 * through its programs, as a program in a terminal cannot speak to Wayland (that needs a
 * window) and X11 would need a client of its own. Wayland: wl-copy and wl-paste (wl-clipboard);
 * X11, and Wayland through XWayland: xclip, xsel. Not over SSH (the clipboard there is the
 * remote machine's): OSC 52 is used, as when none of the programs is installed. */

#include "shell.h"
#include "sys.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GET_TIMEOUT_MS 1000
#define GET_MAX (16u << 20)

/* A program of PATH. */
static int have(const char *prog)
{
    const char *path = getenv("PATH"), *p, *colon;
    char full[1024];

    for (p = path ? path : "/usr/bin:/bin"; *p; p = colon + 1) {
        size_t n;

        colon = strchr(p, ':');
        n = colon ? (size_t)(colon - p) : strlen(p);
        if (n > 0 && n + strlen(prog) + 2 < sizeof full) {
            memcpy(full, p, n);
            full[n] = '/';
            strcpy(full + n + 1, prog);
            if (access(full, X_OK) == 0)
                return 1;
        }
        if (!colon)
            break;
    }
    return 0;
}

static int set_env(const char *v)
{
    return v && v[0];
}

/* The commands that copy and paste in this session: 1, 0 when there is no graphical session
 * of ours (SSH, a console), -1 when there is one but none of its programs. */
static int tools(const char **copy, const char *const **paste)
{
    static const char *const wl_get[] = { "wl-paste", "--no-newline", "--type", "text", NULL };
    static const char *const xclip_get[] = { "xclip", "-selection", "clipboard", "-o", NULL };
    static const char *const xsel_get[] = { "xsel", "--clipboard", "--output", NULL };
    int wl = set_env(getenv("WAYLAND_DISPLAY")), x = set_env(getenv("DISPLAY"));

    if (set_env(getenv("SSH_CONNECTION")) || set_env(getenv("SSH_TTY")) || (!wl && !x))
        return 0;
    if (wl && have("wl-copy") && have("wl-paste")) {
        *copy = "exec wl-copy --type text/plain";
        *paste = wl_get;
    } else if (x && have("xclip")) {
        *copy = "exec xclip -selection clipboard -in";
        *paste = xclip_get;
    } else if (x && have("xsel")) {
        *copy = "exec xsel --clipboard --input";
        *paste = xsel_get;
    } else {
        return -1;
    }
    return 1;
}

int sys_clip_set(const char *s, size_t n)
{
    const char *copy;
    const char *const *paste;
    char line[128];
    const char *argv[4];
    int r = tools(&copy, &paste), st = 0;

    if (r <= 0)
        return r == 0 ? -1 : -2;
    /* the program stays to serve the text: its output away from the pipes we wait on */
    strcpy(line, copy);
    strcat(line, " >/dev/null 2>&1");
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = line;
    argv[3] = NULL;
    if (shell_capture(argv, NULL, s, n, NULL, NULL, GET_TIMEOUT_MS, 0, &st) != 0 || !WIFEXITED(st) ||
        WEXITSTATUS(st) != 0)
        return -2;
    return 0;
}

char *sys_clip_get(size_t *n)
{
    const char *copy;
    const char *const *paste;
    SBuf out = SBUF_INIT;
    int st = 0;
    size_t i, k;

    *n = 0;
    if (tools(&copy, &paste) <= 0)
        return NULL;
    if (shell_capture(paste, NULL, NULL, 0, &out, NULL, GET_TIMEOUT_MS, GET_MAX, &st) != 0 ||
        !WIFEXITED(st) || WEXITSTATUS(st) != 0 || out.len == 0) {
        sbuf_free(&out);
        return NULL;                        /* empty, or not text */
    }
    /* CR LF (and a CR alone) as LF; no NUL in a text */
    for (i = k = 0; i < out.len; i++) {
        if (out.p[i] == '\r') {
            out.p[k++] = '\n';
            if (i + 1 < out.len && out.p[i + 1] == '\n')
                i++;
        } else if (out.p[i]) {
            out.p[k++] = out.p[i];
        }
    }
    sbuf_add(&out, "", 1);
    *n = k;
    out.p[k] = '\0';
    return sbuf_steal(&out);
}
