#include "editor.h"
#include "edit.h"
#include "commands.h"
#include "config.h"
#include "cursors.h"
#include "find.h"
#include "fold.h"
#include "git.h"
#include "mouse.h"
#include "run.h"
#include "session.h"
#include "utf8.h"
#include "util.h"

#include <stdlib.h>
#include <unistd.h>

#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
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
    return &e->buf->lines[e->cy];
}

/* Size of the active pane's text area. */
static size_t text_rows(const Editor *e)
{
    Win *w = e->layout.active;

    return w && w->h >= 2 ? (size_t)w->h - 1 : 1;
}

static size_t text_cols(const Editor *e)
{
    Win *w = e->layout.active;

    return w ? (size_t)win_text_w(w) : 1;
}

size_t editor_view_col(const Buffer *b, const View *v, int tabw)
{
    const Line *l = &b->lines[v->cy];

    return utf8_cols(l->s, l->len, v->cx, tabw);
}

size_t editor_cursor_col(const Editor *e)
{
    const Line *l = &e->buf->lines[e->cy];

    return utf8_cols(l->s, l->len, e->cx, e->tabw);
}

/* ---- views, documents, panes ------------------------------------------- */

/* Brings a view back inside the text: another pane may have shortened it. */
static void view_clamp(const Buffer *b, View *v)
{
    const Line *l;

    if (v->cy >= b->nlines)
        v->cy = b->nlines - 1;
    l = &b->lines[v->cy];
    if (v->cx > l->len)
        v->cx = l->len;
    while (v->cx > 0 && v->cx < l->len && ((unsigned char)l->s[v->cx] & 0xC0) == 0x80)
        v->cx--;
    if (v->sel_on && v->ay >= b->nlines)
        v->sel_on = 0;
    if (v->sel_on && v->ax > b->lines[v->ay].len)
        v->sel_on = 0;
    if (v->rowoff >= b->nlines)
        v->rowoff = b->nlines - 1;
}

void editor_store_view(Editor *e)
{
    Win *w = e->layout.active;
    View *v = &w->view;

    v->cy = e->cy;
    v->cx = e->cx;
    v->want_col = e->want_col;
    v->rowoff = e->rowoff;
    v->coloff = e->coloff;
    v->sel_on = e->sel_on;
    v->ay = e->ay;
    v->ax = e->ax;
    w->doc->last = *v;
}

/* Makes w the active pane: its document and view become the live ones. */
static void load_win(Editor *e, Win *w)
{
    View *v = &w->view;

    e->layout.active = w;
    e->doc = w->doc;
    e->buf = &w->doc->buf;
        view_clamp(e->buf, v);
    e->cy = v->cy;
    e->cx = v->cx;
    e->want_col = v->want_col;
    e->rowoff = v->rowoff;
    e->coloff = v->coloff;
    e->sel_on = v->sel_on;
    e->ay = v->ay;
    e->ax = v->ax;
    e->paste_valid = 0;
    e->match_on = 0;
    e->tabw = w->doc->tabw;
    e->snip.on = 0;
    complete_close(&e->comp);
    cur_clear(e);
}

/* Shows d in the active pane. */
static void show_doc(Editor *e, Doc *d)
{
    Win *w = e->layout.active;

    editor_store_view(e);
    buf_undo_close(e->buf);
    w->doc = d;
    w->view = d->last;
    load_win(e, w);
}

void editor_activate(Editor *e, Doc *d)
{
    if (d != e->doc)
        show_doc(e, d);
}

static size_t doc_index(const Editor *e, const Doc *d)
{
    size_t i;

    for (i = 0; i < e->ndocs; i++)
        if (e->docs[i] == d)
            return i;
    return 0;
}

static Doc *add_doc(Editor *e, Doc *d)
{
    if (e->ndocs == e->docs_cap) {
        e->docs_cap = e->docs_cap ? e->docs_cap * 2 : 8;
        e->docs = xrealloc(e->docs, e->docs_cap * sizeof *e->docs);
    }
    e->docs[e->ndocs++] = d;
    return d;
}

/* Layout of the screen: tab bar, panes, message row. */
static int tabbar_visible(const Editor *e)
{
    return e->ndocs >= 2 && e->rows >= 6;
}

static void relayout(Editor *e)
{
    int y0 = tabbar_visible(e) ? 1 : 0, x0 = 0, h = e->rows - y0 - 1;
    int sb = e->sidebar_on ? e->sidebar_cols : 0;

    if (sb > e->cols / 2)
        sb = e->cols / 2;
    x0 = sb > 0 ? sb + 1 : 0;
    e->text_x = x0;
    e->text_y = y0;
    e->sidebar_w = sb;
    if (h < 2)
        h = 2;
    layout_compute(&e->layout, x0, y0, e->cols - x0 > 0 ? e->cols - x0 : 1, h);
}

void editor_init(Editor *e)
{
    Doc *d = doc_new();

    memset(e, 0, sizeof *e);
    add_doc(e, d);
    layout_init(&e->layout, d);
    load_win(e, e->layout.active);
    e->tabw = 8;
    e->rows = 24;
    e->cols = 80;
    e->clipboard_osc52 = 1;
    e->mouse_on = 1;
    e->sidebar_cols = SIDEBAR_W;
    e->theme = theme_get(0);
    e->color_depth = theme_detect_depth();
    e->render = render_create();
    keymap_init(&e->km);
    config_init(&e->cfg);
    relayout(e);
}

void editor_free(Editor *e)
{
    size_t i;

    layout_free(&e->layout);
    for (i = 0; i < e->ndocs; i++)
        doc_free(e->docs[i]);
    free(e->docs);
    picker_free(&e->picker);
    find_free(e);
    complete_free(&e->comp);
    cur_clear(e);
    run_kill(e);
    keymap_free(&e->km);
    config_free(&e->cfg);
    free(e->cfgdir);
    e->theme = theme_get(0);
    theme_clear_user();
    syn_clear_user();
    tree_free(&e->tree);
    free(e->root);
    clip_free(&e->clip);
    render_free(e->render);
    e->render = NULL;
}

static int same_file(const char *a, const char *b)
{
    char *ra = realpath(a, NULL), *rb = realpath(b, NULL);
    int r = ra && rb && strcmp(ra, rb) == 0;

    free(ra);
    free(rb);
    return r;
}

static void note_doc(Editor *e, const Doc *d)
{
    if (d->buf.is_new)
        editor_set_msg(e, MSG_TTL_MS, "Nuovo file");
    else if (d->buf.has_nul)
        editor_set_msg(e, MSG_TTL_MS, "Attenzione: il file contiene byte NUL (binario?)");
    else if (d->readonly)
        editor_set_msg(e, MSG_TTL_MS, "File in sola lettura");
}

/* An untouched, unnamed, empty document is replaced by the first file opened. */
static int is_blank_doc(const Doc *d)
{
    return !d->buf.path && !d->buf.modified && d->buf.nlines == 1 && d->buf.lines[0].len == 0;
}

