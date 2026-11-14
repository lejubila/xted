#ifndef KED_EDITOR_H
#define KED_EDITOR_H

#include "buffer.h"
#include "doc.h"
#include "files.h"
#include "keymap.h"
#include "layout.h"
#include "picker.h"
#include "clip.h"
#include "config.h"
#include "complete.h"
#include "render.h"
#include "search.h"
#include "shell.h"
#include "tags.h"
#include "snippet.h"
#include "syntax.h"
#include "theme.h"
#include "term.h"
#include "util.h"

#include <stddef.h>

#define MSG_TTL_MS 5000
#define SIDEBAR_W 28

enum { PK_BUFFERS = 1, PK_FILES, PK_RESULTS, PK_COMMANDS, PK_TAGS };

enum { FIND_OFF = 0, FIND_SEARCH, FIND_WITH, FIND_CONFIRM, FIND_FILES };

#define FIND_MAX_RESULTS 2000

/* Search and replace state (see find.c). */
typedef struct {
    int mode;               /* FIND_* */
    Matcher *m;             /* the current pattern (NULL until the first search) */
    char pat[512];          /* last accepted pattern */
    char repl[512];         /* last replacement text */
    int regex, cs, word;    /* options */
    int hl;                 /* highlight the matches of m */
    int replace;            /* the bar is for replacing */
    size_t oy, ox;          /* where the search started */
    int osel;               /* the selection at that moment */
    size_t oay, oax;
    int hit_valid;
    Hit hit;                /* the match shown */
    int has_span;
    Span span;              /* replace only inside this part of the text */
    size_t sy, sx;          /* where the replacement started */
    int passed_end;
    size_t nrepl;
    int undo_open;
    FileHits files;         /* results of the last search in files */
    TagHits tags;           /* definitions offered by the last F12 */
    char *tags_dir;         /* the folder of the tags file they came from */
    char tag_name[128];
} Find;


/* An extra cursor (multi-cursor mode); the primary one lives in Editor.cy/cx/sel_*. */
typedef struct {
    size_t cy, cx;
    size_t want_col;
    int sel_on;
    size_t ay, ax;
} Cursor;

/* The command running in the background and its output (see run.c). */
typedef struct {
    Doc *out;               /* the "[Output]" document, NULL until the first run */
    Job job;
    int on;                 /* a command runs (its pipe may be closed already) */
    char cmd[512];          /* the last command */
    char filter[512];       /* the last filter */
    long t0;                /* when it started, monotonic ms */
    size_t bytes;           /* output kept so far */
    int trunc;
    int ansi;               /* inside an escape sequence we are stripping */
    size_t err_line;        /* output line of the last error visited, plus one */
} Run;

/* Mouse state kept between events (see mouse.c). */
typedef struct {
    long t;                 /* monotonic ms of the last press */
    int x, y, n;            /* where it was, and how many presses in a row (1..3) */
    int drag;               /* DRAG_*: what the running drag moves */
    struct Node *node;      /* the split whose border is dragged */
} MouseState;

enum { DRAG_NONE, DRAG_TEXT, DRAG_BORDER, DRAG_SIDEBAR };

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
    Find find;
    Run run;
    Keymap km;              /* which key runs which command */
    Config cfg;             /* the user's settings */
    int git_off;            /* git could not be run: the margin is off for this session */
    int mouse_on;           /* the terminal reports clicks and the wheel */
    int resize_mode;        /* the arrow keys move split borders (Alt+E) */
    MouseState mouse;
    char *cfgdir;           /* where they were read from (NULL: unknown) */
    Complete comp;          /* suggestion popup */
    SnipSession snip;       /* running snippet: Tab visits its stops */
    Cursor *xc;             /* extra cursors, in text order; see cursors.c */
    size_t nxc, xc_cap;
    int undo_hold;          /* inside cur_each: one undo group for all cursors */
    int picker_kind;        /* PK_* */

    /* the active pane's view, live: it is copied into the pane when focus moves */
    size_t cy, cx;          /* cursor: line, byte offset in the line */
    size_t want_col;        /* display column kept across vertical moves */
    size_t rowoff, coloff;  /* first visible line / display column */
    int rows, cols;         /* terminal size */
    int text_x, text_y;     /* top-left of the pane area (right of the sidebar, below the tab bar) */
    int sidebar_w;          /* columns taken by the browser (0 when hidden) */
    int sidebar_cols;       /* its width when shown */
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

    char hint[256];         /* "name(params)" of the call at the cursor, for the status bar */
    size_t hint_a, hint_b;  /* the current parameter inside it */
    const Doc *hint_doc;    /* where it was computed, to do it again only after a change */
    size_t hint_y, hint_x;
    unsigned long hint_gen;

    char msg[256];
    long msg_until;         /* monotonic ms when msg expires; 0 = sticky */

    int prompt_on;
    char prompt_label[64];
    char prompt_text[512];
    size_t prompt_len;
    char prompt_info[64];   /* shown at the right end of the prompt row */

    int quit;
    Render *render;
} Editor;

