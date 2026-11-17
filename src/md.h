#ifndef XTED_MD_H
#define XTED_MD_H

#include "buffer.h"

#include <stddef.h>
#include <stdint.h>

/* A style byte of the preview: the syntax class (SC_*) in the low bits, attributes above. */
enum { MD_CLS = 0x0F, MD_BOLD = 0x10, MD_ITALIC = 0x20, MD_UNDER = 0x40, MD_CODEBG = 0x80 };

/* The rendered preview of a Markdown document: kept in the preview's Doc, made again when the
 * source changes or the pane changes width. */
typedef struct MdPreview {
    struct Doc *src;        /* the document it shows */
    unsigned long gen;      /* src's Buffer.gen it was built from */
    int width, glyphs;      /* the width and the glyph set it was built for */
    uint8_t **st;           /* the style of each byte of each preview line */
    size_t *srcline;        /* the source line each preview line comes from */
    size_t n;
    size_t synced_y;        /* the source line the pane was last scrolled to (-1: none) */
} MdPreview;

/* Writes the preview of src, width columns wide, into out (whose text is replaced; no undo) and
 * its styles into m. ascii: + - | instead of the box and bullet characters. */
void md_build(MdPreview *m, const Buffer *src, Buffer *out, int width, int ascii);
/* The styles of preview line y, NULL if none. */
const uint8_t *md_styles(const MdPreview *m, size_t y);
/* The first preview line that comes from source line y or after it. */
size_t md_line_for(const MdPreview *m, size_t y);
void md_free(MdPreview *m);

#endif
