#include "editor.h"
#include "edit.h"
#include "utf8.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void editor_set_msg(Editor *e, long ttl_ms, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof e->msg, fmt, ap);
    va_end(ap);
    e->msg_until = ttl_ms ? now_ms() + ttl_ms : 0;
}

static Line *cur_line(Editor *e)
{
    return &e->buf.lines[e->cy];
}

static size_t text_rows(const Editor *e)
{
    return e->rows >= 3 ? (size_t)e->rows - 2 : 1;
}

size_t editor_cursor_col(const Editor *e)
{
    const Line *l = &e->buf.lines[e->cy];

    return utf8_cols(l->s, l->len, e->cx, e->tabw);
}

void editor_init(Editor *e)
{
    memset(e, 0, sizeof *e);
    buf_init(&e->buf);
    e->tabw = 8;
    e->rows = 24;
    e->cols = 80;
    e->clipboard_osc52 = 1;
    e->theme = theme_get(0);
    e->color_depth = theme_detect_depth();
    e->render = render_create();
}

void editor_set_syntax(Editor *e, Syntax *syn)
{
    syn_free(e->syn);
    e->syn = syn;
    e->buf.stable = 0;
}

void editor_free(Editor *e)
{
    syn_free(e->syn);
    e->syn = NULL;
    buf_free(&e->buf);
    clip_free(&e->clip);
    render_free(e->render);
    e->render = NULL;
}

int editor_open(Editor *e, const char *path, char *err, size_t errlen)
{
    if (buf_load(&e->buf, path, err, errlen) < 0)
        return -1;
    editor_set_syntax(e, syn_detect(&e->buf));
    if (e->buf.is_new)
        editor_set_msg(e, MSG_TTL_MS, "Nuovo file");
    else if (e->buf.has_nul)
        editor_set_msg(e, MSG_TTL_MS, "Attenzione: il file contiene byte NUL (binario?)");
    return 0;
}

/* ---- viewport ---------------------------------------------------------- */

static void scroll(Editor *e)
{
    size_t th = text_rows(e), cw = (size_t)e->cols;
    size_t col = editor_cursor_col(e), w = 1;
    const Line *l = &e->buf.lines[e->cy];

    if (e->cy < e->rowoff)
        e->rowoff = e->cy;
    if (e->cy >= e->rowoff + th)
        e->rowoff = e->cy - th + 1;

    if (e->cx < l->len) {
        uint32_t cp;

        utf8_decode(l->s + e->cx, l->len - e->cx, &cp);
        w = (size_t)char_cols(cp, col, e->tabw);
        if (w == 0)
            w = 1;
    }
    if (w > cw)
        w = cw;
    if (col < e->coloff)
        e->coloff = col;
    if (col + w > e->coloff + cw)
        e->coloff = col + w - cw;
}

static void resize(Editor *e)
{
    term_size(&e->rows, &e->cols);
    render_invalidate(e->render);
}

/* ---- events ------------------------------------------------------------ */

/* Renders when idle, then returns the next key. -1 when the terminal is
 * gone (e->quit is set). */
static int next_key(Editor *e, Key *k)
{
    for (;;) {
        if (!term_has_input()) {
            int ev, timeout = -1;

            if (e->msg[0] && e->msg_until) {
                long left = e->msg_until - now_ms();

                if (left <= 0) {
                    e->msg[0] = '\0';
                    e->msg_until = 0;
                } else {
                    timeout = (int)left;
                }
            }
            scroll(e);
            syn_update(e->syn, &e->buf, e->rowoff + text_rows(e));
            e->match_on = syn_match_bracket(e->syn, &e->buf, e->cy, e->cx,
                                            &e->by, &e->bx, &e->my, &e->mx);
            render_frame(e->render, e);
            ev = term_wait(timeout);
            if (ev & TERM_EV_RESIZE)
                resize(e);
            if (!(ev & TERM_EV_KEY))
                continue;
        }
        term_read_key(k);
        if (k->code == KEY_EOF) {
            e->quit = 1;
            return -1;
        }
        if (k->code != KEY_NONE)
            return 0;
    }
}

/* ---- prompts ----------------------------------------------------------- */

