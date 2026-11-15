#include "edit.h"
#include "i18n.h"
#include "cursors.h"
#include "shell.h"
#include "utf8.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/wait.h>
#include <string.h>

static Line *cur_line(Editor *e)
{
    return &e->buf->lines[e->cy];
}

static int is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static int word_at(const Line *l, size_t i)
{
    uint32_t cp;

    utf8_decode(l->s + i, l->len - i, &cp);
    return cp_is_word(cp);
}

/* Undo grouping for a command: begin remembers the cursor to restore (the
 * start of the selection, if any), done records where the cursor ended up,
 * finish also closes the group. */
static void begin(Editor *e)
{
    size_t y = e->cy, x = e->cx, y1, x1;

    if (e->undo_hold)
        return;
    if (edit_sel_active(e))
        edit_sel_range(e, &y, &x, &y1, &x1);
    buf_undo_begin(e->buf, y, x);
}

static void done(Editor *e)
{
    if (e->undo_hold)
        return;
    buf_undo_cursor(e->buf, e->cy, e->cx);
}

static void finish(Editor *e)
{
    done(e);
    if (!e->undo_hold)
        buf_undo_close(e->buf);
}

/* One level of indentation of the document: a tab character, or indent_w spaces. */
static size_t indent_unit(const Editor *e, char *unit)
{
    size_t n, w = e->doc->indent_w > 0 && e->doc->indent_w <= 16 ? (size_t)e->doc->indent_w : 8;

    if (!e->doc->expand_tab) {
        unit[0] = '\t';
        return 1;
    }
    for (n = 0; n < w; n++)
        unit[n] = ' ';
    return n;
}

/* ---- selection --------------------------------------------------------- */

int edit_sel_active(const Editor *e)
{
    return e->sel_on && (e->ay != e->cy || e->ax != e->cx);
}

void edit_sel_range(const Editor *e, size_t *y0, size_t *x0, size_t *y1, size_t *x1)
{
    if (e->ay < e->cy || (e->ay == e->cy && e->ax <= e->cx)) {
        *y0 = e->ay;
        *x0 = e->ax;
        *y1 = e->cy;
        *x1 = e->cx;
    } else {
        *y0 = e->cy;
        *x0 = e->cx;
        *y1 = e->ay;
        *x1 = e->ax;
    }
}

int edit_view_sel_line(const Buffer *b, const View *v, size_t y, size_t *from, size_t *to,
                       int *nl)
{
    size_t y0, x0, y1, x1;

    if (!v->sel_on || (v->ay == v->cy && v->ax == v->cx))
        return 0;
    if (v->ay < v->cy || (v->ay == v->cy && v->ax <= v->cx)) {
        y0 = v->ay; x0 = v->ax; y1 = v->cy; x1 = v->cx;
    } else {
        y0 = v->cy; x0 = v->cx; y1 = v->ay; x1 = v->ax;
    }
    if (y < y0 || y > y1)
        return 0;
    *from = y == y0 ? x0 : 0;
    *to = y == y1 ? x1 : b->lines[y].len;
    *nl = y < y1;
    return 1;
}

int edit_sel_line(const Editor *e, size_t y, size_t *from, size_t *to, int *nl)
{
    View v;

    v.cy = e->cy;
    v.cx = e->cx;
    v.sel_on = e->sel_on;
    v.ay = e->ay;
    v.ax = e->ax;
    return edit_view_sel_line(e->buf, &v, y, from, to, nl);
}

void edit_sel_collapse(Editor *e, int to_end)
{
    size_t y0, x0, y1, x1;

    if (edit_sel_active(e)) {
        edit_sel_range(e, &y0, &x0, &y1, &x1);
        e->cy = to_end ? y1 : y0;
        e->cx = to_end ? x1 : x0;
    }
    e->sel_on = 0;
}

void edit_select_all(Editor *e)
{
    e->sel_on = 1;
    e->ay = 0;
    e->ax = 0;
    e->cy = e->buf->nlines - 1;
    e->cx = cur_line(e)->len;
}

void edit_select_line(Editor *e)
{
    e->sel_on = 1;
    e->ay = e->cy;
    e->ax = 0;
    if (e->cy + 1 < e->buf->nlines) {
        e->cy++;
        e->cx = 0;
    } else {
        e->cx = cur_line(e)->len;
    }
}

void edit_select_word(Editor *e)
{
    const Line *l = cur_line(e);
    size_t start = e->cx, end = e->cx;

    if (e->cx < l->len && word_at(l, e->cx)) {
        /* on a word */
    } else if (e->cx > 0 && word_at(l, utf8_prev(l->s, e->cx))) {
        /* just after one */
    } else {
        return;
    }
    while (start > 0 && word_at(l, utf8_prev(l->s, start)))
        start = utf8_prev(l->s, start);
    while (end < l->len && word_at(l, end))
        end = utf8_next(l->s, l->len, end);
    e->sel_on = 1;
    e->ay = e->cy;
    e->ax = start;
    e->cx = end;
}

/* Deletes the selection, if any; returns whether there was one. */
static int delete_selection(Editor *e)
{
    size_t y0, x0, y1, x1;

    if (!edit_sel_active(e)) {
        e->sel_on = 0;
        return 0;
    }
    edit_sel_range(e, &y0, &x0, &y1, &x1);
    buf_delete_range(e->buf, y0, x0, y1, x1);
    e->cy = y0;
    e->cx = x0;
    e->sel_on = 0;
    return 1;
}

/* First and last line touched by the selection. A selection that ends at
 * column 0 of a later line does not include that line. */
static void sel_lines(const Editor *e, size_t *a, size_t *b)
{
    size_t y0, x0, y1, x1;

    if (!edit_sel_active(e)) {
        *a = *b = e->cy;
        return;
    }
    edit_sel_range(e, &y0, &x0, &y1, &x1);
    *a = y0;
    *b = (y1 > y0 && x1 == 0) ? y1 - 1 : y1;
}