/* The settings of the language the document turned out to be in. */
static void apply_cfg(Editor *e, Doc *d)
{
    LangCfg l = config_lang(&e->cfg, d->syn ? syn_name(d->syn) : NULL);

    d->tabw = l.tab_width;
    d->expand_tab = l.expand_tab;
    d->trim_space = l.trim_trailing_space;
    d->final_nl = l.final_newline;
    d->autosuggest = l.autosuggest;
    d->suggest_min = l.suggest_min;
    if (d == e->doc)
        e->tabw = d->tabw;
}

static void report_cfg(const char *msg, void *ctx)
{
    config_error(&((Editor *)ctx)->cfg, 0, msg);
}

void editor_load_config(Editor *e)
{
    size_t i;
    char msg[200];

    free(e->cfgdir);
    e->cfgdir = config_dir();
    e->theme = theme_get(0);
    e->theme_idx = 0;
    theme_clear_user();
    syn_clear_user();
    if (e->cfgdir) {
        size_t sn = strlen(e->cfgdir) + 16;
        char *sub = xmalloc(sn);

        config_load(&e->cfg, e->cfgdir);
        snprintf(sub, sn, "%s/themes", e->cfgdir);
        theme_load_dir(sub, report_cfg, e);
        snprintf(sub, sn, "%s/syntax", e->cfgdir);
        syn_load_dir(sub, report_cfg, e);
        free(sub);
    }
    if (e->cfg.clipboard >= 0)
        e->clipboard_osc52 = e->cfg.clipboard;
    if (e->cfg.mouse >= 0)
        e->mouse_on = e->cfg.mouse;
    if (e->cfg.theme[0]) {
        int t = theme_by_name(e->cfg.theme);

        if (t >= 0) {
            e->theme_idx = (size_t)t;
            e->theme = theme_get((size_t)t);
        } else {
            snprintf(msg, sizeof msg, "tema «%s» sconosciuto", e->cfg.theme);
            config_error(&e->cfg, 0, msg);
        }
    }
    for (i = 0; i < e->cfg.nkeys; i++) {
        const KeyLine *kl = &e->cfg.keys[i];
        Key k;
        int cmd = -1;

        if (key_parse(kl->key, &k, msg, sizeof msg) < 0) {
            config_error(&e->cfg, kl->line, msg);
            continue;
        }
        if (strcmp(kl->cmd, "none") != 0) {
            cmd = command_by_id(kl->cmd);
            if (cmd < 0) {
                snprintf(msg, sizeof msg, "comando «%s» sconosciuto", kl->cmd);
                config_error(&e->cfg, kl->line, msg);
                continue;
            }
        }
        keymap_bind(&e->km, k, cmd);
    }
    for (i = 0; i < e->ndocs; i++)
        apply_cfg(e, e->docs[i]);
    if (e->cfg.nerrors > 0)
        editor_set_msg(e, 0, "config: %s%s", e->cfg.first_error,
                       e->cfg.nerrors > 1 ? " (e altri errori)" : "");
}