/* Shows a question and waits for one of the letters in keys (case
 * insensitive). Returns it in lowercase, or 0 on Esc. */
static int ask(Editor *e, const char *question, const char *keys)
{
    editor_set_msg(e, 0, "%s", question);
    for (;;) {
        Key k;
        uint32_t c;

        if (next_key(e, &k) < 0)
            return 0;
        if (k.code == KEY_ESC) {
            e->msg[0] = '\0';
            return 0;
        }
        c = (k.code >= 'A' && k.code <= 'Z') ? k.code + 32 : k.code;
        if (c != 0 && c < 0x80 && strchr(keys, (int)c)) {
            e->msg[0] = '\0';
            return (int)c;
        }
    }
}

/* Single-line text input on the message row. Returns the length, 0 if
 * cancelled. */
static size_t prompt(Editor *e, const char *label, char *out, size_t outsz)
{
    int ok = 0;

    e->prompt_on = 1;
    snprintf(e->prompt_label, sizeof e->prompt_label, "%s", label);
    e->prompt_len = 0;
    e->prompt_text[0] = '\0';

    for (;;) {
        Key k;

        if (next_key(e, &k) < 0)
            break;
        if (k.code == KEY_ENTER) {
            if (e->prompt_len) {
                ok = 1;
                break;
            }
        } else if (k.code == KEY_ESC) {
            break;
        } else if (k.code == KEY_BACKSPACE || k.code == 8) {
            if (e->prompt_len) {
                e->prompt_len = utf8_prev(e->prompt_text, e->prompt_len);
                e->prompt_text[e->prompt_len] = '\0';
            }
        } else if (k.code >= 0x20 && k.code < KEY_SPECIAL && k.code != 0x7F &&
                   !(k.mods & (MOD_ALT | MOD_CTRL))) {
            char b[4];
            size_t n = (size_t)utf8_encode(k.code, b);

            if (e->prompt_len + n < sizeof e->prompt_text) {
                memcpy(e->prompt_text + e->prompt_len, b, n);
                e->prompt_len += n;
                e->prompt_text[e->prompt_len] = '\0';
            }
        }
    }
    e->prompt_on = 0;
    if (!ok)
        return 0;
    snprintf(out, outsz, "%s", e->prompt_text);
    return e->prompt_len;
}

/* ---- movement ---------------------------------------------------------- */

/* Called before a movement key: Shift starts or extends the selection,
 * anything else drops it. */
static void sel_begin(Editor *e, int shift)
{
    if (!shift) {
        e->sel_on = 0;
    } else if (!edit_sel_active(e)) {
        e->sel_on = 1;
        e->ay = e->cy;
        e->ax = e->cx;
    }
}

static void move_left(Editor *e)
{
    if (e->cx > 0) {
        Line *l = cur_line(e);

        e->cx = grapheme_prev(l->s, l->len, e->cx);
    } else if (e->cy > 0) {
        e->cy--;
        e->cx = cur_line(e)->len;
    }
}

static void move_right(Editor *e)
{
    Line *l = cur_line(e);

    if (e->cx < l->len) {
        e->cx = grapheme_next(l->s, l->len, e->cx);
    } else if (e->cy + 1 < e->buf.nlines) {
        e->cy++;
        e->cx = 0;
    }
}

static void move_vertical(Editor *e, long delta)
{
    long ny = (long)e->cy + delta;
    Line *l;

    if (ny < 0)
        ny = 0;
    if (ny >= (long)e->buf.nlines)
        ny = (long)e->buf.nlines - 1;
    e->cy = (size_t)ny;
    l = cur_line(e);
    e->cx = utf8_byte_at_col(l->s, l->len, e->want_col, e->tabw);
}

static void move_page(Editor *e, int down)
{
    size_t th = text_rows(e), page = th > 1 ? th - 1 : 1;
    size_t maxoff = e->buf.nlines > th ? e->buf.nlines - th : 0;

    if (down) {
        e->rowoff += page;
        if (e->rowoff > maxoff)
            e->rowoff = maxoff;
        move_vertical(e, (long)page);
    } else {
        e->rowoff = e->rowoff > page ? e->rowoff - page : 0;
        move_vertical(e, -(long)page);
    }
}