/* ---- typing ------------------------------------------------------------ */

void edit_insert_cp(Editor *e, uint32_t cp)
{
    char b[4];
    int n = utf8_encode(cp, b);

    begin(e);
    delete_selection(e);
    buf_insert(e->buf, e->cy, e->cx, b, (size_t)n);
    e->cx += (size_t)n;
    done(e);
}

/* ---- automatic pairs ---------------------------------------------------- */

/* The closing character for an opening one that this document pairs, 0 if none. Quotes are for
 * code, not prose; a Rust quote is also a lifetime; the backtick is a string or code marker
 * only in some languages. */
static char pair_closer(const Editor *e, uint32_t cp)
{
    const char *lang = e->doc->syn ? syn_name(e->doc->syn) : NULL;

    if (!e->doc->autopair || cp > 126)
        return 0;
    switch (cp) {
    case '(':
        return ')';
    case '[':
        return ']';
    case '{':
        return '}';
    case '"':
        return lang && strcmp(lang, "Markdown") != 0 && strcmp(lang, "Diff") != 0 ? '"' : 0;
    case '\'':
        return lang && strcmp(lang, "Markdown") != 0 && strcmp(lang, "Diff") != 0 &&
                       strcmp(lang, "Rust") != 0 ? '\'' : 0;
    case '`':
        return lang && (strcmp(lang, "JavaScript") == 0 || strcmp(lang, "TypeScript") == 0 ||
                        strcmp(lang, "Markdown") == 0 || strcmp(lang, "Shell") == 0) ? '`' : 0;
    default:
        return 0;
    }
}

/* Pairs are not made with several cursors, in a snippet or in a read-only document. */
static int pairing_allowed(const Editor *e)
{
    return !cur_multi(e) && !e->snip.on && !e->doc->readonly;
}

/* Is the cursor inside a string or a comment (a quote typed there is a character)? */
static int in_string_or_comment(Editor *e)
{
    Syntax *s = e->doc->syn;
    const uint8_t *cls;
    const Line *l = cur_line(e);

    if (!s || e->cx == 0 || l->len == 0)
        return 0;
    syn_update(s, e->buf, e->cy);
    cls = syn_classes(s, e->buf, e->cy);
    if (!cls)
        return 0;
    if (cls[e->cx - 1] == SC_COMMENT)
        return 1;
    if (cls[e->cx - 1] != SC_STRING)
        return 0;
    if (e->cx < l->len)
        return cls[e->cx] == SC_STRING;
    {
        /* at the end of the line: after the closing quote of a string, or still in it */
        size_t r = e->cx - 1;
        char q = l->s[e->cx - 1];

        while (r > 0 && cls[r - 1] == SC_STRING)
            r--;
        return !((q == '"' || q == '\'' || q == '`') && r < e->cx - 1 && l->s[r] == q);
    }
}

static int word_byte(unsigned char c)
{
    return isalnum(c) || c == '_' || c >= 0x80;
}

int edit_type(Editor *e, uint32_t cp)
{
    const Line *l;
    char close = pair_closer(e, cp), open = (char)cp;
    unsigned char next, prev;
    size_t y0, x0, y1, x1;

    if (!pairing_allowed(e) || cp > 126)
        return 0;
    l = cur_line(e);
    next = e->cx < l->len ? (unsigned char)l->s[e->cx] : 0;
    prev = e->cx > 0 ? (unsigned char)l->s[e->cx - 1] : 0;

    /* the closing character that is already there is stepped over */
    if (!edit_sel_active(e) && next == cp && e->doc->autopair &&
        (cp == ')' || cp == ']' || cp == '}' || (close && close == open))) {
        e->cx++;
        return 1;
    }
    if (!close)
        return 0;
    if (edit_sel_active(e)) {
        /* the selection is wrapped and stays selected */
        begin(e);
        edit_sel_range(e, &y0, &x0, &y1, &x1);
        buf_insert(e->buf, y1, x1, &close, 1);
        buf_insert(e->buf, y0, x0, &open, 1);
        e->ay = y0;
        e->ax = x0 + 1;
        e->cy = y1;
        e->cx = y0 == y1 ? x1 + 1 : x1;
        e->sel_on = 1;
        done(e);
        return 1;
    }
    if (next && !isspace(next) && next != ')' && next != ']' && next != '}')
        return 0;
    if (close == open && (prev == (unsigned char)open || (open == '\'' && word_byte(prev))))
        return 0;
    if (in_string_or_comment(e))
        return 0;
    begin(e);
    {
        char both[2];

        both[0] = open;
        both[1] = close;
        buf_insert(e->buf, e->cy, e->cx, both, 2);
    }
    e->cx++;
    done(e);
    return 1;
}

/* Backspace between the two characters of an empty pair removes both. */
int edit_backspace_pair(Editor *e)
{
    const Line *l;
    char close;

    if (!pairing_allowed(e) || edit_sel_active(e) || e->cx == 0)
        return 0;
    l = cur_line(e);
    if (e->cx >= l->len)
        return 0;
    close = pair_closer(e, (unsigned char)l->s[e->cx - 1]);
    if (!close || l->s[e->cx] != close)
        return 0;
    begin(e);
    buf_delete(e->buf, e->cy, e->cx - 1, 2);
    e->cx--;
    done(e);
    return 1;
}

