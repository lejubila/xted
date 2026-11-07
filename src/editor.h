#ifndef KED_EDITOR_H
#define KED_EDITOR_H

#include "buffer.h"
#include "render.h"
#include "term.h"

#include <stddef.h>

typedef struct Editor {
    Buffer buf;
    size_t cy, cx;          /* cursor: line, byte offset in the line */
    size_t want_col;        /* display column kept across vertical moves */
    size_t rowoff, coloff;  /* first visible line / display column */
    int rows, cols;         /* terminal size */
    int tabw;

    char msg[256];
    long msg_until;         /* monotonic ms when msg expires; 0 = sticky */

    int prompt_on;
    char prompt_label[64];
    char prompt_text[512];
    size_t prompt_len;

    int quit;
    Render *render;
} Editor;

void editor_init(Editor *e);
void editor_free(Editor *e);
/* Loads path into the buffer (before the terminal is initialised). */
int editor_open(Editor *e, const char *path, char *err, size_t errlen);
void editor_run(Editor *e);

/* Display column of the cursor within its line. */
size_t editor_cursor_col(const Editor *e);

#endif