/* Marks the lines of d that differ from HEAD. Only for a document that matches its file on disk. */
static void git_refresh_doc(Editor *e, Doc *d)
{
    uint8_t *marks = NULL;
    struct stat st;
    size_t i;
    int r;

    if (e->git_off || e->cfg.git == 0 || !d->buf.path || d->buf.modified || d->scratch)
        return;
    if (stat(d->buf.path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (20 << 20))
        return;
    r = git_file_marks(d->buf.path, d->buf.nlines, &marks);
    if (r < 0) {
        /* no git (or it failed): stop asking for this session */
        e->git_off = 1;
        r = 0;
    }
    for (i = 0; i < d->buf.nlines; i++)
        d->buf.lines[i].git = r == 1 ? marks[i] : GIT_NONE;
    d->gutter = r == 1;
    free(marks);
}

void editor_cmd_git(Editor *e)
{
    if (e->doc->buf.modified)
        editor_set_msg(e, MSG_TTL_MS, "Salva il file per aggiornare gli indicatori Git");
    else {
        e->git_off = 0;
        git_refresh_doc(e, e->doc);
        relayout(e);
        editor_set_msg(e, MSG_TTL_MS, e->doc->gutter ? "Indicatori Git aggiornati"
                                                      : "Il file non è in un repository Git");
    }
}

static Doc *install_doc(Editor *e, Doc *d)
{
    apply_cfg(e, d);
    git_refresh_doc(e, d);
    Doc *cur = e->doc;

    if (e->ndocs == 1 && is_blank_doc(cur)) {
        size_t i = doc_index(e, cur);

        e->docs[i] = d;
        e->layout.active->doc = d;
        e->layout.active->view = d->last;
        {
            Win *w[64];
            size_t n = layout_wins(&e->layout, w, 64), k;

            for (k = 0; k < n && k < 64; k++)
                if (w[k]->doc == cur) {
                    w[k]->doc = d;
                    w[k]->view = d->last;
                }
        }
        doc_free(cur);
        load_win(e, e->layout.active);
    } else {
        add_doc(e, d);
        show_doc(e, d);
    }
    return d;
}

Doc *editor_open_doc(Editor *e, const char *path, char *err, size_t errlen)
{
    size_t i;
    Doc *d;

    for (i = 0; i < e->ndocs; i++)
        if (e->docs[i]->buf.path && same_file(e->docs[i]->buf.path, path)) {
            show_doc(e, e->docs[i]);
            return e->docs[i];
        }
    d = doc_new();
    if (doc_load(d, path, err, errlen) < 0) {
        doc_free(d);
        return NULL;
    }
    install_doc(e, d);
    note_doc(e, d);
    return d;
}

Doc *editor_open_fd(Editor *e, int fd, char *err, size_t errlen)
{
    Doc *d = doc_new();

    if (doc_load_fd(d, fd, err, errlen) < 0) {
        doc_free(d);
        return NULL;
    }
    d->buf.modified = 1;        /* there is no file behind it yet */
    install_doc(e, d);
    return d;
}

void editor_goto(Editor *e, size_t line, size_t col)
{
    Line *l;

    if (line == 0)
        line = 1;
    e->cy = line - 1 < e->buf->nlines ? line - 1 : e->buf->nlines - 1;
    l = cur_line(e);
    e->cx = utf8_byte_at_col(l->s, l->len, col > 0 ? col - 1 : 0, e->tabw);
    e->want_col = editor_cursor_col(e);
    e->sel_on = 0;
}

/* ---- viewport ---------------------------------------------------------- */

static void scroll_view(const Buffer *b, View *v, size_t th, size_t cw, int tabw)
{
    const Line *l = &b->lines[v->cy];
    size_t col = editor_view_col(b, v, tabw), w = 1;

    if (b->folded && v->rowoff < b->nlines)
        v->rowoff = fold_head(b, v->rowoff);
    if (v->cy < v->rowoff)
        v->rowoff = v->cy;
    else if (fold_count(b, v->rowoff, v->cy) >= th)
        v->rowoff = fold_back(b, v->cy, th - 1);

    if (v->cx < l->len) {
        uint32_t cp;

        utf8_decode(l->s + v->cx, l->len - v->cx, &cp);
        w = (size_t)char_cols(cp, col, tabw);
        if (w == 0)
            w = 1;
    }
    if (w > cw)
        w = cw;
    if (col < v->coloff)
        v->coloff = col;
    if (col + w > v->coloff + cw)
        v->coloff = col + w - cw;
}

/* Keeps the cursor of the active pane on screen. */
static void scroll(Editor *e)
{
    View v;

    v.cy = e->cy;
    v.cx = e->cx;
    v.rowoff = e->rowoff;
    v.coloff = e->coloff;
    scroll_view(e->buf, &v, text_rows(e), text_cols(e), e->tabw);
    e->rowoff = v.rowoff;
    e->coloff = v.coloff;
}

/* Everything the renderer needs to be true before it draws. */
static void prepare_frame(Editor *e)
{
    Win *w[64];
    size_t n, i;

    relayout(e);
    if (e->tree_ready)
        tree_fit(&e->tree, (size_t)(e->rows - e->text_y - 1));
    fold_reveal(e);
    scroll(e);
    editor_store_view(e);
    n = layout_wins(&e->layout, w, 64);
    for (i = 0; i < n && i < 64; i++) {
        Doc *d = w[i]->doc;
        size_t th = w[i]->h >= 2 ? (size_t)w[i]->h - 1 : 1;

        view_clamp(&d->buf, &w[i]->view);
        if (w[i] != e->layout.active)
            scroll_view(&d->buf, &w[i]->view, th, (size_t)win_text_w(w[i]), d->tabw);
        syn_update(d->syn, &d->buf, w[i]->view.rowoff + th);
    }
    e->match_on = syn_match_bracket(e->doc->syn, e->buf, e->cy, e->cx,
                                    &e->by, &e->bx, &e->my, &e->mx);
}

static void resize(Editor *e)
{
    term_size(&e->rows, &e->cols);
    relayout(e);
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
            prepare_frame(e);
            render_frame(e->render, e);
            term_watch(run_fd(e));
            if (run_timeout(e) >= 0 && (timeout < 0 || run_timeout(e) < timeout))
                timeout = run_timeout(e);
            ev = term_wait(timeout);
            if (ev & TERM_EV_RESIZE)
                resize(e);
            if ((ev & TERM_EV_FD) || (!ev && run_timeout(e) >= 0))
                run_pump(e);
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
static size_t prompt(Editor *e, const char *label, const char *init, char *out, size_t outsz)
{
    int ok = 0;

    e->prompt_on = 1;
    snprintf(e->prompt_label, sizeof e->prompt_label, "%s", label);
    snprintf(e->prompt_text, sizeof e->prompt_text, "%s", init ? init : "");
    e->prompt_len = strlen(e->prompt_text);

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

size_t editor_prompt(Editor *e, const char *label, const char *init, char *out, size_t outsz)
{
    return prompt(e, label, init, out, outsz);
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
        e->cy = fold_prev(e->buf, e->cy);
        e->cx = cur_line(e)->len;
    }
}

static void move_right(Editor *e)
{
    Line *l = cur_line(e);

    if (e->cx < l->len) {
        e->cx = grapheme_next(l->s, l->len, e->cx);
    } else if (fold_next(e->buf, e->cy) < e->buf->nlines) {
        e->cy = fold_next(e->buf, e->cy);
        e->cx = 0;
    }
}

static void move_vertical(Editor *e, long delta)
{
    Line *l;

    if (delta > 0)
        e->cy = fold_advance(e->buf, e->cy, (size_t)delta);
    else if (delta < 0)
        e->cy = fold_back(e->buf, e->cy, (size_t)-delta);
    l = cur_line(e);
    e->cx = utf8_byte_at_col(l->s, l->len, e->want_col, e->tabw);
}

static void move_page(Editor *e, int down)
{
    size_t th = text_rows(e), page = th > 1 ? th - 1 : 1;
    size_t maxoff = fold_back(e->buf, fold_head(e->buf, e->buf->nlines - 1), th - 1);

    if (down) {
        e->rowoff = fold_advance(e->buf, e->rowoff, page);
        if (e->rowoff > maxoff)
            e->rowoff = maxoff;
        move_vertical(e, (long)page);
    } else {
        e->rowoff = fold_back(e->buf, e->rowoff, page);
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
        if (fold_next(e->buf, e->cy) < e->buf->nlines) {
            e->cy = fold_next(e->buf, e->cy);
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
            e->cy = fold_prev(e->buf, e->cy);
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

/* Asks what to do about a file that changed on disk. Returns 1 if the
 * document was reloaded. */
static int on_disk_change(Editor *e, Doc *d)
{
    int st = doc_disk_changed(d);
    char err[256], q[300];
    int c;

    if (st == DISK_SAME)
        return 0;
    if (st == DISK_MISSING) {
        d->disk_known = 0;
        editor_set_msg(e, MSG_TTL_MS, "«%s» non esiste più su disco", doc_base_name(d));
        return 0;
    }
    snprintf(q, sizeof q, "«%s» è cambiato su disco%s. Ricaricare? (r = ricarica, i = ignora)",
             doc_base_name(d), d->buf.modified ? " e hai modifiche non salvate" : "");
    c = ask(e, q, "ri");
    if (c == 'r') {
        if (doc_reload(d, err, sizeof err) < 0) {
            editor_set_msg(e, MSG_TTL_MS, "Ricarica fallita: %s", err);
            return 0;
        }
        view_clamp(&d->buf, &e->layout.active->view);
        load_win(e, e->layout.active);
        editor_set_msg(e, MSG_TTL_MS, "Ricaricato da disco");
        return 1;
    }
    doc_accept_disk(d);
    return 0;
}

static int save_doc_as(Editor *e, Doc *d, int force_prompt)
{
    char name[sizeof e->prompt_text], err[256];
    const char *target = NULL;
    long n;

    if (!d->buf.path || force_prompt) {
        if (prompt(e, "Salva con nome: ", d->buf.path, name, sizeof name) == 0) {
            editor_set_msg(e, MSG_TTL_MS, "Salvataggio annullato");
            return -1;
        }
        target = name;
        if (d->buf.path && strcmp(name, d->buf.path) == 0)
            target = NULL;
        else if (access(name, F_OK) == 0) {
            int c = ask(e, "Il file esiste già. Sovrascrivere? (s = sì, n = no)", "sn");

            if (c != 's') {
                editor_set_msg(e, MSG_TTL_MS, "Salvataggio annullato");
                return -1;
            }
        }
    } else if (doc_disk_changed(d) == DISK_CHANGED) {
        int c = ask(e, "Il file è cambiato su disco. Sovrascrivere? (s = sì, n = no)", "sn");

        if (c != 's') {
            editor_set_msg(e, MSG_TTL_MS, "Salvataggio annullato");
            return -1;
        }
    }
    if (d->trim_space)
        edit_trim_trailing(e, d);
    if (d->final_nl)
        d->buf.final_newline = 1;
    n = doc_save(d, target, err, sizeof err);
    if (n < 0) {
        editor_set_msg(e, MSG_TTL_MS, "Errore di salvataggio: %s", err);
        return -1;
    }
    apply_cfg(e, d);
    git_refresh_doc(e, d);
    editor_set_msg(e, MSG_TTL_MS, "%zu righe, %ld byte scritti", d->buf.nlines, n);
    return 0;
}

static int cmd_save(Editor *e)
{
    buf_undo_close(e->buf);
    return save_doc_as(e, e->doc, 0);
}

static void cmd_save_as(Editor *e)
{
    buf_undo_close(e->buf);
    save_doc_as(e, e->doc, 1);
}

static void cmd_save_all(Editor *e)
{
    size_t i, saved = 0;
    Doc *keep = e->doc;

    buf_undo_close(e->buf);
    for (i = 0; i < e->ndocs; i++) {
        Doc *d = e->docs[i];

        if (!d->buf.modified)
            continue;
        if (d->buf.path) {
            char err[256];

            if (doc_save(d, NULL, err, sizeof err) < 0) {
                editor_set_msg(e, MSG_TTL_MS, "Errore su «%s»: %s", doc_base_name(d), err);
                return;
            }
            saved++;
        } else {
            show_doc(e, d);
            if (save_doc_as(e, d, 0) < 0)
                return;
            saved++;
        }
    }
    if (e->doc != keep)
        show_doc(e, keep);
    editor_set_msg(e, MSG_TTL_MS, "%zu file salvati", saved);
}

static void cmd_open(Editor *e)
{
    char path[sizeof e->prompt_text], err[256];

    if (prompt(e, "Apri file: ", NULL, path, sizeof path) == 0)
        return;
    buf_undo_close(e->buf);
    if (!editor_open_doc(e, path, err, sizeof err))
        editor_set_msg(e, MSG_TTL_MS, "%s: %s", path, err);
}

static void switch_doc(Editor *e, Doc *d)
{
    if (d == e->doc)
        return;
    buf_undo_close(e->buf);
    show_doc(e, d);
    on_disk_change(e, d);
}

static void cycle_doc(Editor *e, int delta)
{
    size_t i = doc_index(e, e->doc);

    if (e->ndocs < 2)
        return;
    switch_doc(e, e->docs[(i + e->ndocs + (size_t)(delta < 0 ? e->ndocs - 1 : 1)) % e->ndocs]);
}

/* Drops d from the list; panes that showed it show a neighbour instead. */
static void remove_doc(Editor *e, Doc *d)
{
    size_t i = doc_index(e, d), n, k;
    Doc *repl;
    Win *w[64];

    if (e->ndocs == 1) {
        repl = doc_new();
        e->docs[0] = repl;
    } else {
        repl = e->docs[i > 0 ? i - 1 : 1];
        memmove(&e->docs[i], &e->docs[i + 1], (e->ndocs - i - 1) * sizeof *e->docs);
        e->ndocs--;
    }
    n = layout_wins(&e->layout, w, 64);
    for (k = 0; k < n && k < 64; k++)
        if (w[k]->doc == d) {
            w[k]->doc = repl;
            w[k]->view = repl->last;
        }
    run_doc_removed(e, d);
    doc_free(d);
    load_win(e, e->layout.active);
}

/* Offers to save d. Returns 0 if the user cancelled. */
static int confirm_discard(Editor *e, Doc *d, int *all)
{
    char q[300];
    int c;

    if (!d->buf.modified)
        return 1;
    if (e->doc != d)
        show_doc(e, d);
    if (*all)
        c = 's';
    else {
        snprintf(q, sizeof q, "«%s» ha modifiche non salvate. Salvare? (s = sì, n = no%s, Esc = annulla)",
                 doc_base_name(d), e->ndocs > 1 ? ", t = salva tutti" : "");
        c = ask(e, q, e->ndocs > 1 ? "snt" : "sn");
    }
    if (c == 0)
        return 0;
    if (c == 't') {
        *all = 1;
        c = 's';
    }
    if (c == 's' && save_doc_as(e, d, 0) < 0)
        return 0;
    return 1;
}

static void cmd_close_buffer(Editor *e)
{
    int all = 0;

    buf_undo_close(e->buf);
    if (!confirm_discard(e, e->doc, &all))
        return;
    remove_doc(e, e->doc);
}

static void cmd_quit(Editor *e)
{
    size_t i;
    int all = 0;

    buf_undo_close(e->buf);
    for (i = 0; i < e->ndocs; i++)
        if (!confirm_discard(e, e->docs[i], &all))
            return;
    e->quit = 1;
}

int editor_split_at(Editor *e, int vertical, int pct)
{
    relayout(e);
    editor_store_view(e);
    return layout_split_at(&e->layout, vertical, pct) ? 0 : -1;
}

int editor_split(Editor *e, int vertical)
{
    return editor_split_at(e, vertical, 50);
}

void editor_focus(Editor *e, Win *w)
{
    editor_store_view(e);
    buf_undo_close(e->buf);
    load_win(e, w);
}

void editor_reload_view(Editor *e)
{
    load_win(e, e->layout.active);
}

int editor_ask(Editor *e, const char *question, const char *keys)
{
    return ask(e, question, keys);
}

static void cmd_split(Editor *e, int vertical)
{
    if (editor_split(e, vertical) < 0)
        editor_set_msg(e, MSG_TTL_MS, "Spazio insufficiente per dividere");
}

static void cmd_close_win(Editor *e)
{
    if (layout_count(&e->layout) < 2) {
        editor_set_msg(e, MSG_TTL_MS, "È l'unico pannello");
        return;
    }
    editor_store_view(e);
    layout_close(&e->layout, e->layout.active);
    load_win(e, e->layout.active);
    relayout(e);
}

static void cmd_cycle_win(Editor *e, int delta)
{
    Win *old = e->layout.active;

    editor_store_view(e);
    buf_undo_close(e->buf);
    layout_cycle(&e->layout, delta);
    if (e->layout.active != old)
        load_win(e, e->layout.active);
}

static int is_printable(uint32_t code);

/* ---- picker, browser, session ----------------------------------------- */

static void add_file_item(const char *rel, void *ctx)
{
    picker_add(ctx, rel);
}

static char *under_root(const Editor *e, const char *rel)
{
    SBuf b = SBUF_INIT;

    if (e->root) {
        sbuf_adds(&b, e->root);
        sbuf_adds(&b, "/");
    }
    sbuf_adds(&b, rel);
    sbuf_add(&b, "", 1);
    return sbuf_steal(&b);
}

static void cmd_buffers(Editor *e)
{
    size_t i;

    picker_open(&e->picker, "Buffer");
    e->picker_kind = PK_BUFFERS;
    for (i = 0; i < e->ndocs; i++) {
        const Doc *d = e->docs[i];
        SBuf b = SBUF_INIT;

        sbuf_adds(&b, doc_base_name(d));
        if (d->buf.modified)
            sbuf_adds(&b, " *");
        if (d->buf.path) {
            sbuf_adds(&b, "   ");
            sbuf_adds(&b, d->buf.path);
        }
        sbuf_add(&b, "", 1);
        picker_add(&e->picker, b.p);
        sbuf_free(&b);
    }
    picker_filter(&e->picker);
    picker_select_item(&e->picker, doc_index(e, e->doc));
}

static void cmd_quick_open(Editor *e)
{
    picker_open(&e->picker, "File");
    e->picker_kind = PK_FILES;
    files_scan(e->root ? e->root : ".", add_file_item, &e->picker);
    picker_filter(&e->picker);
    if (e->picker.nitems == 0)
        editor_set_msg(e, MSG_TTL_MS, "Nessun file trovato");
}

static void picker_accept(Editor *e)
{
    long idx = picker_chosen(&e->picker);
    int kind = e->picker_kind;

    picker_close(&e->picker);
    if (idx < 0)
        return;
    if (kind == PK_BUFFERS) {
        if ((size_t)idx < e->ndocs)
            switch_doc(e, e->docs[idx]);
    } else if (kind == PK_RESULTS) {
        find_open_result(e, (size_t)idx);
    } else if (kind == PK_TAGS) {
        find_open_tag(e, (size_t)idx);
    } else if (kind == PK_COMMANDS) {
        command_run(e, (int)idx);
    } else if (kind == PK_FILES) {
        char err[256], *path = under_root(e, e->picker.items[idx]);

        buf_undo_close(e->buf);
        if (!editor_open_doc(e, path, err, sizeof err))
            editor_set_msg(e, MSG_TTL_MS, "%s: %s", path, err);
        free(path);
    }
}

static void handle_picker(Editor *e, Key k)
{
    Picker *p = &e->picker;

    if (k.code == KEY_ESC || k.code == CTRL('p') || k.code == CTRL('e') ||
        k.code == CTRL('q')) {
        picker_close(p);
    } else if (k.code == KEY_ENTER) {
        picker_accept(e);
    } else if (k.code == KEY_UP) {
        picker_move(p, -1);
    } else if (k.code == KEY_DOWN) {
        picker_move(p, 1);
    } else if (k.code == KEY_PGUP) {
        picker_move(p, -PICKER_ROWS);
    } else if (k.code == KEY_PGDN) {
        picker_move(p, PICKER_ROWS);
    } else if (k.code == KEY_BACKSPACE || k.code == 8) {
        picker_backspace(p);
    } else if (is_printable(k.code) && !(k.mods & (MOD_ALT | MOD_CTRL))) {
        picker_type(p, k.code);
    }
}

void editor_show_tree(Editor *e, const char *dir)
{
    free(e->root);
    e->root = xstrdup(dir);
    if (e->tree_ready)
        tree_free(&e->tree);
    tree_init(&e->tree, dir);
    e->tree_ready = 1;
    e->sidebar_on = 1;
    e->sidebar_focus = 1;
}

static void cmd_toggle_tree(Editor *e)
{
    if (!e->sidebar_on) {
        if (!e->tree_ready) {
            tree_init(&e->tree, e->root ? e->root : ".");
            e->tree_ready = 1;
        }
        e->sidebar_on = 1;
        e->sidebar_focus = 1;
    } else if (e->sidebar_focus) {
        e->sidebar_on = 0;
        e->sidebar_focus = 0;
    } else {
        e->sidebar_focus = 1;
    }
    relayout(e);
    render_invalidate(e->render);
}

/* Returns 0 for keys the browser leaves to the normal commands (Ctrl and Alt combinations). */
static int handle_tree(Editor *e, Key k)
{
    Tree *t = &e->tree;
    TreeEntry *en = t->sel < t->n ? &t->e[t->sel] : NULL;
    size_t page = e->rows > 4 ? (size_t)e->rows - 4 : 1;

    switch (k.code) {
    case KEY_ESC:
        e->sidebar_focus = 0;
        break;
    case CTRL('b'):
        cmd_toggle_tree(e);
        break;
    case KEY_UP:
        if (t->sel > 0)
            t->sel--;
        break;
    case KEY_DOWN:
        if (t->sel + 1 < t->n)
            t->sel++;
        break;
    case KEY_PGUP:
        t->sel = t->sel > page ? t->sel - page : 0;
        break;
    case KEY_PGDN:
        t->sel = t->sel + page < t->n ? t->sel + page : t->n - 1;
        break;
    case KEY_HOME:
        t->sel = 0;
        break;
    case KEY_END:
        t->sel = t->n - 1;
        break;
    case KEY_LEFT:
        tree_left(t);
        break;
    case KEY_RIGHT:
    case KEY_ENTER:
        if (!en)
            break;
        if (en->is_dir) {
            if (k.code == KEY_ENTER || !en->open)
                tree_toggle(t, t->sel);
        } else {
            char err[256], *path = xstrdup(en->path);

            buf_undo_close(e->buf);
            if (editor_open_doc(e, path, err, sizeof err))
                e->sidebar_focus = 0;
            else
                editor_set_msg(e, MSG_TTL_MS, "%s: %s", path, err);
            free(path);
        }
        break;
    default:
        if ((k.mods & (MOD_CTRL | MOD_ALT)) ||
            (k.code < 0x20 && k.code != KEY_ENTER && k.code != KEY_TAB))
            return 0;
        break;
    }
    return 1;
}

/* The panes as a prefix expression: "V50 <a> <b>" (side by side, a gets 50%),
 * "H30 <a> <b>" (stacked), or a leaf "W<doc>,<line>,<col>,<rowoff>,<coloff>". */
static void ser_layout(SBuf *b, const Node *n, const int *docmap, const Editor *e)
{
    if (n->win) {
        const View *v = &n->win->view;
        int idx = docmap[doc_index(e, n->win->doc)];

        sbuf_addf(b, "W%d,%zu,%zu,%zu,%zu ", idx < 0 ? 0 : idx, v->cy, v->cx, v->rowoff,
                  v->coloff);
        return;
    }
    sbuf_addf(b, "%c%d ", n->vertical ? 'V' : 'H', (n->frac + 5) / 10);
    ser_layout(b, n->a, docmap, e);
    ser_layout(b, n->b, docmap, e);
}

typedef struct PNode {
    int vertical, pct, leaf;
    long doc;
    size_t y, x, ro, co;
    struct PNode *a, *b;
} PNode;

static void pnode_free(PNode *n)
{
    if (!n)
        return;
    pnode_free(n->a);
    pnode_free(n->b);
    free(n);
}

/* Parses what ser_layout wrote; NULL if the text is malformed. */
static PNode *parse_layout(const char **p, int depth, int *count)
{
    PNode *n;
    int used = 0;

    while (**p == ' ')
        (*p)++;
    if (depth > 8 || ++*count > 32 || !**p)
        return NULL;
    n = xmalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    if (**p == 'W') {
        if (sscanf(*p + 1, "%ld,%zu,%zu,%zu,%zu%n", &n->doc, &n->y, &n->x, &n->ro, &n->co,
                   &used) < 5) {
            free(n);
            return NULL;
        }
        n->leaf = 1;
        *p += 1 + used;
        return n;
    }
    if ((**p != 'V' && **p != 'H') || sscanf(*p + 1, "%d%n", &n->pct, &used) < 1) {
        free(n);
        return NULL;
    }
    n->vertical = **p == 'V';
    *p += 1 + used;
    n->a = parse_layout(p, depth + 1, count);
    n->b = n->a ? parse_layout(p, depth + 1, count) : NULL;
    if (!n->b) {
        pnode_free(n);
        return NULL;
    }
    return n;
}

/* Rebuilds the panes below the active one. Returns -1 if they do not fit. */
static int build_layout(Editor *e, const PNode *n, Doc *const *opened, size_t nopened)
{
    if (n->leaf) {
        Win *w = e->layout.active;

        w->doc = n->doc >= 0 && (size_t)n->doc < nopened && opened[n->doc] ? opened[n->doc]
                                                                           : e->doc;
        memset(&w->view, 0, sizeof w->view);
        w->view.cy = n->y;
        w->view.cx = n->x;
        w->view.rowoff = n->ro;
        w->view.coloff = n->co;
        w->view.want_col = 0;
        return 0;
    }
    {
        Win *old = e->layout.active, *nw;

        relayout(e);
        nw = layout_split_at(&e->layout, n->vertical, n->pct);
        if (!nw)
            return -1;
        e->layout.active = old;
        if (build_layout(e, n->a, opened, nopened) < 0)
            return -1;
        e->layout.active = nw;
        return build_layout(e, n->b, opened, nopened);
    }
}

void editor_save_session(Editor *e)
{
    run_shutdown(e);
    Session s;
    char *file = session_file();
    size_t i;
    int *docmap;
    SBuf lay = SBUF_INIT;
    Win *w[64];
    size_t nw, k;

    if (!file)
        return;
    memset(&s, 0, sizeof s);
    editor_store_view(e);
    docmap = xmalloc(e->ndocs * sizeof *docmap);
    for (i = 0; i < e->ndocs; i++) {
        const Doc *d = e->docs[i];
        char *abs;

        docmap[i] = -1;
        if (!d->buf.path || d->buf.is_new)
            continue;
        abs = realpath(d->buf.path, NULL);
        if (!abs)
            continue;
        if (d == e->doc)
            s.active = s.n;
        docmap[i] = (int)s.n;
        session_add(&s, abs, d->last.cy, d->last.cx, d->last.rowoff);
        free(abs);
    }
    if (s.n > 0) {
        ser_layout(&lay, e->layout.root, docmap, e);
        sbuf_add(&lay, "", 1);
        s.layout = sbuf_steal(&lay);
        nw = layout_wins(&e->layout, w, 64);
        for (k = 0; k < nw && k < 64; k++)
            if (w[k] == e->layout.active)
                s.activewin = k;
        s.sidebar = e->sidebar_on;
    }
    session_save(file, &s);
    session_free(&s);
    free(docmap);
    free(file);
}

int editor_restore_session(Editor *e)
{
    Session s;
    char *file = session_file();
    size_t i, opened = 0;
    Doc *active = NULL, **docs_by_entry;
    char err[256];

    if (!file)
        return 0;
    if (session_load(file, &s) < 0) {
        free(file);
        return 0;
    }
    free(file);
    docs_by_entry = xmalloc((s.n ? s.n : 1) * sizeof *docs_by_entry);
    for (i = 0; i < s.n; i++) {
        Doc *d = editor_open_doc(e, s.e[i].path, err, sizeof err);

        docs_by_entry[i] = NULL;
        if (!d || d->buf.is_new) {
            if (d)
                remove_doc(e, d);
            continue;
        }
        docs_by_entry[i] = d;
        e->cy = s.e[i].y < e->buf->nlines ? s.e[i].y : e->buf->nlines - 1;
        e->cx = s.e[i].x < cur_line(e)->len ? s.e[i].x : cur_line(e)->len;
        e->rowoff = s.e[i].rowoff;
        e->want_col = editor_cursor_col(e);
        editor_store_view(e);
        opened++;
        if (i == s.active)
            active = d;
    }
    if (active)
        editor_activate(e, active);
    if (opened > 0 && s.layout) {
        const char *p = s.layout;
        int count = 0;
        PNode *tree = parse_layout(&p, 0, &count);

        if (tree && !tree->leaf) {
            Win *w[64];
            size_t nw;

            build_layout(e, tree, docs_by_entry, s.n);
            nw = layout_wins(&e->layout, w, 64);
            e->layout.active = w[s.activewin < nw && s.activewin < 64 ? s.activewin : 0];
            load_win(e, e->layout.active);
        }
        pnode_free(tree);
    }
    if (opened > 0 && s.sidebar) {
        if (!e->tree_ready) {
            tree_init(&e->tree, e->root ? e->root : ".");
            e->tree_ready = 1;
        }
        e->sidebar_on = 1;
    }
    free(docs_by_entry);
    session_free(&s);
    return (int)opened;
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
            e->cy = e->buf->nlines - 1;
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
        if (ctrl)
            break;
        if (shift)
            edit_dedent(e);
        else
            edit_tab(e);
        break;
    case KEY_ESC:
        e->sel_on = 0;
        e->find.hl = 0;
        break;
    case KEY_PASTE: {
        size_t n;
        const char *s = term_paste_text(&n);

        edit_paste_text(e, s, n);
        break;
    }
    default:
        if (is_printable(k.code))
            edit_insert_cp(e, k.code);
        break;
    }
    return 0;
}

/* Does a key that is not a command change the text? Used to refuse it in a read-only document. */
static int modifies(Key k)
{
    if (k.code == KEY_PASTE)
        return 1;
    if (k.mods & (MOD_ALT | MOD_CTRL))
        return 0;
    if (k.code == KEY_ENTER || k.code == KEY_TAB || k.code == 8)
        return 1;
    return k.code == KEY_BACKSPACE || k.code == KEY_DEL || is_printable(k.code);
}

/* ---- multi-cursor keys -------------------------------------------------- */

typedef struct {
    Key k;
    int shift, ctrl;
} KeyCtx;

static void run_key(Editor *e, void *a, size_t idx)
{
    const KeyCtx *c = a;

    (void)idx;
    if (!handle_plain(e, c->k, c->shift, c->ctrl))
        e->want_col = editor_cursor_col(e);
}

static void cut_one(Editor *e, void *a, size_t idx)
{
    (void)a;
    (void)idx;
    if (edit_sel_active(e))
        edit_delete(e);
}

typedef struct {
    const char *text;
    size_t n;
    size_t nlines;
    int split;              /* one line per cursor */
} PasteCtx;

static void paste_one(Editor *e, void *a, size_t idx)
{
    const PasteCtx *p = a;
    const char *s = p->text;
    size_t n = p->n;

    if (p->split) {
        size_t line = 0, i = 0;

        while (line < idx) {
            while (i < p->n && s[i] != '\n')
                i++;
            i++;
            line++;
        }
        s += i;
        n = 0;
        while (i + n < p->n && s[n] != '\n')
            n++;
    }
    edit_paste_text(e, s, n);
}

static void multi_paste(Editor *e, const char *text, size_t n)
{
    PasteCtx p;
    size_t i, lines = 1;

    p.text = text;
    p.n = n;
    if (n > 0 && text[n - 1] == '\n')
        p.n = n - 1;            /* a whole-line copy ends with a newline of its own */
    for (i = 0; i < p.n; i++)
        if (text[i] == '\n')
            lines++;
    p.nlines = lines;
    p.split = lines == cur_count(e);
    cur_each(e, paste_one, &p);
    buf_undo_close(e->buf);
}

/* Handles a key while there are several cursors. Returns 1 if it was used;
 * 0 means the key goes on to the normal single-cursor commands (the extra
 * cursors are dropped first, except for the ones that add cursors). */
static int handle_multi(Editor *e, Key k)
{
    int alt = (k.mods & MOD_ALT) != 0, ctrl = (k.mods & MOD_CTRL) != 0;
    int shift = (k.mods & MOD_SHIFT) != 0;
    int cmd = keymap_find(&e->km, k);
    const char *id = cmd >= 0 ? commands[cmd].id : "";
    KeyCtx c;

    if (strncmp(id, "cursor_", 7) == 0)
        return 0;
    if (strcmp(id, "copy") == 0 || strcmp(id, "cut") == 0) {
        size_t n;
        char *t = cur_selected_text(e, &n);
        int cut = id[1] == 'u';

        if (t) {
            edit_store_clip(e, t, n, cut ? "Tagliato" : "Copiato");
            if (cut) {
                cur_each(e, cut_one, NULL);
                buf_undo_close(e->buf);
            }
            return 1;
        }
    }
    if (strcmp(id, "paste") == 0 || k.code == KEY_PASTE) {
        if (k.code == KEY_PASTE) {
            size_t n;
            const char *s = term_paste_text(&n);

            multi_paste(e, s, n);
            return 1;
        }
        {
            const ClipEntry *ce = clip_get(&e->clip, 0);

            if (ce) {
                multi_paste(e, ce->text, ce->n);
                return 1;
            }
        }
    }
    if (cmd < 0 && !alt && (!ctrl || k.code == KEY_LEFT || k.code == KEY_RIGHT ||
                            k.code == KEY_UP || k.code == KEY_DOWN || k.code == KEY_HOME ||
                            k.code == KEY_END)) {
        switch (k.code) {
        case KEY_ENTER: case KEY_BACKSPACE: case 8: case KEY_DEL: case KEY_TAB:
        case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
        case KEY_HOME: case KEY_END: case KEY_PGUP: case KEY_PGDN:
            break;
        default:
            if (!is_printable(k.code))
                goto collapse;
        }
        c.k = k;
        c.shift = shift;
        c.ctrl = ctrl;
        cur_each(e, run_key, &c);
        if (!is_typing(k))
            buf_undo_close(e->buf);
        return 1;
    }
collapse:
    cur_clear(e);
    return 0;
}

/* ---- suggestion popup --------------------------------------------------- */

static void complete_accept(Editor *e)
{
    const Cand *c = &e->comp.v[e->comp.sel];
    char *text = xstrdup(c->text);
    size_t from = e->comp.x, i, ns;
    int snippet = c->kind == CAND_SNIPPET;
    const SnipDef *sd = snip_lang(e->doc->syn ? syn_name(e->doc->syn) : NULL, &ns);

    complete_close(&e->comp);
    if (snippet)
        for (i = 0; i < ns; i++)
            if (strcmp(sd[i].name, text) == 0) {
                edit_expand_snippet(e, from, &sd[i]);
                free(text);
                return;
            }
    edit_replace_word(e, from, text, strlen(text));
    free(text);
}

/* Keys that belong to an open popup. Returns 1 if the key was used. */
static int handle_complete(Editor *e, Key k)
{
    if (k.mods & (MOD_ALT | MOD_CTRL | MOD_SHIFT))
        return 0;
    switch (k.code) {
    case KEY_UP:
        complete_move(&e->comp, -1);
        return 1;
    case KEY_DOWN:
        complete_move(&e->comp, 1);
        return 1;
    case KEY_ENTER:
        /* Enter only accepts a choice made with the arrows: typed or pasted
         * text with newlines must not be rewritten under the user's hands */
        if (!e->comp.picked) {
            complete_close(&e->comp);
            return 0;
        }
        /* fall through */
    case KEY_TAB:
        buf_undo_close(e->buf);
        complete_accept(e);
        return 1;
    case KEY_ESC:
        complete_close(&e->comp);
        return 1;
    default:
        return 0;
    }
}

/* After a key was handled: open or refresh the popup while a word is being
 * typed, close it for anything else. */
static void update_complete(Editor *e, Key k)
{
    int word = !(k.mods & (MOD_ALT | MOD_CTRL)) && is_printable(k.code) && cp_is_word(k.code);
    int bs = !(k.mods & (MOD_ALT | MOD_CTRL)) && (k.code == KEY_BACKSPACE || k.code == 8);

    /* keys already waiting (fast typing, a paste without bracketing) are not worth a scan each */
    if ((word || (bs && e->comp.on)) && !edit_sel_active(e) && !e->doc->readonly &&
        !e->snip.on && !cur_multi(e) && e->doc->autosuggest && !term_has_input()) {
        e->comp.min_prefix = (size_t)e->doc->suggest_min;
        complete_open(&e->comp, e->docs, e->ndocs, e->doc, e->cy, e->cx);
    }
    else
        complete_close(&e->comp);
}

static void resize_mode_off(Editor *e)
{
    e->resize_mode = 0;
    e->msg[0] = '\0';
    e->msg_until = 0;
}

/* Arrow keys move the borders of the active pane while the mode lasts. Returns 0 for a key that
 * ends the mode and must be handled as usual. */
static int handle_resize_key(Editor *e, Key k)
{
    int dir = -1;

    if (k.code == KEY_FOCUS_IN || k.code == KEY_FOCUS_OUT || k.code == KEY_MOUSE)
        return 0;
    switch (k.code) {
    case KEY_LEFT: dir = LAYOUT_LEFT; break;
    case KEY_RIGHT: dir = LAYOUT_RIGHT; break;
    case KEY_UP: dir = LAYOUT_UP; break;
    case KEY_DOWN: dir = LAYOUT_DOWN; break;
    default: break;
    }
    if (dir >= 0 && !(k.mods & (MOD_ALT | MOD_CTRL))) {
        if (layout_resize(&e->layout, e->layout.active, dir, (k.mods & MOD_SHIFT) ? 10 : 2) < 0)
            editor_set_msg(e, MSG_TTL_MS, "Nessuno split in quella direzione");
        return 1;
    }
    if (k.code == '=' && !k.mods) {
        layout_equalize(&e->layout);
        return 1;
    }
    resize_mode_off(e);
    return k.code == KEY_ESC || k.code == KEY_ENTER;
}

static void handle_key(Editor *e, Key k)
{
    int cmd, mod, vertical;
    int alt = (k.mods & MOD_ALT) != 0, shift = (k.mods & MOD_SHIFT) != 0;
    if (k.code == KEY_MOUSE) {
        MouseEv ev = term_mouse_event();

        mouse_event(e, &ev);
        return;
    }
    if (e->resize_mode && handle_resize_key(e, k))
        return;
    if (k.code == KEY_FOCUS_IN) {
        on_disk_change(e, e->doc);
        git_refresh_doc(e, e->doc);
        return;
    }
    if (k.code == KEY_FOCUS_OUT)
        return;
    if (e->find.mode != FIND_OFF) {
        find_handle_key(e, k);
        return;
    }
    if (e->picker.on) {
        handle_picker(e, k);
        return;
    }
    if (e->sidebar_on && e->sidebar_focus && handle_tree(e, k))
        return;
    if (e->doc == e->run.out && !(k.mods & (MOD_ALT | MOD_CTRL))) {
        if (k.code == KEY_ENTER && run_enter(e))
            return;
        if (k.code == KEY_ESC && e->run.on) {
            run_stop(e);
            return;
        }
    }
    cmd = keymap_find(&e->km, k);
    mod = cmd >= 0 ? commands[cmd].modifies : modifies(k);
    if (e->doc->readonly && mod) {
        editor_set_msg(e, MSG_TTL_MS, "File in sola lettura (Alt+S per salvare con un altro nome)");
        return;
    }
    /* editing a folded line opens it: the edit must not leave the cursor in a fold */
    if (e->buf->folded && mod)
        e->buf->lines[e->cy].fold = 0;

    size_t snip_y = 0, snip_nl = 0, snip_len = 0;

    if (cur_multi(e) && handle_multi(e, k)) {
        e->want_col = editor_cursor_col(e);
        complete_close(&e->comp);
        return;
    }
    if (e->comp.on && handle_complete(e, k)) {
        e->want_col = editor_cursor_col(e);
        return;
    }
    if (e->snip.on) {
        if (alt || (k.mods & MOD_CTRL)) {
            e->snip.on = 0;
        } else if (k.code == KEY_TAB) {
            buf_undo_close(e->buf);
            edit_snip_goto(e, shift ? (e->snip.cur > 0 ? e->snip.cur - 1 : 0) : e->snip.cur + 1);
            e->want_col = editor_cursor_col(e);
            return;
        } else if (k.code == KEY_ESC) {
            e->snip.on = 0;
        }
    }
    if (!is_typing(k))
        buf_undo_close(e->buf);
    if (cmd < 0)
        e->paste_valid = 0;

    if (e->snip.on) {
        snip_y = e->snip.stop[e->snip.cur].y;
        snip_nl = e->buf->nlines;
        snip_len = e->buf->lines[snip_y].len;
    }
    if (cmd >= 0) {
        command_run(e, cmd);
        vertical = 1;               /* command_run settled the wanted column */
    } else if (alt) {
        vertical = 0;               /* an Alt key with no command */
    } else {
        vertical = handle_plain(e, k, shift, (k.mods & MOD_CTRL) != 0);
    }
    if (e->snip.on)
        snip_adjust(&e->snip, snip_nl, e->buf->nlines, snip_len,
                    snip_y < e->buf->nlines ? e->buf->lines[snip_y].len : 0, e->cy, e->cx);

    if (!vertical)
        e->want_col = editor_cursor_col(e);
    update_complete(e, k);
}

/* ---- command entry points (the palette and, later, key maps use them) --- */

void editor_cmd_save(Editor *e) { cmd_save(e); }
void editor_cmd_save_as(Editor *e) { cmd_save_as(e); }
void editor_cmd_save_all(Editor *e) { cmd_save_all(e); }
void editor_cmd_open(Editor *e) { cmd_open(e); }
void editor_cmd_quick_open(Editor *e) { cmd_quick_open(e); }
void editor_cmd_buffers(Editor *e) { cmd_buffers(e); }
void editor_cmd_close_buffer(Editor *e) { cmd_close_buffer(e); }
void editor_cmd_quit(Editor *e) { cmd_quit(e); }
void editor_cmd_next_buffer(Editor *e) { cycle_doc(e, 1); }
void editor_cmd_prev_buffer(Editor *e) { cycle_doc(e, -1); }
void editor_cmd_split_v(Editor *e) { cmd_split(e, 1); }
void editor_cmd_split_h(Editor *e) { cmd_split(e, 0); }
void editor_cmd_close_win(Editor *e) { cmd_close_win(e); }
void editor_cmd_next_win(Editor *e) { cmd_cycle_win(e, 1); }
void editor_cmd_prev_win(Editor *e) { cmd_cycle_win(e, -1); }
void editor_cmd_toggle_tree(Editor *e) { cmd_toggle_tree(e); }

void editor_cmd_match(Editor *e)
{
    if (e->match_on) {
        e->sel_on = 0;
        e->cy = e->my;
        e->cx = e->mx;
    } else {
        editor_set_msg(e, MSG_TTL_MS, "Nessuna parentesi corrispondente");
    }
}

void editor_cmd_theme(Editor *e)
{
    e->theme_idx++;
    e->theme = theme_get(e->theme_idx);
    render_invalidate(e->render);
    editor_set_msg(e, MSG_TTL_MS, "Tema: %s", e->theme->name);
}

void editor_cmd_redraw(Editor *e)
{
    render_invalidate(e->render);
}

void editor_cmd_palette(Editor *e)
{
    size_t i;
    char item[160];

    picker_open(&e->picker, "Comando");
    e->picker_kind = PK_COMMANDS;
    for (i = 0; i < ncommands; i++) {
        char label[40];

        keymap_label(&e->km, (int)i, label, sizeof label);
        snprintf(item, sizeof item, "%-42s %s", commands[i].name, label);
        picker_add(&e->picker, item);
    }
    picker_filter(&e->picker);
}

void editor_relayout(Editor *e)
{
    relayout(e);
}

void editor_key(Editor *e, Key k)
{
    handle_key(e, k);
}

void editor_cmd_resize_mode(Editor *e)
{
    if (layout_count(&e->layout) < 2) {
        editor_set_msg(e, MSG_TTL_MS, "Nessun altro pannello da ridimensionare");
        return;
    }
    e->resize_mode = 1;
    editor_set_msg(e, 0,
                   "Ridimensiona: frecce spostano il bordo (Maiusc x10)  = pareggia  Esc esci");
}

void editor_cmd_equalize(Editor *e)
{
    layout_equalize(&e->layout);
}

void editor_cmd_toggle_mouse(Editor *e)
{
    e->mouse_on = !e->mouse_on;
    term_mouse(e->mouse_on);
    editor_set_msg(e, MSG_TTL_MS, e->mouse_on ? "Mouse attivo" : "Mouse disattivato");
}

void editor_run(Editor *e)
{
    resize(e);
    term_mouse(e->mouse_on);
    if (!e->msg[0])
        editor_set_msg(e, MSG_TTL_MS, "Ctrl+S salva  Ctrl+Q esci  Alt+X comandi");

    while (!e->quit) {
        Key k;

        if (next_key(e, &k) < 0)
            break;
        handle_key(e, k);
    }
}