void edit_newline(Editor *e)
{
    size_t n = 0;
    int between;
    const Line *l;

    begin(e);
    delete_selection(e);
    l = cur_line(e);
    while (n < l->len && n < e->cx && is_blank(l->s[n]))
        n++;
    between = pairing_allowed(e) && e->cx > 0 && e->cx < l->len && e->doc->autopair &&
              ((l->s[e->cx - 1] == '{' && l->s[e->cx] == '}') ||
               (l->s[e->cx - 1] == '(' && l->s[e->cx] == ')') ||
               (l->s[e->cx - 1] == '[' && l->s[e->cx] == ']'));
    buf_split(e->buf, e->cy, e->cx);
    e->cy++;
    e->cx = 0;
    if (n > 0) {
        /* the head of the old line keeps the indentation we copy from */
        const Line *prev = &e->buf->lines[e->cy - 1];

        buf_insert(e->buf, e->cy, 0, prev->s, n);
        e->cx = n;
    }
    if (between) {
        /* {|} becomes the brace on a line of its own and the cursor on an indented one */
        char unit[17];
        size_t ul = indent_unit(e, unit);

        buf_split(e->buf, e->cy, e->cx);
        if (n > 0)
            buf_insert(e->buf, e->cy + 1, 0, e->buf->lines[e->cy - 1].s, n);
        buf_insert(e->buf, e->cy, e->cx, unit, ul);
        e->cx += ul;
    }
    finish(e);
}

void edit_backspace(Editor *e)
{
    begin(e);
    if (!delete_selection(e)) {
        if (e->cx > 0) {
            const Line *l = cur_line(e);
            size_t from = grapheme_prev(l->s, l->len, e->cx);

            buf_delete(e->buf, e->cy, from, e->cx - from);
            e->cx = from;
        } else if (e->cy > 0) {
            e->cy--;
            e->cx = cur_line(e)->len;
            buf_join(e->buf, e->cy);
        }
    }
    done(e);
}

void edit_delete(Editor *e)
{
    begin(e);
    if (!delete_selection(e)) {
        const Line *l = cur_line(e);

        if (e->cx < l->len) {
            size_t to = grapheme_next(l->s, l->len, e->cx);

            buf_delete(e->buf, e->cy, e->cx, to - e->cx);
        } else if (e->cy + 1 < e->buf->nlines) {
            buf_join(e->buf, e->cy);
        }
    }
    done(e);
}

void edit_tab(Editor *e)
{
    size_t a, b, k;

    if (!edit_sel_active(e)) {
        if (e->doc->expand_tab) {
            /* spaces up to the next indentation stop */
            size_t iw = (size_t)(e->doc->indent_w > 0 ? e->doc->indent_w : 8);
            size_t col = editor_cursor_col(e), n = iw - col % iw;

            begin(e);
            e->sel_on = 0;
            while (n-- > 0) {
                buf_insert(e->buf, e->cy, e->cx, " ", 1);
                e->cx++;
            }
            done(e);
            return;
        }
        edit_insert_cp(e, '\t');
        return;
    }
    sel_lines(e, &a, &b);
    begin(e);
    {
        char unit[17];
        size_t ulen = indent_unit(e, unit);

        for (k = a; k <= b; k++)
            buf_insert(e->buf, k, 0, unit, ulen);
        /* keep the selection on the same text; an end at column 0 stays there */
        if (e->ay >= a && e->ay <= b && e->ax > 0)
            e->ax += ulen;
        if (e->cy >= a && e->cy <= b && e->cx > 0)
            e->cx += ulen;
    }
    finish(e);
}

void edit_dedent(Editor *e)
{
    size_t a, b, k;

    sel_lines(e, &a, &b);
    begin(e);
    for (k = a; k <= b; k++) {
        const Line *l = &e->buf->lines[k];
        size_t r = 0;

        if (l->len > 0 && l->s[0] == '\t')
            r = 1;
        else
            while (r < l->len && r < (size_t)(e->doc->indent_w > 0 ? e->doc->indent_w : 8) &&
                   l->s[r] == ' ')
                r++;
        if (r == 0)
            continue;
        buf_delete(e->buf, k, 0, r);
        if (e->cy == k)
            e->cx = e->cx > r ? e->cx - r : 0;
        if (e->sel_on && e->ay == k)
            e->ax = e->ax > r ? e->ax - r : 0;
    }
    finish(e);
}

/* Rewrites the indentation of every line: tab characters (cols_per_tab columns each) or spaces,
 * keeping its width in columns, measured with the document's tab width. Returns the number of
 * lines changed. */
size_t edit_convert_indent(Editor *e, int to_tabs, size_t cols_per_tab)
{
    size_t k, changed = 0, tabw = e->tabw > 0 ? (size_t)e->tabw : 8;

    if (cols_per_tab == 0)
        cols_per_tab = tabw;
    begin(e);
    for (k = 0; k < e->buf->nlines; k++) {
        const Line *l = &e->buf->lines[k];
        size_t lead = 0, width = 0, nl, tabs = 0, spaces;
        char *repl;

        while (lead < l->len && (l->s[lead] == ' ' || l->s[lead] == '\t')) {
            width += l->s[lead] == '\t' ? tabw - width % tabw : 1;
            lead++;
        }
        if (lead == 0 || lead == l->len)
            continue;                       /* not indented, or only blanks */
        if (to_tabs) {
            tabs = width / cols_per_tab;
            spaces = width % cols_per_tab;
        } else {
            spaces = width;
        }
        nl = tabs + spaces;
        repl = xmalloc(nl + 1);
        memset(repl, '\t', tabs);
        memset(repl + tabs, ' ', spaces);
        if (nl == lead && memcmp(repl, l->s, lead) == 0) {
            free(repl);
            continue;
        }
        buf_delete(e->buf, k, 0, lead);
        buf_insert(e->buf, k, 0, repl, nl);
        free(repl);
        changed++;
        if (e->cy == k)
            e->cx = e->cx >= lead ? e->cx - lead + nl : (e->cx < nl ? e->cx : nl);
        if (e->sel_on && e->ay == k)
            e->ax = e->ax >= lead ? e->ax - lead + nl : (e->ax < nl ? e->ax : nl);
    }
    finish(e);
    return changed;
}

/* ---- clipboard --------------------------------------------------------- */

