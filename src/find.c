#include "sys.h"
#include "find.h"
#include "i18n.h"
#include "edit.h"
#include "files.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STATS_CAP 99999

static Matcher *matcher(Editor *e)
{
    Find *f = &e->find;

    if (!f->m) {
        f->m = xmalloc(sizeof *f->m);
        memset(f->m, 0, sizeof *f->m);
    }
    return f->m;
}

static void set_sel(Editor *e, const Hit *h)
{
    e->sel_on = 1;
    e->ay = h->y;
    e->ax = h->x;
    e->cy = h->y;
    e->cx = h->end;
    e->want_col = editor_cursor_col(e);
}

static void restore_origin(Editor *e)
{
    Find *f = &e->find;

    e->cy = f->oy < e->buf->nlines ? f->oy : e->buf->nlines - 1;
    e->cx = f->ox <= e->buf->lines[e->cy].len ? f->ox : e->buf->lines[e->cy].len;
    e->sel_on = f->osel && f->oay < e->buf->nlines;
    e->ay = f->oay;
    e->ax = f->oax;
    e->want_col = editor_cursor_col(e);
}

static void close_bar(Editor *e)
{
    e->find.mode = FIND_OFF;
    e->prompt_on = 0;
    e->prompt_info[0] = '\0';
}

/* "Aa W .*" for the options that are on. */
static void options_text(const Find *f, char *out, size_t n)
{
    snprintf(out, n, "%s%s%s", f->cs ? "Aa " : "", f->word ? "W " : "", f->regex ? ".* " : "");
}

static void set_info(Editor *e, const char *tail)
{
    char opts[16];

    options_text(&e->find, opts, sizeof opts);
    snprintf(e->prompt_info, sizeof e->prompt_info, "%s%s", opts, tail);
}

/* Compiles the text of the bar; false (with the reason in the info) if unusable. */
static int compile(Editor *e)
{
    Find *f = &e->find;
    Matcher *m = matcher(e);

    if (sm_compile(m, e->prompt_text, e->prompt_len, f->regex, f->cs, f->word) < 0) {
        set_info(e, m->err[0] ? m->err : "");
        f->hl = 0;
        return 0;
    }
    return 1;
}

/* After the text or an option changed: show the first match from the start point. */
static void refresh(Editor *e)
{
    Find *f = &e->find;
    Matcher *m;
    Hit h;
    int wr;
    size_t before, total;
    char tail[48];

    f->hit_valid = 0;
    if (!compile(e)) {
        if (f->mode != FIND_FILES)
            restore_origin(e);
        return;
    }
    m = f->m;
    f->hl = 1;
    if (f->mode == FIND_FILES) {
        set_info(e, "");
        return;
    }
    if (search_next(m, e->buf, f->oy, f->ox, 1, 1, f->has_span ? &f->span : NULL, &h, &wr)) {
        set_sel(e, &h);
        f->hit = h;
        f->hit_valid = 1;
        search_stats(m, e->buf, h.y, h.x, STATS_CAP, &before, &total);
        snprintf(tail, sizeof tail, "%zu/%zu", before + 1, total);
        set_info(e, tail);
    } else {
        restore_origin(e);
        set_info(e, tr("no results"));
    }
}

static size_t char_at(const Buffer *b, size_t y, size_t x)
{
    const Line *l = &b->lines[y];
    uint32_t cp;

    return x < l->len ? (size_t)utf8_decode(l->s + x, l->len - x, &cp) : 1;
}

