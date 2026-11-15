#ifndef XTED_CFGSAVE_H
#define XTED_CFGSAVE_H

#include <stddef.h>

/* Writing settings into the files the editor reads. A file is changed line by line: the line of a
 * key that is already there is replaced, a new key goes at the end of its section, and comments,
 * unknown lines and order are kept. The file is written under a temporary name and renamed, so it
 * is never left half written. Errors come back as a message in err (Italian, for the user). */

typedef struct {
    const char *key, *value;
} CfgPair;

/* Sets the pairs in a file of the editor's own format ("key = value", "[section]"); section NULL
 * is the top level, before the first header. A missing file, folder or section is created.
 * Returns 0, or -1 with err. */
int cfgsave_write(const char *file, const char *section, const CfgPair *p, size_t n, char *err,
                  size_t errn);

/* Sets those of the pairs that .editorconfig can say (expand_tab, indent_width, tab_width) for the
 * file docpath: in the section of the nearest .editorconfig that matches it (the last one, if
 * several do), else in a new [*.ext] section; with no .editorconfig at all one is created in the
 * project's folder, with "root = true". The file written is copied to file. Returns how many
 * pairs were written, or -1 with err. */
int cfgsave_editorconfig(const char *docpath, const CfgPair *p, size_t n, char *file, size_t filen,
                         char *err, size_t errn);

/* The .editorconfig that cfgsave_editorconfig would write for path (malloc'ed): the nearest one,
 * else a new one in the project's folder. */
char *cfgsave_editorconfig_path(const char *docpath);
/* The nearest ".xted" going up from the folder of path (malloc'ed), NULL if there is none. */
char *cfgsave_find_project(const char *path);
/* Where a ".xted" for path goes if there is none yet: the nearest folder going up that has a
 * .xted, .git, composer.json or package.json, else the folder of the file (malloc'ed). */
char *cfgsave_project_root(const char *path);

#endif