/* Takes ownership of text. */
static void store_clip(Editor *e, char *text, size_t n, int whole, const char *verb)
{
    int too_big = 0;

    if (e->clipboard_osc52) {
        SBuf osc = SBUF_INIT;

        if (clip_osc52(&osc, text, n) == 0)
            term_write(osc.p, osc.len);
        else
            too_big = 1;
        sbuf_free(&osc);
    }
    clip_push(&e->clip, text, n, whole);
    editor_set_msg(e, MSG_TTL_MS, tr("%s: %zu bytes%s"), verb, n,
                   too_big ? tr(" (too large for the system clipboard)") : "");
}

static void copy_to_clip(Editor *e, const char *verb)
{
    char *text;
    size_t n;

    if (edit_sel_active(e)) {
        size_t y0, x0, y1, x1;

        edit_sel_range(e, &y0, &x0, &y1, &x1);
        text = buf_get_range(e->buf, y0, x0, y1, x1, &n);
        store_clip(e, text, n, 0, verb);
    } else {
        const Line *l = cur_line(e);

        n = l->len + 1;
        text = xmalloc(n);
        if (l->len > 0)
            memcpy(text, l->s, l->len);
        text[l->len] = '\n';
        store_clip(e, text, n, 1, verb);
    }
}

/* Puts the cursor on line y, clamped, at the column the user was aiming for. */
static void settle_cursor(Editor *e, size_t y)
{
    const Line *l;

    if (y >= e->buf->nlines)
        y = e->buf->nlines - 1;
    e->cy = y;
    l = cur_line(e);
    e->cx = utf8_byte_at_col(l->s, l->len, e->want_col, e->tabw);
}

void edit_copy(Editor *e)
{
    copy_to_clip(e, tr("Copied"));
}

void edit_cut(Editor *e)
{
    int had_sel = edit_sel_active(e);

    copy_to_clip(e, tr("Cut"));
    begin(e);
    if (had_sel) {
        delete_selection(e);
    } else {
        buf_delete_lines(e->buf, e->cy, e->cy);
        e->sel_on = 0;
        settle_cursor(e, e->cy);
    }
    finish(e);
}

/* Inserts clip entry c at the cursor and remembers where it went. */
static void insert_entry(Editor *e, const ClipEntry *c, size_t idx)
{
    size_t y = e->cy, x = e->cx;

    e->paste_y0 = y;
    e->paste_x0 = x;
    buf_insert_text(e->buf, &y, &x, c->text, c->n);
    e->cy = e->paste_y1 = y;
    e->cx = e->paste_x1 = x;
    e->paste_idx = idx;
    e->paste_valid = 1;
}

void edit_paste(Editor *e)
{
    const ClipEntry *c = clip_get(&e->clip, 0);
    int had_sel;

    if (!c) {
        editor_set_msg(e, MSG_TTL_MS, tr("Clipboard is empty"));
        return;
    }
    begin(e);
    had_sel = delete_selection(e);
    if (c->whole_line && !had_sel)
        e->cx = 0;
    insert_entry(e, c, 0);
    finish(e);
}

void edit_paste_older(Editor *e)
{
    size_t idx;
    const ClipEntry *c;

    if (!e->paste_valid) {
        editor_set_msg(e, MSG_TTL_MS, tr("Alt+V replaces the last paste"));
        return;
    }
    if (e->clip.count < 2) {
        editor_set_msg(e, MSG_TTL_MS, tr("No previous clipboard entry"));
        e->paste_valid = 1;
        return;
    }
    idx = e->paste_idx + 1 < e->clip.count ? e->paste_idx + 1 : 0;
    c = clip_get(&e->clip, idx);

    begin(e);
    buf_delete_range(e->buf, e->paste_y0, e->paste_x0, e->paste_y1, e->paste_x1);
    e->cy = e->paste_y0;
    e->cx = e->paste_x0;
    insert_entry(e, c, idx);
    finish(e);
}

void edit_paste_text(Editor *e, const char *s, size_t n)
{
    SBuf clean = SBUF_INIT;
    size_t i, y, x;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '\r') {
            sbuf_add(&clean, "\n", 1);
            if (i + 1 < n && s[i + 1] == '\n')
                i++;
        } else if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7F)) {
            sbuf_add(&clean, s + i, 1);
        }
    }
    if (clean.len == 0) {
        sbuf_free(&clean);
        return;
    }
    begin(e);
    delete_selection(e);
    y = e->cy;
    x = e->cx;
    buf_insert_text(e->buf, &y, &x, clean.p, clean.len);
    e->cy = y;
    e->cx = x;
    finish(e);
    sbuf_free(&clean);
}

/* ---- undo -------------------------------------------------------------- */

void edit_undo(Editor *e)
{
    size_t y, x;

    e->sel_on = 0;
    cur_clear(e);
    if (!buf_undo(e->buf, &y, &x)) {
        editor_set_msg(e, MSG_TTL_MS, tr("Nothing to undo"));
        return;
    }
    e->cy = y;
    e->cx = x;
}

void edit_redo(Editor *e)
{
    size_t y, x;

    e->sel_on = 0;
    cur_clear(e);
    if (!buf_redo(e->buf, &y, &x)) {
        editor_set_msg(e, MSG_TTL_MS, tr("Nothing to redo"));
        return;
    }
    e->cy = y;
    e->cx = x;
}

/* ---- lines ------------------------------------------------------------- */

void edit_move_lines(Editor *e, int down)
{
    size_t a, b, ny, n;
    char *text;
    const Line *l;

    sel_lines(e, &a, &b);
    if (down ? b + 1 >= e->buf->nlines : a == 0)
        return;

    /* the line the block jumps over moves to the other side of it */
    begin(e);
    ny = down ? b + 1 : a - 1;
    l = &e->buf->lines[ny];
    n = l->len;
    text = xmalloc(n + 1);
    if (n > 0)
        memcpy(text, l->s, n);
    buf_delete_lines(e->buf, ny, ny);
    if (down)
        buf_insert_lines(e->buf, a, 0, text, n);
    else
        buf_insert_lines(e->buf, b - 1, 1, text, n);
    free(text);

    if (down) {
        e->cy++;
        e->ay++;
    } else {
        e->cy--;
        e->ay--;
    }
    finish(e);
}