/* Down / Up inside the bar. */
static void step(Editor *e, int forward)
{
    Find *f = &e->find;
    Hit h;
    int wr;
    size_t y = f->hit_valid ? f->hit.y : f->oy, x = f->hit_valid ? f->hit.x : f->ox;

    if (!f->m || !f->m->valid)
        return;
    if (forward && f->hit_valid)
        x += char_at(e->buf, y, x);
    if (search_next(f->m, e->buf, y, x, forward, 1, f->has_span ? &f->span : NULL, &h, &wr)) {
        size_t before, total;
        char tail[48];

        set_sel(e, &h);
        f->hit = h;
        f->hit_valid = 1;
        f->oy = h.y;
        f->ox = h.x;
        search_stats(f->m, e->buf, h.y, h.x, STATS_CAP, &before, &total);
        snprintf(tail, sizeof tail, "%zu/%zu%s", before + 1, total, wr ? tr(" (wrapped)") : "");
        set_info(e, tail);
    }
}

void find_open(Editor *e, int replace)
{
    Find *f = &e->find;
    size_t n;

    if (replace && e->doc->readonly) {
        editor_set_msg(e, MSG_TTL_MS, tr("File is read-only (Alt+S to save under another name)"));
        return;
    }
    buf_undo_close(e->buf);
    matcher(e);
    f->replace = replace;
    f->mode = FIND_SEARCH;
    f->has_span = 0;
    f->oy = e->cy;
    f->ox = e->cx;
    f->osel = e->sel_on;
    f->oay = e->ay;
    f->oax = e->ax;
    f->nrepl = 0;
    f->hit_valid = 0;

    snprintf(e->prompt_text, sizeof e->prompt_text, "%s", f->pat);
    if (edit_sel_active(e)) {
        size_t y0, x0, y1, x1;

        edit_sel_range(e, &y0, &x0, &y1, &x1);
        if (y0 == y1 && x1 - x0 < sizeof e->prompt_text - 1) {
            memcpy(e->prompt_text, e->buf->lines[y0].s + x0, x1 - x0);
            e->prompt_text[x1 - x0] = '\0';
            f->oy = y0;
            f->ox = x0;
        } else if (replace && y0 != y1) {
            f->has_span = 1;
            f->span.y0 = y0;
            f->span.x0 = x0;
            f->span.y1 = y1;
            f->span.x1 = x1;
        }
    }
    e->prompt_len = n = strlen(e->prompt_text);
    (void)n;
    e->prompt_on = 1;
    snprintf(e->prompt_label, sizeof e->prompt_label, "%s",
             replace ? tr("Replace - find: ") : tr("Find: "));
    if (e->prompt_len > 0)
        refresh(e);
    else
        set_info(e, "");
}

void find_files_open(Editor *e)
{
    Find *f = &e->find;

    buf_undo_close(e->buf);
    matcher(e);
    f->replace = 0;
    f->mode = FIND_FILES;
    f->has_span = 0;
    f->hit_valid = 0;
    snprintf(e->prompt_text, sizeof e->prompt_text, "%s", f->pat);
    e->prompt_len = strlen(e->prompt_text);
    e->prompt_on = 1;
    snprintf(e->prompt_label, sizeof e->prompt_label, tr("Find in files: "));
    if (e->prompt_len > 0)
        refresh(e);
    else
        set_info(e, "");
}

static void text_input(Editor *e, Key k)
{
    char b[4];
    size_t n = (size_t)utf8_encode(k.code, b);

    if (e->prompt_len + n < sizeof e->prompt_text) {
        memcpy(e->prompt_text + e->prompt_len, b, n);
        e->prompt_len += n;
        e->prompt_text[e->prompt_len] = '\0';
    }
}

static int printable(Key k)
{
    return k.code >= 0x20 && k.code < KEY_SPECIAL && k.code != 0x7F &&
           !(k.code >= 0x80 && k.code < 0xA0) && !(k.mods & (MOD_ALT | MOD_CTRL));
}

/* ---- replace ----------------------------------------------------------- */

static void confirm_prompt(Editor *e)
{
    char tail[48];

    snprintf(e->prompt_label, sizeof e->prompt_label,
             tr("Replace? (y = yes, n = no, a = all, Esc = done) "));
    e->prompt_len = 0;
    e->prompt_text[0] = '\0';
    snprintf(tail, sizeof tail, tr("%zu replaced"), e->find.nrepl);
    snprintf(e->prompt_info, sizeof e->prompt_info, "%s", tail);
}

