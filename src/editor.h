#ifndef KED_EDITOR_H
#define KED_EDITOR_H

#include "buffer.h"
#include "clip.h"
#include "render.h"
#include "syntax.h"
#include "theme.h"
#include "term.h"
#include "util.h"

#include <stddef.h>

#define MSG_TTL_MS 5000

typedef struct Editor {
    Buffer buf;
    size_t cy, cx;          /* cursor: line, byte offset in the line */
    size_t want_col;        /* display column kept across vertical moves */
    size_t rowoff, coloff;  /* first visible line / display column */
    int rows, cols;         /* terminal size */
    int tabw;

    int sel_on;             /* a selection runs from the anchor to the cursor */
    size_t ay, ax;          /* selection anchor: line, byte offset */

    Clip clip;
    int clipboard_osc52;    /* also send copies to the system clipboard */
    int paste_valid;        /* the last command was a paste that Alt+V can replace */
    size_t paste_y0, paste_x0, paste_y1, paste_x1;
    size_t paste_idx;       /* kill ring entry that was pasted */

    Syntax *syn;            /* NULL: plain text */
    size_t theme_idx;
    const Theme *theme;
    int color_depth;
    int match_on;           /* the bracket at/before the cursor has a partner */
    size_t by, bx, my, mx;  /* that bracket and its partner */

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

/* Replaces the language (takes ownership of syn, which may be NULL). */
void editor_set_syntax(Editor *e, Syntax *syn);

/* Shows a message on the last row; ttl_ms == 0 keeps it until replaced. */
void editor_set_msg(Editor *e, long ttl_ms, const char *fmt, ...) KED_PRINTF(3, 4);
/* Display column of the cursor within its line. */
size_t editor_cursor_col(const Editor *e);

#endif