void edit_duplicate_lines(Editor *e, int down)
{
    size_t a, b, n;
    char *text;

    sel_lines(e, &a, &b);
    text = buf_get_range(e->buf, a, 0, b, e->buf->lines[b].len, &n);
    begin(e);
    if (down) {
        buf_insert_lines(e->buf, b, 1, text, n);
        e->cy += b - a + 1;
        e->ay += b - a + 1;
    } else {
        buf_insert_lines(e->buf, a, 0, text, n);
    }
    free(text);
    finish(e);
}

void edit_delete_lines(Editor *e)
{
    size_t a, b;

    sel_lines(e, &a, &b);
    begin(e);
    buf_delete_lines(e->buf, a, b);
    e->sel_on = 0;
    settle_cursor(e, a);
    finish(e);
}

void edit_join_lines(Editor *e)
{
    size_t y0, x0, y1, x1, count, k, y;

    if (edit_sel_active(e)) {
        edit_sel_range(e, &y0, &x0, &y1, &x1);
    } else {
        y0 = y1 = e->cy;
    }
    count = y1 > y0 ? y1 - y0 : 1;
    if (y0 + count >= e->buf->nlines)
        count = e->buf->nlines - 1 - y0;
    if (count == 0)
        return;

    begin(e);
    y = y0;
    for (k = 0; k < count; k++) {
        const Line *l, *next = &e->buf->lines[y + 1];
        size_t lead = 0;

        while (lead < next->len && is_blank(next->s[lead]))
            lead++;
        buf_delete(e->buf, y + 1, 0, lead);
        l = &e->buf->lines[y];
        next = &e->buf->lines[y + 1];
        e->cx = l->len;
        if (l->len > 0 && next->len > 0 && !is_blank(l->s[l->len - 1]))
            buf_insert(e->buf, y, l->len, " ", 1);
        buf_join(e->buf, y);
    }
    e->cy = y;
    e->sel_on = 0;
    finish(e);
}

/* ---- comments ---------------------------------------------------------- */

static size_t lead_blank(const Line *l)
{
    size_t n = 0;

    while (n < l->len && is_blank(l->s[n]))
        n++;
    return n;
}

static int has_marker(const Line *l, size_t at, const char *m)
{
    size_t n = strlen(m);

    return l->len >= at + n && memcmp(l->s + at, m, n) == 0;
}

/* Moves a byte offset of line y after n bytes were inserted (n > 0) or
 * removed (n < 0) at column col of that line. */
static void shift_x(size_t *x, size_t col, long n)
{
    if (n > 0) {
        if (*x >= col)
            *x += (size_t)n;
    } else if (*x > col) {
        size_t r = (size_t)-n;

        *x = *x >= col + r ? *x - r : col;
    }
}

static void shift_pos(Editor *e, size_t y, size_t col, long n)
{
    if (e->cy == y)
        shift_x(&e->cx, col, n);
    if (e->sel_on && e->ay == y)
        shift_x(&e->ax, col, n);
}

void edit_toggle_comment(Editor *e)
{
    const char *open, *close;
    size_t a, b, k, indent = (size_t)-1, olen, clen = 0;
    int uncomment = 1, any = 0;

    sel_lines(e, &a, &b);
    if (!syn_comment_at(e->doc->syn, e->buf, a, &open, &close)) {
        editor_set_msg(e, MSG_TTL_MS, tr("No comment defined for this language"));
        return;
    }
    olen = strlen(open);
    if (close)
        clen = strlen(close);
    for (k = a; k <= b; k++) {
        const Line *l = &e->buf->lines[k];
        size_t lead = lead_blank(l);

        if (lead == l->len)
            continue;
        any = 1;
        if (lead < indent)
            indent = lead;
        if (!has_marker(l, lead, open))
            uncomment = 0;
    }
    if (!any) {
        /* only blank lines: comment them at column 0 */
        indent = 0;
        uncomment = 0;
    }

    begin(e);
    for (k = a; k <= b; k++) {
        const Line *l = &e->buf->lines[k];
        size_t lead = lead_blank(l);

        if (any && lead == l->len)
            continue;
        if (uncomment) {
            size_t r = olen, col = lead;

            if (has_marker(l, col + r, " "))
                r++;
            buf_delete(e->buf, k, col, r);
            shift_pos(e, k, col, -(long)r);
            if (close) {
                const Line *m = &e->buf->lines[k];
                size_t end = m->len;

                while (end > 0 && is_blank(m->s[end - 1]))
                    end--;
                if (end >= clen && memcmp(m->s + end - clen, close, clen) == 0) {
                    size_t from = end - clen, n = clen + (m->len - end);

                    if (from > 0 && is_blank(m->s[from - 1])) {
                        from--;
                        n++;
                    }
                    buf_delete(e->buf, k, from, n);
                    shift_pos(e, k, from, -(long)n);
                }
            }
        } else {
            SBuf t = SBUF_INIT;

            sbuf_add(&t, open, olen);
            sbuf_adds(&t, " ");
            buf_insert(e->buf, k, indent, t.p, t.len);
            shift_pos(e, k, indent, (long)t.len);
            if (close) {
                const Line *m = &e->buf->lines[k];

                t.len = 0;
                sbuf_adds(&t, " ");
                sbuf_add(&t, close, clen);
                buf_insert(e->buf, k, m->len, t.p, t.len);
            }
            sbuf_free(&t);
        }
    }
    finish(e);
}