/* Smart home: first non-blank character, then column 0 on the next press. */
static void move_home(Editor *e)
{
    Line *l = cur_line(e);
    size_t fnb = 0;

    while (fnb < l->len && (l->s[fnb] == ' ' || l->s[fnb] == '\t'))
        fnb++;
    e->cx = (e->cx == fnb) ? 0 : fnb;
}

static int word_at(const Line *l, size_t i)
{
    uint32_t cp;

    utf8_decode(l->s + i, l->len - i, &cp);
    return cp_is_word(cp);
}

static void move_word_right(Editor *e)
{
    Line *l = cur_line(e);
    size_t i = e->cx;

    if (i >= l->len) {
        if (e->cy + 1 < e->buf.nlines) {
            e->cy++;
            e->cx = 0;
        }
        return;
    }
    while (i < l->len && !word_at(l, i))
        i = utf8_next(l->s, l->len, i);
    while (i < l->len && word_at(l, i))
        i = utf8_next(l->s, l->len, i);
    e->cx = i;
}

static void move_word_left(Editor *e)
{
    Line *l = cur_line(e);
    size_t i = e->cx;

    if (i == 0) {
        if (e->cy > 0) {
            e->cy--;
            e->cx = cur_line(e)->len;
        }
        return;
    }
    while (i > 0 && !word_at(l, utf8_prev(l->s, i)))
        i = utf8_prev(l->s, i);
    while (i > 0 && word_at(l, utf8_prev(l->s, i)))
        i = utf8_prev(l->s, i);
    e->cx = i;
}

/* ---- file commands ----------------------------------------------------- */

static int cmd_save(Editor *e)
{
    char name[sizeof e->prompt_text], err[256];
    const char *target = NULL;
    int had_path = e->buf.path != NULL;
    long n;

    if (!had_path) {
        if (prompt(e, "Salva con nome: ", name, sizeof name) == 0) {
            editor_set_msg(e, MSG_TTL_MS, "Salvataggio annullato");
            return -1;
        }
        target = name;
    }
    n = buf_save(&e->buf, target, err, sizeof err);
    if (n < 0) {
        editor_set_msg(e, MSG_TTL_MS, "Errore di salvataggio: %s", err);
        return -1;
    }
    if (!had_path)
        editor_set_syntax(e, syn_detect(&e->buf));
    editor_set_msg(e, MSG_TTL_MS, "%zu righe, %ld byte scritti", e->buf.nlines, n);
    return 0;
}

static void cmd_quit(Editor *e)
{
    if (e->buf.modified) {
        int c = ask(e, "Modifiche non salvate. Salvare? (s = sì, n = no, Esc = annulla)", "sn");

        if (c == 0)
            return;
        if (c == 's' && cmd_save(e) < 0)
            return;
    }
    e->quit = 1;
}

/* ---- dispatch ---------------------------------------------------------- */

static int is_printable(uint32_t code)
{
    return code >= 0x20 && code < KEY_SPECIAL && code != 0x7F && !(code >= 0x80 && code < 0xA0);
}

/* Typing, Backspace and Delete keep one undo group open, so a run of them
 * is undone in one step; every other key closes it. */
static int is_typing(Key k)
{
    if (k.mods & (MOD_ALT | MOD_CTRL))
        return 0;
    if (k.code == KEY_BACKSPACE || k.code == 8 || k.code == KEY_DEL)
        return 1;
    if (k.code == KEY_TAB)
        return !(k.mods & MOD_SHIFT);
    return is_printable(k.code);
}

/* Returns 1 for vertical movement, which keeps the wanted column. */
static int handle_alt(Editor *e, Key k, int shift)
{
    switch (k.code) {
    case KEY_UP:
        if (shift)
            edit_duplicate_lines(e, 0);
        else
            edit_move_lines(e, 0);
        return 1;
    case KEY_DOWN:
        if (shift)
            edit_duplicate_lines(e, 1);
        else
            edit_move_lines(e, 1);
        return 1;
    case 'l':
        edit_select_line(e);
        break;
    case 'w':
        edit_select_word(e);
        break;
    case 'v':
        edit_paste_older(e);
        break;
    case 'm':
        if (e->match_on) {
            e->sel_on = 0;
            e->cy = e->my;
            e->cx = e->mx;
        } else {
            editor_set_msg(e, MSG_TTL_MS, "Nessuna parentesi corrispondente");
        }
        break;
    case 't':
        e->theme_idx++;
        e->theme = theme_get(e->theme_idx);
        render_invalidate(e->render);
        editor_set_msg(e, MSG_TTL_MS, "Tema: %s", e->theme->name);
        break;
    default:
        break;
    }
    return 0;
}

