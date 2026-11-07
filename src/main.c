#include "editor.h"
#include "term.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *f)
{
    fputs("uso: ked [file]\n", f);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    char err[256];
    Editor e;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "ked: opzione sconosciuta: %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
        if (path) {
            fputs("ked: un solo file alla volta (buffer multipli: Fase 4)\n", stderr);
            return 2;
        }
        path = argv[i];
    }

    editor_init(&e);
    if (path && editor_open(&e, path, err, sizeof err) < 0) {
        fprintf(stderr, "ked: %s: %s\n", path, err);
        return 1;
    }
    if (term_init() < 0) {
        fputs("ked: serve un terminale interattivo\n", stderr);
        return 1;
    }
    editor_run(&e);
    term_restore();
    editor_free(&e);
    return 0;
}