static void finish_replace(Editor *e)
{
    Find *f = &e->find;

    if (f->undo_open) {
        buf_undo_cursor(e->buf, e->cy, e->cx);
        buf_undo_close(e->buf);
        f->undo_open = 0;
    }
    close_bar(e);
    e->sel_on = 0;
    f->hl = 0;
    if (f->nrepl > 0)
        editor_set_msg(e, MSG_TTL_MS, tr("%zu replacements"), f->nrepl);
    else
        editor_set_msg(e, MSG_TTL_MS, tr("No replacements"));
}

/* Moves to the next match at or after (y,x); finishes when there is none left. */
static void confirm_advance(Editor *e, size_t y, size_t x)
{
    Find *f = &e->find;
    Hit h;
    int wr;

    if (!search_next(f->m, e->buf, y, x, 1, !f->has_span, f->has_span ? &f->span : NULL, &h,
                     &wr)) {
        finish_replace(e);
        return;
    }
    if (wr)
        f->passed_end = 1;
    if (f->passed_end && (h.y > f->sy || (h.y == f->sy && h.x >= f->sx))) {
        finish_replace(e);
        return;
    }
    f->hit = h;
    set_sel(e, &h);
    confirm_prompt(e);
}

static void replace_current(Editor *e)
{
    Find *f = &e->find;
    size_t oldlen = f->hit.end - f->hit.x, newend, newlen;
    long delta;

    if (!f->undo_open) {
        buf_undo_begin(e->buf, f->hit.y, f->hit.x);
        f->undo_open = 1;
    }
    newend = search_replace_at(e->buf, f->m, &f->hit, f->repl);
    newlen = newend - f->hit.x;
    delta = (long)newlen - (long)oldlen;
    f->nrepl++;
    if (f->has_span && f->hit.y == f->span.y1)
        f->span.x1 = (size_t)((long)f->span.x1 + delta);
    if (f->hit.y == f->sy && f->hit.x < f->sx)
        f->sx = (size_t)((long)f->sx + delta);
    e->cy = f->hit.y;
    e->cx = newend;
    e->sel_on = 0;
    buf_undo_cursor(e->buf, e->cy, e->cx);
}

static void start_replace(Editor *e)
{
    Find *f = &e->find;

    f->nrepl = 0;
    f->passed_end = 0;
    f->undo_open = 0;
    if (f->has_span) {
        f->sy = f->span.y0;
        f->sx = f->span.x0;
    } else {
        f->sy = f->oy;
        f->sx = f->ox;
    }
    f->mode = FIND_CONFIRM;
    e->sel_on = 0;
    confirm_advance(e, f->sy, f->sx);
}

static void handle_confirm(Editor *e, Key k)
{
    Find *f = &e->find;
    uint32_t c = k.code;

    if (c >= 'A' && c <= 'Z')
        c += 32;
    if (c == 's' || c == 'y' || k.code == KEY_ENTER) {
        replace_current(e);
        confirm_advance(e, f->hit.y, e->cx);
    } else if (c == 'n') {
        confirm_advance(e, f->hit.y, f->hit.end);
    } else if (c == 't' || c == 'a') {
        while (f->mode == FIND_CONFIRM) {
            replace_current(e);
            confirm_advance(e, f->hit.y, e->cx);
        }
    } else if (k.code == KEY_ESC || c == 'q') {
        finish_replace(e);
    }
}

/* ---- search in files --------------------------------------------------- */

typedef struct {
    unsigned long dev[64], ino[64];
    size_t n;
} Seen;

static int seen_skip(unsigned long dev, unsigned long ino, void *ctx)
{
    const Seen *s = ctx;
    size_t i;

    for (i = 0; i < s->n; i++)
        if (s->dev[i] == dev && s->ino[i] == ino)
            return 1;
    return 0;
}

