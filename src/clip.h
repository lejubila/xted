#ifndef KED_CLIP_H
#define KED_CLIP_H

#include "util.h"

#include <stddef.h>

#define CLIP_MAX 32
/* Largest text sent to the system clipboard; many terminals drop bigger OSC 52 requests. */
#define OSC52_MAX 100000

/* A whole_line entry holds complete lines (ending in '\n') copied without a
 * selection; pasting it inserts above the cursor line. */
typedef struct {
    char *text;
    size_t n;
    int whole_line;
} ClipEntry;

/* Kill ring: e[0] is the most recent. */
typedef struct {
    ClipEntry e[CLIP_MAX];
    size_t count;
} Clip;

void clip_free(Clip *c);
/* Takes ownership of text (malloc'ed); drops the oldest entry when full. */
void clip_push(Clip *c, char *text, size_t n, int whole_line);
const ClipEntry *clip_get(const Clip *c, size_t i);

/* Writes 4 * ceil(n / 3) bytes to out (no NUL); returns the count. */
size_t base64_encode(const char *in, size_t n, char *out);
/* Appends an OSC 52 "set clipboard" request; -1 if text is too big. */
int clip_osc52(SBuf *out, const char *text, size_t n);

#endif