void edit_replace_word(Editor *e, size_t from_x, const char *s, size_t n)
{
    if (from_x > e->cx)
        return;
    begin(e);
    e->sel_on = 0;
    buf_delete(e->buf, e->cy, from_x, e->cx - from_x);
    buf_insert(e->buf, e->cy, from_x, s, n);
    e->cx = from_x + n;
    finish(e);
}

static int is_word_ch(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* True if the line holds name as a whole word. */
static int has_word(const Line *l, const char *name, size_t nl)
{
    size_t i;

    for (i = 0; i + nl <= l->len; i++)
        if (memcmp(l->s + i, name, nl) == 0 && (i == 0 || !is_word_ch(l->s[i - 1])) &&
            (i + nl == l->len || !is_word_ch(l->s[i + nl])))
            return 1;
    return 0;
}

static int starts_with(const Line *l, const char *w)
{
    size_t n = strlen(w);

    return l->len >= n && memcmp(l->s, w, n) == 0 && (l->len == n || !is_word_ch(l->s[n]));
}

/* The "use Class;" statement for a class of the project or of the library: nothing if it is
 * imported or defined in the file, or in the namespace of the file. Keeps the use block sorted. */
static void add_use(Editor *e, const char *fq)
{
    const Buffer *b = e->buf;
    const char *name = strrchr(fq, '\\');
    size_t nl, y, nsl = (size_t)-1, php = (size_t)-1, first_use = (size_t)-1, last_use = 0, at;
    size_t ins = (size_t)-1, added = 0;
    int header = 1;
    char stmt[264], pre[2] = "", post[2] = "";
    size_t sl, lim = b->nlines < 600 ? b->nlines : 600;

    name = name ? name + 1 : fq;
    nl = strlen(name);
    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];
        size_t i = 0;

        while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t'))
            i++;
        {
            Line t = *l;

            t.s += i;
            t.len -= i;
            if (php == (size_t)-1 && t.len >= 5 && memcmp(t.s, "<?php", 5) == 0)
                php = y;
            if (header && starts_with(&t, "namespace") && nsl == (size_t)-1) {
                nsl = y;
            } else if (header && starts_with(&t, "use")) {
                if (has_word(&t, name, nl))
                    return;                         /* imported (maybe with as) */
                if (memchr(t.s, '{', t.len) == NULL && memchr(t.s, '(', t.len) == NULL &&
                    !has_word(&t, "function", 8) && !has_word(&t, "const", 5)) {
                    if (first_use == (size_t)-1)
                        first_use = y;
                    last_use = y;
                    /* the first use line that sorts after fq: the new one goes before it */
                    if (ins == (size_t)-1 && t.len > 4) {
                        char cur[256];
                        size_t cl = t.len - 4 < sizeof cur - 1 ? t.len - 4 : sizeof cur - 1;

                        memcpy(cur, t.s + 4, cl);
                        cur[cl] = '\0';
                        while (cl > 0 && (cur[cl - 1] == ';' || cur[cl - 1] == ' '))
                            cur[--cl] = '\0';
                        if (strcasecmp(cur, fq) > 0)
                            ins = y;
                    }
                }
            } else if (t.len > 0 && (starts_with(&t, "class") || starts_with(&t, "abstract") ||
                                     starts_with(&t, "final") || starts_with(&t, "interface") ||
                                     starts_with(&t, "trait") || starts_with(&t, "enum") ||
                                     starts_with(&t, "function"))) {
                header = 0;
            }
            if (t.len > 6 && (starts_with(&t, "class") || starts_with(&t, "interface") ||
                              starts_with(&t, "trait") || starts_with(&t, "enum") ||
                              starts_with(&t, "abstract") || starts_with(&t, "final"))) {
                /* defined in this very file */
                size_t k;

                for (k = 0; k + nl <= t.len; k++)
                    if (memcmp(t.s + k, name, nl) == 0 && k > 0 && t.s[k - 1] == ' ' &&
                        (k + nl == t.len || !is_word_ch(t.s[k + nl])))
                        return;
            }
        }
    }
    /* the namespace of the file: a class of the same one needs no use */
    if (nsl != (size_t)-1) {
        const Line *l = &b->lines[nsl];
        size_t i = 0, k;
        const char *sep = strrchr(fq, '\\');

        while (i < l->len && l->s[i] != ' ')
            i++;
        while (i < l->len && l->s[i] == ' ')
            i++;
        for (k = l->len; k > i && (l->s[k - 1] == ';' || l->s[k - 1] == ' '); k--)
            ;
        if (sep && (size_t)(sep - fq) == k - i && memcmp(fq, l->s + i, k - i) == 0)
            return;
    } else if (!strchr(fq, '\\')) {
        return;                                     /* a global class in a file without namespace */
    }
    sl = (size_t)snprintf(stmt, sizeof stmt, "use %s;\n", fq);
    if (sl >= sizeof stmt)
        return;
    if (first_use != (size_t)-1) {
        at = ins != (size_t)-1 ? ins : last_use + 1;
    } else {
        size_t base = nsl != (size_t)-1 ? nsl : php;

        if (base == (size_t)-1)
            return;
        at = base + 1;
        if (at < b->nlines && b->lines[at].len == 0)
            at++;                                   /* after the blank line */
        else
            pre[0] = '\n';
        if (at >= b->nlines || b->lines[at].len > 0)
            post[0] = '\n';
    }
    if (at >= b->nlines)
        return;
    {
        SBuf text = SBUF_INIT;
        size_t yy = at, xx = 0, i;

        sbuf_adds(&text, pre);
        sbuf_add(&text, stmt, sl);
        sbuf_adds(&text, post);
        for (i = 0; i < text.len; i++)
            if (text.p[i] == '\n')
                added++;
        buf_insert_text(e->buf, &yy, &xx, text.p, text.len);
        sbuf_free(&text);
    }
    if (at <= e->cy)
        e->cy += added;
}