static void run_files(Editor *e)
{
    Find *f = &e->find;
    Seen seen;
    size_t i;
    SBuf item = SBUF_INIT;

    memset(&seen, 0, sizeof seen);
    filehits_free(&f->files);
    for (i = 0; i < e->ndocs; i++) {
        const Doc *d = e->docs[i];
        struct stat st;

        if (!d->buf.path)
            continue;
        search_buffer(f->m, d->buf.path, &d->buf, &f->files, FIND_MAX_RESULTS);
        if (seen.n < 64 && stat(d->buf.path, &st) == 0) {
            seen.dev[seen.n] = (unsigned long)st.st_dev;
            seen.ino[seen.n] = (unsigned long)st.st_ino;
            seen.n++;
        }
    }
    search_dir(f->m, e->root ? e->root : ".", seen_skip, &seen, &f->files, FIND_MAX_RESULTS);
    if (f->files.n == 0) {
        editor_set_msg(e, MSG_TTL_MS, tr("No results for «%s»"), f->pat);
        return;
    }
    picker_open(&e->picker, tr("Results"));
    e->picker_kind = PK_RESULTS;
    for (i = 0; i < f->files.n; i++) {
        const FileHit *h = &f->files.v[i];

        item.len = 0;
        sbuf_addf(&item, "%s:%zu: %s", h->path, h->line + 1, h->text);
        sbuf_add(&item, "", 1);
        picker_add(&e->picker, item.p);
    }
    sbuf_free(&item);
    picker_filter(&e->picker);
    if (f->files.truncated)
        editor_set_msg(e, MSG_TTL_MS, tr("Results truncated to the first %d"), FIND_MAX_RESULTS);
}

void find_open_result(Editor *e, size_t idx)
{
    Find *f = &e->find;
    const FileHit *h;
    char err[256];
    Hit hit;
    size_t ms, me;

    if (idx >= f->files.n)
        return;
    h = &f->files.v[idx];
    buf_undo_close(e->buf);
    if (!editor_open_doc(e, h->path, err, sizeof err)) {
        editor_set_msg(e, MSG_TTL_MS, "%s: %s", h->path, err);
        return;
    }
    e->cy = h->line < e->buf->nlines ? h->line : e->buf->nlines - 1;
    e->cx = h->col <= e->buf->lines[e->cy].len ? h->col : e->buf->lines[e->cy].len;
    e->sel_on = 0;
    if (f->m && f->m->valid &&
        sm_find(f->m, e->buf->lines[e->cy].s, e->buf->lines[e->cy].len, e->cx, &ms, &me, NULL)) {
        hit.y = e->cy;
        hit.x = ms;
        hit.end = me;
        set_sel(e, &hit);
        f->hl = 1;
    }
    e->want_col = editor_cursor_col(e);
}

/* ---- keys -------------------------------------------------------------- */

