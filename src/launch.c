/* xted --desktop on POSIX: what the icon and "Open with" run, with no terminal (Terminal=false
 * in xted.desktop, so that no window flashes when the files go to an xted already open). When
 * none takes them, a terminal emulator is started with xted in it: the one of "terminal =" in
 * the configuration, $TERMINAL, xdg-terminal-exec, the one of the desktop, then the usual ones. */

#include "launch.h"
#include "sys.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A terminal and the words that make it run a command (put before it). */
typedef struct {
    const char *prog;
    const char *args[3];
} Term;

static const Term known[] = {
    { "xdg-terminal-exec", { NULL } },
    { "x-terminal-emulator", { "-e", NULL } },
    { "ptyxis", { "--", NULL } },
    { "kgx", { "--", NULL } },
    { "gnome-terminal", { "--", NULL } },
    { "konsole", { "-e", NULL } },
    { "xfce4-terminal", { "-x", NULL } },
    { "mate-terminal", { "-x", NULL } },
    { "tilix", { "-e", NULL } },
    { "kitty", { NULL } },
    { "alacritty", { "-e", NULL } },
    { "foot", { NULL } },
    { "wezterm", { "start", "--", NULL } },
    { "xterm", { "-e", NULL } },
};

/* prog is a program of PATH (or a path that can run). */
static int can_run(const char *prog)
{
    const char *path = getenv("PATH"), *p, *colon;
    char full[4096];

    if (strchr(prog, '/'))
        return access(prog, X_OK) == 0;
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

/* Runs the terminal: pre (the terminal and its words), then cmd. Returns only if it cannot. */
static void run(char **pre, size_t npre, char **cmd, size_t ncmd)
{
    char **argv = xmalloc((npre + ncmd + 1) * sizeof *argv);

    memcpy(argv, pre, npre * sizeof *argv);
    memcpy(argv + npre, cmd, ncmd * sizeof *argv);
    argv[npre + ncmd] = NULL;
    if (can_run(argv[0]))
        execvp(argv[0], argv);
    free(argv);
}

static void run_known(const Term *t, char **cmd, size_t ncmd)
{
    char *pre[4];
    size_t n = 0, k;

    pre[n++] = (char *)t->prog;
    for (k = 0; t->args[k]; k++)
        pre[n++] = (char *)t->args[k];
    run(pre, n, cmd, ncmd);
}

static void run_named(const char *prog, char **cmd, size_t ncmd)
{
    size_t i;

    for (i = 0; i < sizeof known / sizeof known[0]; i++)
        if (strcmp(known[i].prog, prog) == 0)
            run_known(&known[i], cmd, ncmd);
}

int launch_in_terminal(const IpcFile *f, size_t n, const char *term)
{
    char exe[4096], **cmd = xmalloc((n + 2) * sizeof *cmd), *home = getenv("HOME");
    const char *de = getenv("XDG_CURRENT_DESKTOP"), *env = getenv("TERMINAL");
    size_t ncmd = 0, i;
    long len = (long)sys_exe_path(exe, sizeof exe - 1);

    if (len <= 0)
        snprintf(exe, sizeof exe, "xted");
    else
        exe[len] = '\0';
    cmd[ncmd++] = exe;
    for (i = 0; i < n; i++) {
        char *a = xmalloc(strlen(f[i].path) + 48);

        if (f[i].line)
            sprintf(a, "%s:%zu:%zu", f[i].path, f[i].line, f[i].col ? f[i].col : 1);
        else
            strcpy(a, f[i].path);
        cmd[ncmd++] = a;
    }
    /* xted starts in the folder of the first file (its project), or in the home */
    if (n > 0) {
        char *d = xstrdup(f[0].path), *s = strrchr(d, '/');
        struct stat st;

        if (stat(f[0].path, &st) == 0 && S_ISDIR(st.st_mode))
            s = NULL;                       /* a folder: that one */
        else if (s)
            *(s == d ? s + 1 : s) = '\0';
        if (chdir(d) < 0 && home)
            (void)!chdir(home);
        free(d);
    } else if (home) {
        (void)!chdir(home);
    }

    /* the configured terminal: its words, then xted */
    if (term && term[0]) {
        char *copy = xstrdup(term), *pre[32], *w;
        size_t np = 0;

        for (w = strtok(copy, " \t"); w && np < 32; w = strtok(NULL, " \t"))
            pre[np++] = w;
        if (np > 0) {
            if (np == 1)
                run_named(pre[0], cmd, ncmd);   /* a known name alone: its own words */
            run(pre, np, cmd, ncmd);
        }
        free(copy);
    }
    if (env && env[0]) {
        char *pre[2];

        run_named(env, cmd, ncmd);
        pre[0] = (char *)env;
        pre[1] = "-e";
        run(pre, 2, cmd, ncmd);
    }
    run_named("xdg-terminal-exec", cmd, ncmd);
    /* the terminal of the desktop in use */
    if (de && strstr(de, "KDE"))
        run_named("konsole", cmd, ncmd);
    if (de && strstr(de, "GNOME")) {
        run_named("ptyxis", cmd, ncmd);
        run_named("kgx", cmd, ncmd);
        run_named("gnome-terminal", cmd, ncmd);
    }
    if (de && strstr(de, "XFCE"))
        run_named("xfce4-terminal", cmd, ncmd);
    if (de && strstr(de, "MATE"))
        run_named("mate-terminal", cmd, ncmd);
    for (i = 1; i < sizeof known / sizeof known[0]; i++)
        run_known(&known[i], cmd, ncmd);
    for (i = 1; i < ncmd; i++)
        free(cmd[i]);
    free(cmd);
    return -1;
}
