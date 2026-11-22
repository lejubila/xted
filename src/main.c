#include "config.h"
#include "editor.h"
#include "version.h"
#include "i18n.h"
#include "files.h"
#include "ipc.h"
#include "launch.h"
#include "sys.h"
#include "term.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(FILE *f)
{
#ifdef _WIN32
    fputs(tr("usage: xted [file[:line[:column]] ...]\n"
             "     xted -       reads the text from stdin\n"
             "     xted -V      shows the version (also --version)\n"
             "     xted --remote file...  opens the files in the xted already open, if there is one\n"
             "configuration: %APPDATA%\\xted: config, syntax\\, themes\\, lang\\\n"), f);
#else
    fputs(tr("usage: xted [file[:line[:column]] ...]\n"
             "     xted -       reads the text from stdin\n"
             "     xted -V      shows the version (also --version)\n"
             "     xted --remote file...  opens the files in the xted already open, if there is one\n"
             "configuration: $XDG_CONFIG_HOME/xted (or ~/.config/xted): config, syntax/, themes/, lang/\n"), f);
#endif
}

/* Splits "name:12:3" into the name and a line/column, but only when the
 * whole string is not an existing file. Returns the name (malloc'ed). */
static char *split_spec(const char *arg, size_t *line, size_t *col)
{
    size_t n = strlen(arg), end = n, nums[2] = { 0, 0 }, k = 0;
    struct stat st;

    *line = *col = 0;
    if (stat(arg, &st) == 0)
        return xstrdup(arg);
    while (k < 2) {
        size_t i = end;

        while (i > 0 && isdigit((unsigned char)arg[i - 1]))
            i--;
        if (i == end || i == 0 || arg[i - 1] != ':')
            break;
        nums[k++] = (size_t)strtoul(arg + i, NULL, 10);
        end = i - 1;
    }
    if (k == 0 || end == 0)
        return xstrdup(arg);
    if (k == 2) {
        *line = nums[1];
        *col = nums[0];
    } else {
        *line = nums[0];
    }
    {
        char *name = xmalloc(end + 1);

        memcpy(name, arg, end);
        name[end] = '\0';
        return name;
    }
}

/* The files of the command line for another xted: absolute, with their line and column (the
 * options and "-" left out). */
static IpcFile *arg_files(int argc, char **argv, size_t *n)
{
    IpcFile *f = xmalloc((size_t)argc * sizeof *f);
    char cwd[PATH_MAX];
    int i;

    *n = 0;
    if (!sys_getcwd(cwd, sizeof cwd))
        cwd[0] = '\0';
    for (i = 1; i < argc; i++) {
        char *name;

        if (argv[i][0] == '-')
            continue;
        sys_path_fix(argv[i]);
        name = split_spec(argv[i], &f[*n].line, &f[*n].col);
        if (sys_is_abs(name) || !cwd[0]) {
            f[*n].path = name;
        } else {
            f[*n].path = files_join(cwd, name);
            free(name);
        }
        (*n)++;
    }
    return f;
}

static void free_files(IpcFile *f, size_t n)
{
    size_t k;

    for (k = 0; k < n; k++)
        free(f[k].path);
    free(f);
}

/* xted --remote: the files to the xted that listens. 1 when it took them; 0 when there is none,
 * or no file is given. */
static int send_remote(int argc, char **argv)
{
    size_t n;
    IpcFile *f = arg_files(argc, argv, &n);
    int r = n > 0 && ipc_send(f, n);

    free_files(f, n);
    return r;
}

#ifndef _WIN32
/* xted --desktop (the icon, "Open with"): with no terminal of its own, the files go to the xted
 * that listens, else a terminal emulator is started with xted in it. From a terminal it is as
 * --remote. Returns the exit status, or -1 to start here as usual. */