void find_handle_key(Editor *e, Key k)
{
    Find *f = &e->find;

    if (f->mode == FIND_CONFIRM) {
        handle_confirm(e, k);
        return;
    }
    if (k.code == KEY_ESC) {
        if (f->mode == FIND_SEARCH)
            restore_origin(e);
        f->hl = f->mode == FIND_WITH ? 0 : f->hl && f->mode == FIND_FILES;
        close_bar(e);
        return;
    }
    if (k.mods & MOD_ALT) {
        if (f->mode == FIND_WITH)
            return;
        if (k.code == 'c')
            f->cs = !f->cs;
        else if (k.code == 'w')
            f->word = !f->word;
#ifndef XTED_NO_REGEX
        else if (k.code == 'r')
            f->regex = !f->regex;
#endif
        else
            return;
        refresh(e);
        return;
    }
    if (k.code == KEY_BACKSPACE || k.code == 8) {
        if (e->prompt_len) {
            e->prompt_len = utf8_prev(e->prompt_text, e->prompt_len);
            e->prompt_text[e->prompt_len] = '\0';
        }
    } else if (printable(k)) {
        text_input(e, k);
    } else if (k.code == KEY_DOWN || k.code == KEY_UP) {
        if (f->mode == FIND_SEARCH)
            step(e, k.code == KEY_DOWN);
        return;
    } else if (k.code == KEY_ENTER) {
        if (f->mode == FIND_WITH) {
            snprintf(f->repl, sizeof f->repl, "%s", e->prompt_text);
            e->prompt_info[0] = '\0';
            start_replace(e);
            return;
        }
        if (!f->m || !f->m->valid) {
            if (e->prompt_len == 0)
                close_bar(e);
            return;
        }
        snprintf(f->pat, sizeof f->pat, "%s", e->prompt_text);
        if (f->mode == FIND_FILES) {
            close_bar(e);
            run_files(e);
        } else if (f->replace) {
            f->mode = FIND_WITH;
            snprintf(e->prompt_label, sizeof e->prompt_label, tr("Replace with: "));
            snprintf(e->prompt_text, sizeof e->prompt_text, "%s", f->repl);
            e->prompt_len = strlen(e->prompt_text);
            e->prompt_info[0] = '\0';
        } else {
            if (!f->hit_valid) {
                restore_origin(e);
                editor_set_msg(e, MSG_TTL_MS, tr("No results for «%s»"), f->pat);
            }
            close_bar(e);
        }
        return;
    } else {
        return;
    }
    if (f->mode != FIND_WITH)
        refresh(e);
}

void find_next(Editor *e, int forward)
{
    Find *f = &e->find;
    Hit h;
    int wr;
    size_t y, x;

    if (!f->m || !f->m->valid) {
        editor_set_msg(e, MSG_TTL_MS, tr("No search yet: press Ctrl+F"));
        return;
    }
    if (edit_sel_active(e)) {
        size_t y0, x0, y1, x1;

        edit_sel_range(e, &y0, &x0, &y1, &x1);
        y = forward ? y1 : y0;
        x = forward ? x1 : x0;
    } else {
        y = e->cy;
        x = e->cx;
    }
    if (!search_next(f->m, e->buf, y, x, forward, 1, NULL, &h, &wr)) {
        editor_set_msg(e, MSG_TTL_MS, tr("No results for «%s»"), f->pat);
        return;
    }
    set_sel(e, &h);
    f->hl = 1;
    if (wr)
        editor_set_msg(e, MSG_TTL_MS, forward ? tr("Search wrapped to the start")
                                              : tr("Search wrapped to the end"));
}

void find_goto(Editor *e)
{
    char text[64];
    size_t line, col;

    if (editor_prompt(e, tr("Go to line[:column]: "), NULL, text, sizeof text) == 0)
        return;
    if (search_parse_goto(text, &line, &col) < 0) {
        editor_set_msg(e, MSG_TTL_MS, tr("Invalid format: use 12, 12:5 or :5"));
        return;
    }
    buf_undo_close(e->buf);
    if (line > e->buf->nlines) {
        line = e->buf->nlines;
        editor_set_msg(e, MSG_TTL_MS, tr("Past the end: line %zu"), line);
    }
    editor_goto(e, line ? line : e->cy + 1, col ? col : 1);
}

void find_free(Editor *e)
{
    if (e->find.m) {
        sm_free(e->find.m);
        free(e->find.m);
        e->find.m = NULL;
    }
    filehits_free(&e->find.files);
    taghits_free(&e->find.tags);
    free(e->find.tags_dir);
    e->find.tags_dir = NULL;
}

/* ---- definitions (tags) ------------------------------------------------ */

static int word_at(const Line *l, size_t i)
{
    uint32_t cp;

    utf8_decode(l->s + i, l->len - i, &cp);
    return cp_is_word(cp);
}

