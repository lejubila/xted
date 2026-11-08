#ifndef KED_EDITOR_H
#define KED_EDITOR_H

#include "buffer.h"
#include "doc.h"
#include "files.h"
#include "layout.h"
#include "picker.h"
#include "clip.h"
#include "render.h"
#include "syntax.h"
#include "theme.h"
#include "term.h"
#include "util.h"

#include <stddef.h>

#define MSG_TTL_MS 5000
#define SIDEBAR_W 28

enum { PK_BUFFERS = 1, PK_FILES };

typedef struct Editor {
    Doc **docs;             /* open documents, in tab order */
    size_t ndocs, docs_cap;
    Layout layout;          /* panes; the active one is shown through the fields below */
    Doc *doc;               /* document of the active pane */
    Buffer *buf;            /* &doc->buf */

    char *root;             /* folder for Ctrl+P and the browser; NULL = current folder */
    int sidebar_on;         /* file browser shown */
    int sidebar_focus;      /* keys go to the browser */
    Tree tree;
    int tree_ready;
    Picker picker;
    int picker_kind;        /* PK_* */

    /* the active pane's view, live: it is copied into the pane when focus moves */
    size_t cy, cx;          /* cursor: line, byte offset in the line */
    size_t want_col;        /* display column kept across vertical moves */
    size_t rowoff, coloff;  /* first visible line / display column */
    int rows, cols;         /* terminal size */
    int text_x, text_y;     /* top-left of the pane area (right of the sidebar, below the tab bar) */
    int sidebar_w;
    int tabw;

    int sel_on;             /* a selection runs from the anchor to the cursor */
    size_t ay, ax;          /* selection anchor: line, byte offset */

    Clip clip;
    int clipboard_osc52;    /* also send copies to the system clipboard */
    int paste_valid;        /* the last command was a paste that Alt+V can replace */
    size_t paste_y0, paste_x0, paste_y1, paste_x1;
    size_t paste_idx;       /* kill ring entry that was pasted */

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

/* Opens a file in the active pane (or shows it if already open). Returns the
 * document, or NULL with err. */
Doc *editor_open_doc(Editor *e, const char *path, char *err, size_t errlen);
/* Same for the contents of fd, as an unnamed document. */
Doc *editor_open_fd(Editor *e, int fd, char *err, size_t errlen);
/* Shows the file browser rooted at dir, with the keyboard focus. */
void editor_show_tree(Editor *e, const char *dir);
/* Remembers / reopens the files of the current folder (see session.h). */
void editor_save_session(Editor *e);
int editor_restore_session(Editor *e);
/* Splits the active pane (side by side if vertical). Returns 0 on success. */
int editor_split(Editor *e, int vertical);
/* Shows an open document in the active pane. */
void editor_activate(Editor *e, Doc *d);
/* Moves the cursor of the active pane to a 1-based line and column. */
void editor_goto(Editor *e, size_t line, size_t col);
/* Display column of a cursor of any pane. */
size_t editor_view_col(const Buffer *b, const View *v, int tabw);
/* Makes the live view and the pane's copy agree. */
void editor_store_view(Editor *e);

/* Shows a message on the last row; ttl_ms == 0 keeps it until replaced. */
void editor_set_msg(Editor *e, long ttl_ms, const char *fmt, ...) KED_PRINTF(3, 4);
/* Display column of the cursor within its line. */
size_t editor_cursor_col(const Editor *e);

#endif