static int desktop(int argc, char **argv)
{
    size_t n;
    IpcFile *f;
    char *cd, file[PATH_MAX];
    Config c;
    int r;

    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))
        return send_remote(argc, argv) ? 0 : -1;
    f = arg_files(argc, argv, &n);
    if (n > 0 && ipc_send(f, n)) {
        free_files(f, n);
        return 0;
    }
    config_init(&c);
    cd = config_dir();
    if (cd) {
        snprintf(file, sizeof file, "%s/config", cd);
        config_load_file(&c, file);
        free(cd);
    }
    r = launch_in_terminal(f, n, c.terminal);
    fputs(tr("xted: no terminal emulator found: set one with \"terminal =\" in the configuration\n"),
          stderr);
    config_free(&c);
    free_files(f, n);
    return r < 0 ? 1 : 0;
}
#endif

static int is_dir(const char *p)
{
    struct stat st;

    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(int argc, char **argv)
{
    char err[256];
    Editor e;
    Doc *first = NULL;
    int i, stdin_used = 0, remote = 0;
    char *cd;

    sys_init();
    cd = config_dir();
    /* the language of the command line messages comes from the environment (the configuration
     * is read later and may choose another one) */
    i18n_init(cd);
    i18n_set("auto");
    free(cd);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("xted %s\n", XTED_VERSION);
            printf(tr("Copyright (C) %s %s\n"), XTED_YEAR, XTED_AUTHOR);
            printf(tr("%s license: free to use, copy, change and distribute, keeping this notice.\n"),
                   XTED_LICENSE);
            return 0;
        }
        if (strcmp(argv[i], "--remote") == 0) {
            remote = 1;
            continue;
        }
        if (strcmp(argv[i], "--desktop") == 0) {
            remote = 2;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, tr("xted: unknown option: %s\n"), argv[i]);
            usage(stderr);
            return 2;
        }
    }
    /* --remote: the files to the xted that listens, if there is one; else as without it */
#ifndef _WIN32
    if (remote == 2) {
        int r = desktop(argc, argv);

        if (r >= 0)
            return r;
    } else
#endif
    if (remote && send_remote(argc, argv))
        return 0;

    editor_init(&e);
    editor_load_config(&e);
    for (i = 1; i < argc; i++) {
        Doc *d;
        size_t line, col;

        sys_path_fix(argv[i]);              /* C:\dir\a.c as C:/dir/a.c on Windows */
        if (strcmp(argv[i], "--remote") == 0 || strcmp(argv[i], "--desktop") == 0)
            continue;
        if (strcmp(argv[i], "-") == 0) {
            if (stdin_used || isatty(STDIN_FILENO)) {
                fputs(tr("xted: '-' needs the text on stdin (only once)\n"), stderr);
                return 2;
            }
            stdin_used = 1;
            d = editor_open_fd(&e, STDIN_FILENO, err, sizeof err);
            if (!d) {
                fprintf(stderr, "xted: stdin: %s\n", err);
                return 1;
            }
        } else if (is_dir(argv[i])) {
            editor_show_tree(&e, argv[i]);
            continue;
        } else {
            char *name = split_spec(argv[i], &line, &col);

            d = editor_open_doc(&e, name, err, sizeof err);
            if (!d) {
                fprintf(stderr, "xted: %s: %s\n", name, err);
                free(name);
                return 1;
            }
            free(name);
            if (line > 0) {
                editor_goto(&e, line, col);
                editor_store_view(&e);
            }
        }
        if (!first)
            first = d;
    }
    if (first)
        editor_activate(&e, first);
    else if (argc == 1)
        editor_restore_session(&e);
    /* nothing to show: the start screen, until the first key */
    e.intro = !e.doc->buf.path && !e.doc->scratch && e.doc->buf.nlines == 1 && e.doc->buf.lines[0].len == 0;
    editor_load_project(&e);

    if (term_init() < 0) {
        fputs(tr("xted: an interactive terminal is needed\n"), stderr);
        return 1;
    }
    /* the first xted gets the files opened from the desktop (xted --remote) */
    if (e.cfg.single_instance != 0) {
        intptr_t h = ipc_listen();

        if (h != -1) {
            term_watch_server(h);
            atexit(ipc_close);
        }
    }
    editor_run(&e);
    term_restore();
    editor_save_session(&e);
    editor_free(&e);
    return 0;
}