int edit_add_include(Editor *e, const char *hdr)
{
    const Buffer *b = e->buf;
    size_t lim = b->nlines < 400 ? b->nlines : 400, y, first_sys = (size_t)-1, last_sys = 0;
    size_t first_loc = (size_t)-1, ins = (size_t)-1, at, top = 0, hl = strlen(hdr), added = 0;
    int sorted = 1, in_comment = 0, top_done = 0;
    char prev[80] = "", pre[2] = "", post[2] = "";
    SBuf text = SBUF_INIT;

    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];
        size_t i = 0;
        const char *s;

        while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t'))
            i++;
        s = l->s + i;
        if (l->len - i >= 8 && memcmp(s, "#include", 8) == 0) {
            const char *q = s + 8, *end = l->s + l->len;
            char name[80];
            size_t nl = 0;
            int sys;

            while (q < end && (*q == ' ' || *q == '\t'))
                q++;
            sys = q < end && *q == '<';
            if (q < end && (*q == '<' || *q == '"')) {
                char close = *q == '<' ? '>' : '"';

                for (q++; q + nl < end && q[nl] != close && nl < sizeof name - 1; nl++)
                    name[nl] = q[nl];
                name[nl] = '\0';
                if (nl == hl && memcmp(name, hdr, hl) == 0)
                    return 0;                       /* already there (either way) */
                if (sys) {
                    if (first_sys == (size_t)-1)
                        first_sys = y;
                    last_sys = y;
                    if (strcmp(prev, name) > 0)
                        sorted = 0;
                    snprintf(prev, sizeof prev, "%s", name);
                    if (ins == (size_t)-1 && strcmp(name, hdr) > 0)
                        ins = y;
                } else if (first_loc == (size_t)-1) {
                    first_loc = y;
                }
            }
            top_done = 1;
            continue;
        }
        if (top_done)
            continue;
        /* where the includes go in a file that has none: after the comments and the guard */
        if (in_comment) {
            top = y + 1;
            for (i = 0; i + 1 < l->len; i++)
                if (l->s[i] == '*' && l->s[i + 1] == '/')
                    in_comment = 0;
        } else if (l->len - i >= 2 && s[0] == '/' && s[1] == '*') {
            in_comment = 1;
            top = y + 1;
            for (i += 2; i + 1 < l->len; i++)
                if (l->s[i] == '*' && l->s[i + 1] == '/')
                    in_comment = 0;
        } else if (l->len - i >= 2 && s[0] == '/' && s[1] == '/') {
            top = y + 1;
        } else if (l->len == i) {
            if (top == y)
                top = y + 1;
        } else if ((l->len - i >= 7 && memcmp(s, "#ifndef", 7) == 0) ||
                   (l->len - i >= 7 && memcmp(s, "#define", 7) == 0 && top > 0) ||
                   (l->len - i >= 12 && memcmp(s, "#pragma once", 12) == 0)) {
            top = y + 1;
        } else {
            top_done = 1;
        }
    }
    if (first_sys != (size_t)-1) {
        at = sorted && ins != (size_t)-1 ? ins : last_sys + 1;
    } else if (first_loc != (size_t)-1) {
        at = first_loc;
        post[0] = '\n';                             /* system includes before the local ones */
    } else {
        at = top;
        if (at < b->nlines && b->lines[at].len > 0)
            post[0] = '\n';
    }
    sbuf_adds(&text, pre);
    sbuf_adds(&text, "#include <");
    sbuf_adds(&text, hdr);
    sbuf_adds(&text, ">\n");
    sbuf_adds(&text, post);
    {
        size_t yy = at, xx = 0, k;

        if (at >= b->nlines) {
            /* the file ends before the place: append after its last line */
            yy = b->nlines - 1;
            xx = b->lines[yy].len;
            if (xx > 0)
                pre[0] = '\n';
            sbuf_free(&text);
            sbuf_adds(&text, pre);
            sbuf_adds(&text, "#include <");
            sbuf_adds(&text, hdr);
            sbuf_adds(&text, ">\n");
            sbuf_adds(&text, post);
            at = b->nlines;
        }
        for (k = 0; k < text.len; k++)
            if (text.p[k] == '\n')
                added++;
        buf_insert_text(e->buf, &yy, &xx, text.p, text.len);
    }
    sbuf_free(&text);
    if (at <= e->cy)
        e->cy += added;
    return 1;
}

void edit_replace_class(Editor *e, size_t from_x, const char *s, size_t n, const char *fq)
{
    if (from_x > e->cx)
        return;
    begin(e);
    e->sel_on = 0;
    buf_delete(e->buf, e->cy, from_x, e->cx - from_x);
    buf_insert(e->buf, e->cy, from_x, s, n);
    e->cx = from_x + n;
    if (fq && fq[0])
        add_use(e, fq);
    finish(e);
}

void edit_snip_goto(Editor *e, size_t idx)
{
    SnipSession *s = &e->snip;

    if (!s->on || idx >= s->n)
        return;
    s->cur = idx;
    e->cy = s->stop[idx].y;
    e->cx = s->stop[idx].x;
    e->sel_on = 0;
    if (s->stop[idx].len > 0) {
        e->sel_on = 1;
        e->ay = e->cy;
        e->ax = e->cx;
        e->cx += s->stop[idx].len;
    }
    if (idx + 1 == s->n)
        s->on = 0;
}

