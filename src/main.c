#include "config.h"
#include "editor.h"
#include "version.h"
#include "i18n.h"
#include "term.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(FILE *f)
{
    fputs(tr("usage: xted [file[:line[:column]] ...]\n"
             "     xted -       reads the text from stdin\n"
             "     xted -V      shows the version (also --version)\n"
             "configuration: $XDG_CONFIG_HOME/xted (or ~/.config/xted): config, syntax/, themes/, lang/\n"), f);
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
    int i, stdin_used = 0;
    char *cd = config_dir();

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
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, tr("xted: unknown option: %s\n"), argv[i]);
            usage(stderr);
            return 2;
        }
    }

    editor_init(&e);
    editor_load_config(&e);
    for (i = 1; i < argc; i++) {
        Doc *d;
        size_t line, col;

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
    editor_run(&e);
    term_restore();
    editor_save_session(&e);
    editor_free(&e);
    return 0;
}
