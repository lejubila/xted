#include "editor.h"
#include "term.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(FILE *f)
{
    fputs("uso: xted [file[:riga[:colonna]] ...]\n"
          "     xted -       legge il testo da stdin\n"
          "configurazione: $XDG_CONFIG_HOME/xted (o ~/.config/xted): config, syntax/, themes/\n", f);
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

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "xted: opzione sconosciuta: %s\n", argv[i]);
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
                fputs("xted: '-' richiede il testo su stdin (una volta sola)\n", stderr);
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
    editor_load_project(&e);

    if (term_init() < 0) {
        fputs("xted: serve un terminale interattivo\n", stderr);
        return 1;
    }
    editor_run(&e);
    term_restore();
    editor_save_session(&e);
    editor_free(&e);
    return 0;
}