void edit_expand_snippet(Editor *e, size_t from_x, const SnipDef *d)
{
    SBuf raw = SBUF_INIT, text = SBUF_INIT;
    SnipStops st;
    size_t indent, y0 = e->cy, y, x, i, k;
    const Line *l = cur_line(e);

    if (from_x > e->cx)
        return;
    indent = lead_blank(l);
    if (indent > from_x)
        indent = from_x;
    {
        char *ind = xmalloc(indent + 1);

        memcpy(ind, l->s, indent);
        {
            /* the tabs of the body are one level of the document's own indentation */
            SBuf body = SBUF_INIT;
            char unit[17];
            size_t ul = indent_unit(e, unit);
            const char *q;

            for (q = d->body; *q; q++) {
                if (*q == '\t')
                    sbuf_add(&body, unit, ul);
                else
                    sbuf_add(&body, q, 1);
            }
            sbuf_add(&body, "", 1);
            snip_parse(body.p, &raw, &st);
            sbuf_free(&body);
        }
        /* each line after the first starts with the indentation of the first */
        for (i = 0; i < raw.len; i++) {
            sbuf_add(&text, raw.p + i, 1);
            if (raw.p[i] == '\n')
                sbuf_add(&text, ind, indent);
        }
        begin(e);
        e->sel_on = 0;
        buf_delete(e->buf, e->cy, from_x, e->cx - from_x);
        y = e->cy;
        x = from_x;
        buf_insert_text(e->buf, &y, &x, text.p, text.len);
        e->cy = y;
        e->cx = x;
        e->snip.n = 0;
        for (k = 0; k < st.n; k++) {
            size_t off = st.stop[k].off, nl = 0, last = 0;

            for (i = 0; i < off && i < raw.len; i++)
                if (raw.p[i] == '\n')
                    nl++;
            off += nl * indent;
            nl = 0;
            for (i = 0; i < off && i < text.len; i++)
                if (text.p[i] == '\n') {
                    nl++;
                    last = i + 1;
                }
            e->snip.stop[k].y = y0 + nl;
            e->snip.stop[k].x = (nl == 0 ? from_x : 0) + (off - last);
            e->snip.stop[k].len = st.stop[k].len;
            e->snip.n++;
        }
        free(ind);
    }
    sbuf_free(&raw);
    sbuf_free(&text);
    e->snip.on = e->snip.n > 1;
    edit_snip_goto(e, 0);
    if (e->snip.n == 1) {
        e->snip.on = 0;
        e->cy = e->snip.stop[0].y;
        e->cx = e->snip.stop[0].x;
    }
    finish(e);
}

void edit_store_clip(Editor *e, char *text, size_t n, const char *verb)
{
    store_clip(e, text, n, 0, verb);
}

#define FILTER_TIMEOUT_MS 10000
#define FILTER_MAX_OUTPUT (64u << 20)

void edit_filter(Editor *e)
{
    char cmd[sizeof e->run.filter];

    if (editor_prompt(e, tr("Filter with: "), e->run.filter, cmd, sizeof cmd) == 0)
        return;
    snprintf(e->run.filter, sizeof e->run.filter, "%s", cmd);
    edit_filter_cmd(e, cmd);
}

void edit_filter_cmd(Editor *e, const char *cmd)
{
    char *in;
    const char *argv[4];
    SBuf out = SBUF_INIT, err = SBUF_INIT;
    size_t n, y0, x0, y1, x1;
    int st = 0, r, sel = edit_sel_active(e);

    if (sel) {
        edit_sel_range(e, &y0, &x0, &y1, &x1);
    } else {
        y0 = x0 = 0;
        y1 = e->buf->nlines - 1;
        x1 = e->buf->lines[y1].len;
    }
    in = buf_get_range(e->buf, y0, x0, y1, x1, &n);
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = cmd;
    argv[3] = NULL;
    editor_set_msg(e, 0, tr("Filtering…"));
    r = shell_capture(argv, e->root, in, n, &out, &err, FILTER_TIMEOUT_MS, FILTER_MAX_OUTPUT,
                      &st);
    if (r == -1) {
        editor_set_msg(e, MSG_TTL_MS, tr("Cannot start the command"));
    } else if (r == -2) {
        editor_set_msg(e, MSG_TTL_MS, tr("The command took more than %d s: text unchanged"),
                       FILTER_TIMEOUT_MS / 1000);
    } else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        size_t k = 0;

        while (k < err.len && err.p[k] != '\n')
            k++;
        if (k > 0)
            editor_set_msg(e, MSG_TTL_MS, tr("The command failed: %.*s"), (int)(k > 120 ? 120 : k),
                           err.p);
        else
            editor_set_msg(e, MSG_TTL_MS, tr("The command exited with an error: text unchanged"));
    } else {
        size_t ny = y0, nx = x0;

        /* a filter that adds a final newline to text that had none must not add a line */
        if (out.len > 0 && out.p[out.len - 1] == '\n' && (n == 0 || in[n - 1] != '\n'))
            out.len--;
        begin(e);
        buf_delete_range(e->buf, y0, x0, y1, x1);
        buf_insert_text(e->buf, &ny, &nx, out.p, out.len);
        if (sel) {
            e->sel_on = 1;
            e->ay = y0;
            e->ax = x0;
            e->cy = ny;
            e->cx = nx;
        } else {
            e->sel_on = 0;
            e->cy = e->cy < e->buf->nlines ? e->cy : e->buf->nlines - 1;
            e->cx = e->cx < cur_line(e)->len ? e->cx : cur_line(e)->len;
        }
        finish(e);
        editor_set_msg(e, MSG_TTL_MS, tr("Filtered with: %s"), cmd);
    }
    free(in);
    sbuf_free(&out);
    sbuf_free(&err);
}

void edit_trim_trailing(Editor *e, Doc *d)
{
    Buffer *b = &d->buf;
    size_t y;
    int any = 0;

    for (y = 0; y < b->nlines; y++) {
        const Line *l = &b->lines[y];
        size_t end = l->len;

        while (end > 0 && is_blank(l->s[end - 1]))
            end--;
        if (end == l->len)
            continue;
        if (!any) {
            buf_undo_begin(b, d == e->doc ? e->cy : 0, d == e->doc ? e->cx : 0);
            any = 1;
        }
        buf_delete(b, y, end, l->len - end);
        if (d == e->doc && e->cy == y && e->cx > end)
            e->cx = end;
    }
    if (any)
        buf_undo_close(b);
}