/* The identifier around the cursor; returns its length, 0 if there is none. */
static size_t word_under_cursor(const Editor *e, char *out, size_t outsz)
{
    const Line *l = &e->buf->lines[e->cy];
    size_t s = e->cx, t;

    if (!(s < l->len && word_at(l, s))) {
        if (s == 0 || !word_at(l, utf8_prev(l->s, s)))
            return 0;
        s = utf8_prev(l->s, s);                 /* just after a word */
    }
    t = s;
    while (s > 0 && word_at(l, utf8_prev(l->s, s)))
        s = utf8_prev(l->s, s);
    while (t < l->len && word_at(l, t))
        t = utf8_next(l->s, l->len, t);
    if (t - s == 0 || t - s >= outsz)
        return 0;
    memcpy(out, l->s + s, t - s);
    out[t - s] = '\0';
    return t - s;
}

static void goto_hit(Editor *e, const TagHit *h)
{
    char *full, err[256];
    size_t line;

    if (sys_is_abs(h->file))
        full = xstrdup(h->file);
    else
        full = files_join(e->find.tags_dir ? e->find.tags_dir : ".", h->file);
    buf_undo_close(e->buf);
    if (!editor_open_doc(e, full, err, sizeof err)) {
        editor_set_msg(e, MSG_TTL_MS, "%s: %s", full, err);
        free(full);
        return;
    }
    free(full);
    line = tag_resolve(h, e->find.tag_name, e->buf);
    if (line == 0) {
        editor_goto(e, 1, 1);
        editor_set_msg(e, MSG_TTL_MS, tr("«%s» is no longer where the tags file says"), e->find.tag_name);
    } else {
        editor_goto(e, line, 1);
    }
    e->sel_on = 0;
    e->want_col = editor_cursor_col(e);
}

void find_tag(Editor *e)
{
    Find *f = &e->find;
    char name[sizeof f->tag_name], *file, *slash, item[600];
    const char *from = e->doc->buf.path ? e->doc->buf.path : NULL;
    char *dir;
    long n;
    size_t i;

    if (word_under_cursor(e, name, sizeof name) == 0) {
        editor_set_msg(e, MSG_TTL_MS, tr("No word under the cursor"));
        return;
    }
    dir = from ? xstrdup(from) : xstrdup(e->root ? e->root : ".");
    if (from) {
        slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
        } else {
            free(dir);
            dir = xstrdup(".");
        }
        if (!dir[0]) {
            free(dir);
            dir = xstrdup("/");
        }
    }
    file = tags_find_file(dir);
    free(dir);
    if (!file) {
        editor_set_msg(e, MSG_TTL_MS, tr("No tags file (create it with ctags -R)"));
        return;
    }
    taghits_free(&f->tags);
    free(f->tags_dir);
    f->tags_dir = xstrdup(file);
    slash = strrchr(f->tags_dir, '/');
    if (slash)
        *slash = '\0';
    snprintf(f->tag_name, sizeof f->tag_name, "%s", name);
    n = tags_lookup(file, name, &f->tags);
    free(file);
    if (n < 0) {
        editor_set_msg(e, MSG_TTL_MS, tr("Cannot read the tags file"));
        return;
    }
    if (n == 0) {
        editor_set_msg(e, MSG_TTL_MS, tr("No definition of «%s»"), name);
        return;
    }
    if (n == 1) {
        goto_hit(e, &f->tags.v[0]);
        return;
    }
    picker_open(&e->picker, tr("Definition"));
    e->picker_kind = PK_TAGS;
    for (i = 0; i < f->tags.n; i++) {
        const TagHit *h = &f->tags.v[i];

        snprintf(item, sizeof item, "%s  %s  %s", h->file, h->kind[0] ? h->kind : "-", h->addr);
        picker_add(&e->picker, item);
    }
    picker_filter(&e->picker);
}

void find_open_tag(Editor *e, size_t idx)
{
    if (idx < e->find.tags.n)
        goto_hit(e, &e->find.tags.v[idx]);
}
