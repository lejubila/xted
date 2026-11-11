#ifndef KED_TAGS_H
#define KED_TAGS_H

#include "buffer.h"

#include <stddef.h>

/* Reading a ctags "tags" file: name <TAB> file <TAB> address[;"<TAB>extended fields]. */

typedef struct {
    char *file;             /* as written in the tags file (relative to its folder) */
    char *addr;             /* a line number or a /^pattern$/ search */
    char kind[24];          /* "f", "function"... from the extended fields, "" if none */
    size_t line;            /* from a "line:N" field, else 0 */
} TagHit;

typedef struct {
    TagHit *v;
    size_t n, cap;
} TagHits;

/* Looks for a file named "tags" in dir and its parents. Returns its path (malloc'ed) or NULL. */
char *tags_find_file(const char *dir);
/* The entries called name. Returns their number, or -1 if the file cannot be read. A file
 * marked as sorted (!_TAG_FILE_SORTED 1) is searched by bisection, others line by line. */
long tags_lookup(const char *tagsfile, const char *name, TagHits *out);
void taghits_free(TagHits *h);

/* The 1-based line a hit points at in the text of its file; 0 if the search finds nothing. */
size_t tag_resolve(const TagHit *h, const char *name, const Buffer *b);

#endif