static int handle_plain(Editor *e, Key k, int shift, int ctrl)
{
    switch (k.code) {
    case KEY_UP:
        sel_begin(e, shift);
        move_vertical(e, -1);
        return 1;
    case KEY_DOWN:
        sel_begin(e, shift);
        move_vertical(e, 1);
        return 1;
    case KEY_PGUP:
        sel_begin(e, shift);
        move_page(e, 0);
        return 1;
    case KEY_PGDN:
        sel_begin(e, shift);
        move_page(e, 1);
        return 1;
    case KEY_LEFT:
        if (!shift && edit_sel_active(e)) {
            edit_sel_collapse(e, 0);
            break;
        }
        sel_begin(e, shift);
        if (ctrl)
            move_word_left(e);
        else
            move_left(e);
        break;
    case KEY_RIGHT:
        if (!shift && edit_sel_active(e)) {
            edit_sel_collapse(e, 1);
            break;
        }
        sel_begin(e, shift);
        if (ctrl)
            move_word_right(e);
        else
            move_right(e);
        break;
    case KEY_HOME:
        sel_begin(e, shift);
        if (ctrl) {
            e->cy = 0;
            e->cx = 0;
        } else {
            move_home(e);
        }
        break;
    case KEY_END:
        sel_begin(e, shift);
        if (ctrl)
            e->cy = e->buf.nlines - 1;
        e->cx = cur_line(e)->len;
        break;
    case KEY_ENTER:
        edit_newline(e);
        break;
    case KEY_BACKSPACE:
    case 8:
        edit_backspace(e);
        break;
    case KEY_DEL:
        edit_delete(e);
        break;
    case KEY_TAB:
        if (shift)
            edit_dedent(e);
        else
            edit_tab(e);
        break;
    case KEY_ESC:
        e->sel_on = 0;
        break;
    case KEY_PASTE: {
        size_t n;
        const char *s = term_paste_text(&n);

        edit_paste_text(e, s, n);
        break;
    }
    case CTRL('a'):
        edit_select_all(e);
        break;
    case CTRL('c'):
        edit_copy(e);
        break;
    case CTRL('x'):
        edit_cut(e);
        break;
    case CTRL('v'):
        edit_paste(e);
        break;
    case CTRL('z'):
        edit_undo(e);
        break;
    case CTRL('y'):
        edit_redo(e);
        break;
    case CTRL('k'):
        edit_delete_lines(e);
        break;
    case CTRL('j'):
        edit_join_lines(e);
        break;
    case CTRL('s'):
        cmd_save(e);
        break;
    case CTRL('q'):
        cmd_quit(e);
        break;
    case CTRL('l'):
        render_invalidate(e->render);
        break;
    default:
        if (is_printable(k.code))
            edit_insert_cp(e, k.code);
        break;
    }
    return 0;
}

static void handle_key(Editor *e, Key k)
{
    int alt = (k.mods & MOD_ALT) != 0, shift = (k.mods & MOD_SHIFT) != 0;
    int vertical;

    if (!is_typing(k))
        buf_undo_close(&e->buf);
    if (!(alt && k.code == 'v'))
        e->paste_valid = 0;

    if (alt)
        vertical = handle_alt(e, k, shift);
    else
        vertical = handle_plain(e, k, shift, (k.mods & MOD_CTRL) != 0);

    if (!vertical)
        e->want_col = editor_cursor_col(e);
}

void editor_run(Editor *e)
{
    resize(e);
    if (!e->msg[0])
        editor_set_msg(e, MSG_TTL_MS, "Ctrl+S salva  Ctrl+Q esci");

    while (!e->quit) {
        Key k;

        if (next_key(e, &k) < 0)
            break;
        handle_key(e, k);
    }
}
