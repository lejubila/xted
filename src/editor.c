#include "editor.h"
#include "utf8.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MSG_TTL_MS 5000

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void set_msg(Editor *e, long ttl_ms, const char *fmt, ...) KED_PRINTF(3, 4);

static void set_msg(Editor *e, long ttl_ms, const char *fmt, ...)
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
    e->render = render_create();
}

void editor_free(Editor *e)
{
    buf_free(&e->buf);
    render_free(e->render);
    e->render = NULL;
}

int editor_open(Editor *e, const char *path, char *err, size_t errlen)
{
    if (buf_load(&e->buf, path, err, errlen) < 0)
        return -1;
    if (e->buf.is_new)
        set_msg(e, MSG_TTL_MS, "Nuovo file");
    else if (e->buf.has_nul)
        set_msg(e, MSG_TTL_MS, "Attenzione: il file contiene byte NUL (binario?)");
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
    set_msg(e, 0, "%s", question);
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
        if (k.code == KEY_ENTER || k.code == '\n') {
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

static int is_word(uint32_t cp)
{
    return cp >= 0x80 || cp == '_' || (cp >= '0' && cp <= '9') ||
           (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

static int word_at(const Line *l, size_t i)
{
    uint32_t cp;

    utf8_decode(l->s + i, l->len - i, &cp);
    return is_word(cp);
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

/* ---- editing ----------------------------------------------------------- */

static void insert_cp(Editor *e, uint32_t cp)
{
    char b[4];
    int n = utf8_encode(cp, b);

    buf_insert(&e->buf, e->cy, e->cx, b, (size_t)n);
    e->cx += (size_t)n;
}

static void insert_newline(Editor *e)
{
    buf_split(&e->buf, e->cy, e->cx);
    e->cy++;
    e->cx = 0;
}

static void delete_backward(Editor *e)
{
    if (e->cx > 0) {
        Line *l = cur_line(e);
        size_t from = grapheme_prev(l->s, l->len, e->cx);

        buf_delete(&e->buf, e->cy, from, e->cx - from);
        e->cx = from;
    } else if (e->cy > 0) {
        e->cy--;
        e->cx = cur_line(e)->len;
        buf_join(&e->buf, e->cy);
    }
}

static void delete_forward(Editor *e)
{
    Line *l = cur_line(e);

    if (e->cx < l->len) {
        size_t to = grapheme_next(l->s, l->len, e->cx);

        buf_delete(&e->buf, e->cy, e->cx, to - e->cx);
    } else if (e->cy + 1 < e->buf.nlines) {
        buf_join(&e->buf, e->cy);
    }
}

/* ---- file commands ----------------------------------------------------- */

static int cmd_save(Editor *e)
{
    char name[sizeof e->prompt_text], err[256];
    const char *target = NULL;
    long n;

    if (!e->buf.path) {
        if (prompt(e, "Salva con nome: ", name, sizeof name) == 0) {
            set_msg(e, MSG_TTL_MS, "Salvataggio annullato");
            return -1;
        }
        target = name;
    }
    n = buf_save(&e->buf, target, err, sizeof err);
    if (n < 0) {
        set_msg(e, MSG_TTL_MS, "Errore di salvataggio: %s", err);
        return -1;
    }
    set_msg(e, MSG_TTL_MS, "%zu righe, %ld byte scritti", e->buf.nlines, n);
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

static void handle_key(Editor *e, Key k)
{
    int vertical = 0, ctrl = (k.mods & MOD_CTRL) != 0;

    if (k.mods & MOD_ALT)
        return;

    switch (k.code) {
    case KEY_UP:
        move_vertical(e, -1);
        vertical = 1;
        break;
    case KEY_DOWN:
        move_vertical(e, 1);
        vertical = 1;
        break;
    case KEY_PGUP:
        move_page(e, 0);
        vertical = 1;
        break;
    case KEY_PGDN:
        move_page(e, 1);
        vertical = 1;
        break;
    case KEY_LEFT:
        if (ctrl)
            move_word_left(e);
        else
            move_left(e);
        break;
    case KEY_RIGHT:
        if (ctrl)
            move_word_right(e);
        else
            move_right(e);
        break;
    case KEY_HOME:
        if (ctrl) {
            e->cy = 0;
            e->cx = 0;
        } else {
            move_home(e);
        }
        break;
    case KEY_END:
        if (ctrl)
            e->cy = e->buf.nlines - 1;
        e->cx = cur_line(e)->len;
        break;
    case KEY_ENTER:
    case '\n':
        insert_newline(e);
        break;
    case KEY_BACKSPACE:
    case 8:
        delete_backward(e);
        break;
    case KEY_DEL:
        delete_forward(e);
        break;
    case KEY_TAB:
        insert_cp(e, '\t');
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
        if (k.code >= 0x20 && k.code < KEY_SPECIAL && k.code != 0x7F &&
            !(k.code >= 0x80 && k.code < 0xA0))
            insert_cp(e, k.code);
        break;
    }

    if (!vertical)
        e->want_col = editor_cursor_col(e);
}

void editor_run(Editor *e)
{
    resize(e);
    if (!e->msg[0])
        set_msg(e, MSG_TTL_MS, "Ctrl+S salva  Ctrl+Q esci");

    while (!e->quit) {
        Key k;

        if (next_key(e, &k) < 0)
            break;
        handle_key(e, k);
    }
}