void editor_init(Editor *e);
void editor_free(Editor *e);
/* Loads path into the buffer (before the terminal is initialised). */
int editor_open(Editor *e, const char *path, char *err, size_t errlen);
/* Reads the settings, themes and languages of the user and applies them (before the terminal
 * is started; a problem becomes the first message on screen). */
void editor_load_config(Editor *e);
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
/* Handles one key as if typed (the mouse uses it to press Enter in lists). */
void editor_key(Editor *e, Key k);
/* Shows an open document in the active pane. */
void editor_activate(Editor *e, Doc *d);
/* Moves the cursor of the active pane to a 1-based line and column. */
void editor_goto(Editor *e, size_t line, size_t col);
/* Splits the active pane giving the original pct percent; the new pane becomes active. */
int editor_split_at(Editor *e, int vertical, int pct);
/* Moves the keyboard focus to pane w. */
void editor_focus(Editor *e, Win *w);
/* Lays the panes out again (after the browser or a split border moved). */
void editor_relayout(Editor *e);
/* Re-reads the active pane's view into the live fields (after the run code edited it). */
void editor_reload_view(Editor *e);
/* One-key question on the message row: returns the lowercase key of keys, 0 on Esc. */
int editor_ask(Editor *e, const char *question, const char *keys);
/* Single-line text input on the message row (init is the starting text).
 * Returns the length, 0 if cancelled. */
size_t editor_prompt(Editor *e, const char *label, const char *init, char *out, size_t outsz);
/* Display column of a cursor of any pane. */
size_t editor_view_col(const Buffer *b, const View *v, int tabw);
/* Makes the live view and the pane's copy agree. */
void editor_store_view(Editor *e);

/* Shows a message on the last row; ttl_ms == 0 keeps it until replaced. */
void editor_set_msg(Editor *e, long ttl_ms, const char *fmt, ...) KED_PRINTF(3, 4);
/* Display column of the cursor within its line. */
size_t editor_cursor_col(const Editor *e);

/* Commands that live in editor.c, available to the palette (see commands.c). */
void editor_cmd_save(Editor *e);
void editor_cmd_save_as(Editor *e);
void editor_cmd_save_all(Editor *e);
void editor_cmd_open(Editor *e);
void editor_cmd_quick_open(Editor *e);
void editor_cmd_buffers(Editor *e);
void editor_cmd_close_buffer(Editor *e);
void editor_cmd_quit(Editor *e);
void editor_cmd_next_buffer(Editor *e);
void editor_cmd_prev_buffer(Editor *e);
void editor_cmd_split_v(Editor *e);
void editor_cmd_split_h(Editor *e);
void editor_cmd_close_win(Editor *e);
void editor_cmd_next_win(Editor *e);
void editor_cmd_prev_win(Editor *e);
void editor_cmd_toggle_tree(Editor *e);
void editor_cmd_match(Editor *e);
void editor_cmd_theme(Editor *e);
void editor_cmd_indent_tabs(Editor *e);
void editor_cmd_indent_spaces2(Editor *e);
void editor_cmd_indent_spaces4(Editor *e);
void editor_cmd_indent_spaces8(Editor *e);
void editor_cmd_indent_to_tabs(Editor *e);
void editor_cmd_indent_to_spaces(Editor *e);
void editor_cmd_indent_detect(Editor *e);
void editor_cmd_redraw(Editor *e);
void editor_cmd_git(Editor *e);
void editor_cmd_toggle_mouse(Editor *e);
void editor_cmd_resize_mode(Editor *e);
void editor_cmd_equalize(Editor *e);
/* Alt+X: the list of all commands. */
void editor_cmd_palette(Editor *e);

#endif
