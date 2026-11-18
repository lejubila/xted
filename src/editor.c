#include "editor.h"
#include "version.h"
#include "json.h"
#include "md.h"
#include "edit.h"
#include "imports.h"
#include "cfgsave.h"
#include "commands.h"
#include "config.h"
#include "editorconfig.h"
#include "indent.h"
#include "cursors.h"
#include "find.h"
#include "fold.h"
#include "git.h"
#include "i18n.h"
#include "mouse.h"
#include "run.h"
#include "session.h"
#include "stdsym.h"
#include "utf8.h"
#include "util.h"

#include <ctype.h>
#include <limits.h>
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
    e->colors_mode = -1;
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
    files_scan_free(e->fscan);
    free(e->dests);
    free(e->proj);
    picker_free(&e->picker);
    free(e->outline);
    free(e->pal);
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
        editor_set_msg(e, MSG_TTL_MS, tr("New file"));
    else if (d->buf.has_nul)
        editor_set_msg(e, MSG_TTL_MS, tr("Warning: the file contains NUL bytes (binary?)"));
    else if (d->readonly)
        editor_set_msg(e, MSG_TTL_MS, tr("File is read-only"));
}

/* An untouched, unnamed, empty document is replaced by the first file opened. */
static int is_blank_doc(const Doc *d)
{
    return !d->buf.path && !d->buf.modified && d->buf.nlines == 1 && d->buf.lines[0].len == 0;
}

/* What the .xted of the document's project says for its language; 0 if there is none. */
static int project_lang(const Doc *d, LangCfg *out)
{
    Config c;
    char *f;

    if (d->scratch || !d->buf.path || !(f = cfgsave_find_project(d->buf.path)))
        return 0;
    config_init(&c);
    config_load_file(&c, f);
    *out = config_lang(&c, d->syn ? syn_name(d->syn) : NULL);
    config_free(&c);
    free(f);
    return 1;
}

/* The settings of the language the document turned out to be in. */
static void apply_cfg(Editor *e, Doc *d)
{
    LangCfg l = config_lang(&e->cfg, d->syn ? syn_name(d->syn) : NULL), pl;
    EcProps ec;
    int tabw, expand, iw, trim, final_nl;

    /* what the project's .xted says wins over the user's configuration, which wins over
     * .editorconfig, then the style the file has, then the defaults; a command given in this
     * session wins over all */
    if (project_lang(d, &pl))
        config_merge(&l, &pl);
    tabw = l.tab_width;
    expand = l.expand_tab;
    iw = l.indent_width;
    trim = l.trim_trailing_space;
    final_nl = l.final_newline;
    if (d->det.set && !(l.set & CFG_EXPAND_TAB)) {
        expand = d->det.expand;
        if (!(l.set & CFG_INDENT_WIDTH) && d->det.expand)
            iw = d->det.width;
    }
    if (!d->scratch && editorconfig_lookup(d->buf.path, &ec) > 0) {
        if (ec.expand >= 0 && !(l.set & CFG_EXPAND_TAB))
            expand = ec.expand;
        if (ec.size > 0 && !(l.set & CFG_INDENT_WIDTH))
            iw = ec.size;
        if (ec.tab_width > 0 && !(l.set & CFG_TAB_WIDTH))
            tabw = ec.tab_width;
        else if (ec.size > 0 && ec.expand == 1 && !(l.set & CFG_TAB_WIDTH))
            tabw = ec.size;
        if (ec.trim >= 0 && !(l.set & CFG_TRIM))
            trim = ec.trim;
        if (ec.final_nl >= 0 && !(l.set & CFG_FINAL_NL))
            final_nl = ec.final_nl;
    }
    if (d->man.set) {
        expand = d->man.expand;
        if (d->man.width > 0)
            iw = d->man.width;
        if (d->man.tabw > 0)
            tabw = d->man.tabw;
    }
    d->tabw = tabw;
    d->expand_tab = expand;
    d->indent_w = iw > 0 ? iw : tabw;
    d->trim_space = trim;
    d->final_nl = final_nl;
    d->autosuggest = d->man_sug ? d->man_sug == 1 : l.autosuggest;
    d->autopair = d->man_pair ? d->man_pair == 1 : l.autopair;
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
    i18n_init(e->cfgdir);
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
    if (i18n_set(e->cfg.language[0] ? e->cfg.language : "auto") < 0 && e->cfg.language[0] &&
        strcmp(e->cfg.language, "auto") != 0) {
        char lm[100];

        snprintf(lm, sizeof lm, tr("language «%s» not found"), e->cfg.language);
        config_error(&e->cfg, 0, lm);
    }
    if (e->cfg.clipboard >= 0)
        e->clipboard_osc52 = e->cfg.clipboard;
    if (e->cfg.mouse >= 0)
        e->mouse_on = e->cfg.mouse;
    if (e->cfg.colors >= 0) {
        e->colors_mode = e->cfg.colors;
        e->color_depth = e->cfg.colors;
    }
    if (e->cfg.glyphs >= 0)
        e->glyphs = e->cfg.glyphs;
    if (e->cfg.theme[0]) {
        int t = theme_by_name(e->cfg.theme);

        if (t >= 0) {
            e->theme_idx = (size_t)t;
            e->theme = theme_get((size_t)t);
        } else {
            snprintf(msg, sizeof msg, tr("unknown theme «%s»"), e->cfg.theme);
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
                snprintf(msg, sizeof msg, tr("unknown command «%s»"), kl->cmd);
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
                       e->cfg.nerrors > 1 ? tr(" (and other errors)") : "");
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
        editor_set_msg(e, MSG_TTL_MS, tr("Save the file to refresh the Git markers"));
    else {
        e->git_off = 0;
        git_refresh_doc(e, e->doc);
        relayout(e);
        editor_set_msg(e, MSG_TTL_MS, e->doc->gutter ? tr("Git markers refreshed")
                                                      : tr("The file is not in a Git repository"));
    }
}

/* Looks at the text for its indentation style (Doc.det). Returns whether it found one. */
static int detect_indent(Doc *d)
{
    int tabs = 0, width = 0;

    d->det.set = indent_detect(&d->buf, &tabs, &width);
    d->det.expand = !tabs;
    d->det.width = width;
    return d->det.set;
}

static Doc *install_doc(Editor *e, Doc *d)
{
    if (!d->scratch && d->buf.nlines > 1)
        detect_indent(d);
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
/* A Markdown preview pane: made again when its source changed, the pane's width or the glyphs;
 * scrolled to the part the source's cursor is in, when the source is the pane being edited. */
static void md_refresh(Editor *e, Win *w)
{
    MdPreview *m = w->doc->md;
    Doc *src = m->src;
    int width = win_text_w(w), ascii = e->glyphs == GLYPHS_ASCII;
    size_t th = w->h >= 2 ? (size_t)w->h - 1 : 1;

    if (m->gen != src->buf.gen || m->width != width || m->glyphs != ascii || !m->st) {
        md_build(m, &src->buf, &w->doc->buf, width, ascii);
        m->gen = src->buf.gen;
        m->synced_y = (size_t)-1;
    }
    if (e->doc == src && w != e->layout.active && m->synced_y != e->cy) {
        size_t t = md_line_for(m, e->cy);

        w->view.cy = t;
        w->view.cx = 0;
        w->view.rowoff = t > th / 3 ? t - th / 3 : 0;
        if (w->view.rowoff + th > w->doc->buf.nlines)
            w->view.rowoff = w->doc->buf.nlines > th ? w->doc->buf.nlines - th : 0;
        m->synced_y = e->cy;
    }
}

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

        if (d->md && d->md->src)
            md_refresh(e, w[i]);
        view_clamp(&d->buf, &w[i]->view);
        if (w[i] != e->layout.active)
            scroll_view(&d->buf, &w[i]->view, th, (size_t)win_text_w(w[i]), d->tabw);
        syn_update(d->syn, &d->buf, w[i]->view.rowoff + th);
    }
    e->match_on = syn_match_bracket(e->doc->syn, e->buf, e->cy, e->cx,
                                    &e->by, &e->bx, &e->my, &e->mx);
    /* the call the cursor is in: its signature goes to the status bar */
    if (e->hint_doc != e->doc || e->hint_y != e->cy || e->hint_x != e->cx ||
        e->hint_gen != e->buf->gen) {
        e->hint_doc = e->doc;
        e->hint_y = e->cy;
        e->hint_x = e->cx;
        e->hint_gen = e->buf->gen;
        e->hint[0] = '\0';
        if (e->doc->autosuggest && !cur_multi(e) && !e->prompt_on) {
            if (!e->comp.sc)
                e->comp.sc = sym_cache_new();
            sym_budget(e->comp.sc, COMPLETE_SCAN_MS);
            if (!sym_call_hint(e->comp.sc, e->docs, e->ndocs, e->doc, e->cy, e->cx, e->hint,
                               sizeof e->hint, &e->hint_a, &e->hint_b))
                e->hint[0] = '\0';
        }
    }
}

void editor_render_prepare(Editor *e)
{
    prepare_frame(e);
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
static void files_scan_stop(Editor *e);
static void files_scan_pump(Editor *e);

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
            /* the walk of the project for Ctrl+P ends with the list */
            if (e->fscan && (!e->picker.on || e->picker_kind != PK_FILES))
                files_scan_stop(e);
            /* a scan of the project cut short by the time limit goes on while the user waits */
            if ((e->comp.scanning || e->fscan) && (timeout < 0 || timeout > COMPLETE_IDLE_MS))
                timeout = COMPLETE_IDLE_MS;
            if (run_timeout(e) >= 0 && (timeout < 0 || run_timeout(e) < timeout))
                timeout = run_timeout(e);
            ev = term_wait(timeout);
            if (ev & TERM_EV_RESIZE)
                resize(e);
            if ((ev & TERM_EV_FD) || (!ev && run_timeout(e) >= 0))
                run_pump(e);
            if (!ev && e->fscan)
                files_scan_pump(e);
            if (!ev && e->comp.scanning)
                complete_resume(&e->comp, e->docs, e->ndocs, e->doc, e->cy, e->cx);
            if (!(ev & TERM_EV_KEY))
                continue;
        }
        term_read_key(k);
        if (k->code == KEY_EOF) {
            e->quit = 1;
            return -1;
        }
        if (k->code == KEY_MOUSE) {
            /* a click on a button of the message row is its key; its release is nothing */
            MouseEv ev = term_mouse_event();
            Key hk;

            if (ev.y == e->rows - 1 && ev.kind == MOUSE_PRESS && ev.button == 0 &&
                render_hint_key(ev.x, ev.y, &hk))
                *k = hk;
            else if (ev.y == e->rows - 1 && ev.kind == MOUSE_RELEASE)
                k->code = KEY_NONE;
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
    e->asking = keys;                   /* the answers as buttons */
    for (;;) {
        Key k;
        uint32_t c;

        if (next_key(e, &k) < 0) {
            e->asking = NULL;
            return 0;
        }
        if (k.code == KEY_ESC) {
            e->msg[0] = '\0';
            e->asking = NULL;
            return 0;
        }
        c = (k.code >= 'A' && k.code <= 'Z') ? k.code + 32 : k.code;
        /* the prompts name the answers in the language of the user (s/n in Italian, y/n in
         * English); the callers test for s and t, so y and a count as those */
        if (c == 'y' && strchr(keys, 's'))
            c = 's';
        else if (c == 'a' && strchr(keys, 't'))
            c = 't';
        if (c != 0 && c < 0x80 && strchr(keys, (int)c)) {
            e->msg[0] = '\0';
            e->asking = NULL;
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
        editor_set_msg(e, MSG_TTL_MS, tr("«%s» no longer exists on disk"), doc_base_name(d));
        return 0;
    }
    snprintf(q, sizeof q, tr("«%s» changed on disk%s. Reload? (r = reload, i = ignore)"),
             doc_base_name(d), d->buf.modified ? tr(" and you have unsaved changes") : "");
    c = ask(e, q, "ri");
    if (c == 'r') {
        if (doc_reload(d, err, sizeof err) < 0) {
            editor_set_msg(e, MSG_TTL_MS, tr("Reload failed: %s"), err);
            return 0;
        }
        view_clamp(&d->buf, &e->layout.active->view);
        load_win(e, e->layout.active);
        editor_set_msg(e, MSG_TTL_MS, tr("Reloaded from disk"));
        return 1;
    }
    doc_accept_disk(d);
    return 0;
}

/* ---- JSON ------------------------------------------------------------- */

static int is_json(const Doc *d)
{
    return d->syn && strcmp(syn_name(d->syn), "JSON") == 0;
}

/* A comma before } or ] is accepted in JSONC, JSON5 and the files that are read that way:
 * tsconfig.json, jsconfig.json, VS Code's settings (.vscode/), devcontainer.json. */
static int json_lenient(const Doc *d)
{
    const char *p = d->buf.path, *base, *dot;

    if (!p)
        return 0;
    base = strrchr(p, '/');
    base = base ? base + 1 : p;
    dot = strrchr(base, '.');
    return (dot && (strcmp(dot, ".jsonc") == 0 || strcmp(dot, ".json5") == 0)) ||
           strncmp(base, "tsconfig", 8) == 0 || strncmp(base, "jsconfig", 8) == 0 ||
           strstr(p, "/.vscode/") != NULL || strcmp(base, "devcontainer.json") == 0 ||
           strcmp(base, ".devcontainer.json") == 0;
}

static char *doc_text(const Doc *d, size_t *n)
{
    const Buffer *b = &d->buf;

    return buf_get_range(b, 0, 0, b->nlines - 1, b->lines[b->nlines - 1].len, n);
}

/* "JSON: line 12, column 5: a comma is missing" in out; the cursor goes there with go. */
static void json_error(Editor *e, const JsonErr *je, int go, char *out, size_t n)
{
    snprintf(out, n, tr("JSON: line %zu, column %zu: %s"), je->line + 1, je->col + 1,
             tr(json_err_text(je->code)));
    if (go && je->line < e->buf->nlines) {
        e->cy = je->line;
        e->cx = je->col <= e->buf->lines[e->cy].len ? je->col : e->buf->lines[e->cy].len;
        e->want_col = editor_cursor_col(e);
    }
}

static void remove_doc(Editor *e, Doc *d);

/* The preview document of src: the one there is, or a new one. */
static Doc *preview_doc(Editor *e, Doc *src)
{
    Doc *pd;
    const char *base;
    char title[300];
    size_t k;

    for (k = 0; k < e->ndocs; k++)
        if (e->docs[k]->md && e->docs[k]->md->src == src)
            return e->docs[k];
    pd = doc_new();
    pd->scratch = 1;
    pd->readonly = 1;
    pd->tabw = src->tabw;
    base = src->buf.path ? strrchr(src->buf.path, '/') : NULL;
    base = base ? base + 1 : src->buf.path ? src->buf.path : src->title ? src->title : "";
    snprintf(title, sizeof title, "[%s] %s", tr("Preview"), base);
    pd->title = xstrdup(title);
    pd->md = xmalloc(sizeof *pd->md);
    memset(pd->md, 0, sizeof *pd->md);
    pd->md->src = src;
    pd->md->synced_y = (size_t)-1;
    add_doc(e, pd);
    return pd;
}

/* Opens the rendered preview of the Markdown document beside it, or closes it. */
void editor_cmd_md_preview(Editor *e)
{
    Win *w[64], *old = e->layout.active, *nw;
    size_t n = layout_wins(&e->layout, w, 64), k;
    Doc *src = e->doc, *pd = NULL;

    if (src->md) {
        /* in the preview itself: its pane goes, and the preview with it (it is no buffer) */
        if (layout_count(&e->layout) > 1) {
            layout_close(&e->layout, old);
            load_win(e, e->layout.active);
            remove_doc(e, src);
            relayout(e);
        }
        return;
    }
    if (!src->syn || strcmp(syn_name(src->syn), "Markdown") != 0) {
        editor_set_msg(e, MSG_TTL_MS, "%s", tr("Not a Markdown document"));
        return;
    }
    for (k = 0; k < n && k < 64; k++)
        if (w[k]->doc->md && w[k]->doc->md->src == src) {
            Doc *p = w[k]->doc;

            editor_store_view(e);
            layout_close(&e->layout, w[k]);
            load_win(e, e->layout.active);
            remove_doc(e, p);
            relayout(e);
            return;
        }
    pd = preview_doc(e, src);
    if (editor_split(e, 1) < 0) {
        editor_set_msg(e, MSG_TTL_MS, "%s", tr("Not enough room to split"));
        return;
    }
    /* the new pane, on the right, shows the preview; the source keeps the focus */
    nw = e->layout.active;
    load_win(e, old);
    nw->doc = pd;
    memset(&nw->view, 0, sizeof nw->view);
    pd->md->gen = 0;
    pd->md->synced_y = (size_t)-1;
    relayout(e);
}

void editor_cmd_json_check(Editor *e)
{
    size_t n;
    char *t, msg[200];
    JsonErr je;

    if (!is_json(e->doc)) {
        editor_set_msg(e, MSG_TTL_MS, "%s", tr("Not a JSON document"));
        return;
    }
    t = doc_text(e->doc, &n);
    if (json_check(t, n, json_lenient(e->doc), &je) == 0) {
        editor_set_msg(e, MSG_TTL_MS, "%s", tr("JSON: valid"));
    } else {
        json_error(e, &je, 1, msg, sizeof msg);
        editor_set_msg(e, MSG_TTL_MS, "%s", msg);
    }
    free(t);
}

/* The whole document written again: indented with its own indentation, or on one line. */
static void json_rewrite(Editor *e, int compact)
{
    size_t n, y = 0, x = 0, cy = e->cy;
    char *t, msg[200], ind[17];
    JsonErr je;
    SBuf o = SBUF_INIT;
    int r, w = e->doc->indent_w > 0 && e->doc->indent_w <= 16 ? e->doc->indent_w : 4;
    Buffer *b = e->buf;

    if (e->doc->readonly)
        return;
    if (!is_json(e->doc)) {
        editor_set_msg(e, MSG_TTL_MS, "%s", tr("Not a JSON document"));
        return;
    }
    if (e->doc->expand_tab) {
        memset(ind, ' ', (size_t)w);
        ind[w] = '\0';
    } else {
        snprintf(ind, sizeof ind, "\t");
    }
    t = doc_text(e->doc, &n);
    r = json_format(t, n, json_lenient(e->doc), compact ? NULL : ind, &o, &je);
    free(t);
    if (r < 0) {
        json_error(e, &je, 1, msg, sizeof msg);
        editor_set_msg(e, MSG_TTL_MS, "%s", msg);
        sbuf_free(&o);
        return;
    }
    if (r > 0) {
        editor_set_msg(e, MSG_TTL_MS, "%s", tr("JSON with comments: not rewritten, they would be lost"));
        sbuf_free(&o);
        return;
    }
    /* one step of undo */
    buf_undo_begin(b, e->cy, e->cx);
    buf_delete_range(b, 0, 0, b->nlines - 1, b->lines[b->nlines - 1].len);
    buf_insert_text(b, &y, &x, o.p ? o.p : "", o.len);
    buf_undo_close(b);
    sbuf_free(&o);
    e->cy = cy < b->nlines ? cy : b->nlines - 1;
    e->cx = 0;
    e->want_col = 0;
    editor_set_msg(e, MSG_TTL_MS, tr("JSON: %zu lines"), b->nlines);
}

void editor_cmd_json_format(Editor *e)
{
    json_rewrite(e, 0);
}

void editor_cmd_json_compact(Editor *e)
{
    json_rewrite(e, 1);
}

static int save_doc_as(Editor *e, Doc *d, int force_prompt)
{
    char name[sizeof e->prompt_text], err[256];
    const char *target = NULL;
    long n;

    if (!d->buf.path || force_prompt) {
        if (prompt(e, tr("Save as: "), d->buf.path, name, sizeof name) == 0) {
            editor_set_msg(e, MSG_TTL_MS, tr("Save cancelled"));
            return -1;
        }
        target = name;
        if (d->buf.path && strcmp(name, d->buf.path) == 0)
            target = NULL;
        else if (access(name, F_OK) == 0) {
            int c = ask(e, tr("The file already exists. Overwrite? (y = yes, n = no)"), "sn");

            if (c != 's') {
                editor_set_msg(e, MSG_TTL_MS, tr("Save cancelled"));
                return -1;
            }
        }
    } else if (doc_disk_changed(d) == DISK_CHANGED) {
        int c = ask(e, tr("The file changed on disk. Overwrite? (y = yes, n = no)"), "sn");

        if (c != 's') {
            editor_set_msg(e, MSG_TTL_MS, tr("Save cancelled"));
            return -1;
        }
    }
    if (d->trim_space)
        edit_trim_trailing(e, d);
    if (d->final_nl)
        d->buf.final_newline = 1;
    n = doc_save(d, target, err, sizeof err);
    if (n < 0) {
        editor_set_msg(e, MSG_TTL_MS, tr("Save error: %s"), err);
        return -1;
    }
    apply_cfg(e, d);
    git_refresh_doc(e, d);
    editor_set_msg(e, MSG_TTL_MS, tr("%zu lines, %ld bytes written"), d->buf.nlines, n);
    if (is_json(d)) {
        /* saved anyway, but an invalid JSON is said at once */
        size_t tn;
        char *t = doc_text(d, &tn), msg[200];
        JsonErr je;

        if (json_check(t, tn, json_lenient(d), &je) < 0) {
            json_error(e, &je, 0, msg, sizeof msg);
            editor_set_msg(e, MSG_TTL_MS, "%s", msg);
        }
        free(t);
    }
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
                editor_set_msg(e, MSG_TTL_MS, tr("Error on «%s»: %s"), doc_base_name(d), err);
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
    editor_set_msg(e, MSG_TTL_MS, tr("%zu files saved"), saved);
}

static void cmd_open(Editor *e)
{
    char path[sizeof e->prompt_text], err[256];

    if (prompt(e, tr("Open file: "), NULL, path, sizeof path) == 0)
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
    size_t i, n, k;
    Doc *repl;
    Win *w[64];

    /* its Markdown preview goes with it, and the panes that show it */
    for (i = 0; i < e->ndocs; i++) {
        Doc *p = e->docs[i];

        if (p != d && p->md && p->md->src == d) {
            n = layout_wins(&e->layout, w, 64);
            for (k = 0; k < n && k < 64; k++)
                if (w[k]->doc == p && layout_count(&e->layout) > 1)
                    layout_close(&e->layout, w[k]);
            p->md->src = NULL;
            remove_doc(e, p);
            i = (size_t)-1;             /* the list changed: from the start */
        }
    }
    i = doc_index(e, d);

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
        snprintf(q, sizeof q, tr("«%s» has unsaved changes. Save? (y = yes, n = no%s, Esc = cancel)"),
                 doc_base_name(d), e->ndocs > 1 ? tr(", a = save all") : "");
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
        editor_set_msg(e, MSG_TTL_MS, tr("Not enough room to split"));
}

static void cmd_close_win(Editor *e)
{
    if (layout_count(&e->layout) < 2) {
        editor_set_msg(e, MSG_TTL_MS, tr("This is the only pane"));
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

    picker_open(&e->picker, tr("Buffer"));
    e->picker_kind = PK_BUFFERS;
    e->pal_w1 = 0;
    for (i = 0; i < e->ndocs; i++) {
        const Doc *d = e->docs[i];
        SBuf b = SBUF_INIT;
        size_t n;
        int w;

        /* name, then the path after a unit separator: the renderer puts them in columns */
        sbuf_adds(&b, doc_base_name(d));
        if (d->buf.modified)
            sbuf_adds(&b, " *");
        n = b.len;
        w = (int)utf8_cols(b.p, n, n, 0);
        e->pal_w1 = w > e->pal_w1 ? w : e->pal_w1;
        sbuf_adds(&b, "\x1f");
        if (d->buf.path)
            sbuf_adds(&b, d->buf.path);
        sbuf_add(&b, "", 1);
        picker_add(&e->picker, b.p);
        sbuf_free(&b);
    }
    picker_filter(&e->picker);
    picker_select_item(&e->picker, doc_index(e, e->doc));
}

typedef struct {
    Editor *e;
    size_t cap;
} OutCtx;

static void outline_cb(void *ctx, int kind, const char *name, const char *sig, size_t y, size_t x)
{
    OutCtx *o = ctx;
    Editor *e = o->e;
    SBuf b = SBUF_INIT;
    static const char tag[] = { 'c', 'f', 'm', 'p', 'k' };

    /* RULE_CLASS, RULE_FUNC, RULE_METHOD, RULE_PROP, RULE_CONST in this order. The item is kind,
     * name, parameters and line, cut at the unit separator by the renderer into columns. */
    sbuf_addf(&b, "%c%s\x1f", kind >= RULE_CLASS && kind <= RULE_CONST ? tag[kind - RULE_CLASS] : '?',
              name);
    if ((kind == RULE_FUNC || kind == RULE_METHOD) && strlen(sig) < 48)
        sbuf_addf(&b, "(%s)", sig);
    else if (kind == RULE_FUNC || kind == RULE_METHOD)
        sbuf_adds(&b, "(...)");
    sbuf_addf(&b, "\x1f%zu", y + 1);
    sbuf_add(&b, "", 1);
    picker_add(&e->picker, b.p);
    sbuf_free(&b);
    if (e->noutline == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 64;
        e->outline = xrealloc(e->outline, o->cap * 2 * sizeof *e->outline);
    }
    e->outline[2 * e->noutline] = y;
    e->outline[2 * e->noutline + 1] = x;
    e->noutline++;
}

static void cmd_symbols(Editor *e)
{
    OutCtx o;

    o.e = e;
    o.cap = e->noutline = 0;
    free(e->outline);
    e->outline = NULL;
    picker_open(&e->picker, tr("Symbols"));
    e->picker_kind = PK_SYMBOLS;
    if (!e->comp.sc)
        e->comp.sc = sym_cache_new();
    sym_budget(e->comp.sc, 0);
    sym_outline(e->comp.sc, e->doc, outline_cb, &o);
    if (e->picker.nitems == 0) {
        picker_close(&e->picker);
        editor_set_msg(e, MSG_TTL_MS, tr("No symbols in this file"));
        return;
    }
    /* column widths: name, parameters, line number */
    e->pal_w1 = e->pal_w2 = e->pal_w3 = 0;
    {
        size_t k;

        for (k = 0; k < e->picker.nitems; k++) {
            const char *n = e->picker.items[k] + 1, *a = strchr(n, '\x1f'), *l = a ? strchr(a + 1, '\x1f') : NULL;
            int w;

            if (!l)
                continue;
            w = (int)utf8_cols(n, (size_t)(a - n), (size_t)(a - n), 0);
            e->pal_w1 = w > e->pal_w1 ? w : e->pal_w1;
            w = (int)utf8_cols(a + 1, (size_t)(l - a - 1), (size_t)(l - a - 1), 0);
            e->pal_w2 = w > e->pal_w2 ? w : e->pal_w2;
            w = (int)strlen(l + 1);
            e->pal_w3 = w > e->pal_w3 ? w : e->pal_w3;
        }
    }
    picker_filter(&e->picker);
    /* start on the last definition at or above the cursor */
    {
        size_t k, best = 0;

        for (k = 0; k < e->noutline; k++)
            if (e->outline[2 * k] <= e->cy)
                best = k;
        picker_select_item(&e->picker, best);
    }
}

#define FILES_STEP_US 15000     /* time one step of the project walk may take */

static void files_scan_stop(Editor *e)
{
    files_scan_free(e->fscan);
    e->fscan = NULL;
    e->picker.loading = 0;
}

/* One step of the walk that fills the file list: the list is refiltered and, at the end,
 * the indicator goes away. */
static void files_scan_pump(Editor *e)
{
    int done = files_scan_step(e->fscan, add_file_item, &e->picker, FILES_STEP_US);

    e->picker.spin++;
    picker_refilter(&e->picker);
    if (done) {
        files_scan_stop(e);
        if (e->picker.nitems == 0)
            editor_set_msg(e, MSG_TTL_MS, tr("No files found"));
    }
}

static void cmd_quick_open(Editor *e)
{
    files_scan_stop(e);
    picker_open(&e->picker, "File");
    e->picker_kind = PK_FILES;
    e->fscan = files_scan_begin(e->root ? e->root : ".", NULL);
    e->picker.loading = 1;
    /* a small project is complete before the first frame; a big one goes on in the loop */
    files_scan_pump(e);
}

static void save_settings_to(Editor *e, size_t idx);

static void palette_open(Editor *e, const char *query);

static void picker_accept(Editor *e)
{
    long idx = picker_chosen(&e->picker);
    int kind = e->picker_kind;

    if (kind == PK_COMMANDS && idx >= 0) {
        int c = (int)e->pal[idx];

        e->pal_last = (size_t)c;
        if (commands[c].state != ST_NONE) {
            /* a setting: it changes, the palette stays open and shows the new value */
            char query[PICKER_QMAX];
            size_t top = e->picker.top, sel = e->picker.sel;

            snprintf(query, sizeof query, "%s", e->picker.query);
            command_run(e, c);
            palette_open(e, query);
            /* same items in the same order: the list looks as it did, only the values change */
            if (sel < e->picker.nshown) {
                e->picker.sel = sel;
                e->picker.top = top;
            }
            return;
        }
    }
    picker_close(&e->picker);
    if (idx < 0)
        return;
    if (kind == PK_BUFFERS) {
        if ((size_t)idx < e->ndocs)
            switch_doc(e, e->docs[idx]);
    } else if (kind == PK_RESULTS) {
        find_open_result(e, (size_t)idx);
    } else if (kind == PK_SAVECFG) {
        save_settings_to(e, (size_t)idx);
    } else if (kind == PK_SYMBOLS) {
        if ((size_t)idx < e->noutline && e->outline[2 * idx] < e->buf->nlines) {
            buf_undo_close(e->buf);
            e->cy = e->outline[2 * idx];
            e->cx = e->outline[2 * idx + 1];
            if (e->cx > e->buf->lines[e->cy].len)
                e->cx = e->buf->lines[e->cy].len;
            e->want_col = editor_cursor_col(e);
            e->sel_on = 0;
        }
    } else if (kind == PK_TAGS) {
        find_open_tag(e, (size_t)idx);
    } else if (kind == PK_COMMANDS) {
        command_run(e, (int)e->pal[idx]);
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
        if (e->picker_kind == PK_COMMANDS && picker_chosen(p) >= 0)
            e->pal_last = e->pal[picker_chosen(p)];     /* the next palette opens here */
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
 * "H30 <a> <b>" (stacked), or a leaf "W<doc>,<line>,<col>,<rowoff>,<coloff>" ("P" for the
 * Markdown preview of that document). */
static void ser_layout(SBuf *b, const Node *n, const int *docmap, const Editor *e)
{
    if (n->win) {
        const View *v = &n->win->view;
        const Doc *md_src = n->win->doc->md ? n->win->doc->md->src : NULL;
        int idx = docmap[doc_index(e, md_src ? md_src : n->win->doc)];

        /* a Markdown preview is written as the preview of its source: P<doc> */
        sbuf_addf(b, "%c%d,%zu,%zu,%zu,%zu ", md_src ? 'P' : 'W', idx < 0 ? 0 : idx, v->cy, v->cx,
                  v->rowoff, v->coloff);
        return;
    }
    sbuf_addf(b, "%c%d ", n->vertical ? 'V' : 'H', (n->frac + 5) / 10);
    ser_layout(b, n->a, docmap, e);
    ser_layout(b, n->b, docmap, e);
}

typedef struct PNode {
    int vertical, pct, leaf, preview;
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
    if (**p == 'W' || **p == 'P') {
        if (sscanf(*p + 1, "%ld,%zu,%zu,%zu,%zu%n", &n->doc, &n->y, &n->x, &n->ro, &n->co,
                   &used) < 5) {
            free(n);
            return NULL;
        }
        n->leaf = 1;
        n->preview = **p == 'P';
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
        if (n->preview && w->doc->syn && strcmp(syn_name(w->doc->syn), "Markdown") == 0)
            w->doc = preview_doc(e, w->doc);
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
        if (!edit_backspace_pair(e))
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
        if (is_printable(k.code) && !edit_type(e, k.code))
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

static void accept_cand(Editor *e)
{
    const Cand *c = &e->comp.v[e->comp.sel];
    char *text = xstrdup(c->text);
    size_t from = e->comp.x, i, ns;
    int snippet = c->kind == CAND_SNIPPET;
    const SnipDef *sd = snip_lang(e->doc->syn ? syn_name(e->doc->syn) : NULL, &ns);

    SBuf body = SBUF_INIT;
    const Line *ln = &e->buf->lines[e->cy];
    int call = 0;
    char *fq = NULL;

    /* a class of the project or the library: accepting it may add its "use" */
    if (c->kind == CAND_CLASS && c->sig && e->doc->syn &&
        strncmp(syn_name(e->doc->syn), "PHP", 3) == 0 &&
        (strchr(c->sig, '\\') || strcmp(c->sig, c->text) == 0))
        fq = xstrdup(c->sig);
    /* a call already there ("foo|(") only gets the name */
    if (!snippet && !(e->cx < ln->len && ln->s[e->cx] == '('))
        call = complete_call_body(c, &body);
    complete_close(&e->comp);
    if (call == 1) {
        SnipDef d = { text, body.p ? body.p : "" };

        sbuf_add(&body, "", 1);
        d.body = body.p;
        edit_expand_snippet(e, from, &d);
        sbuf_free(&body);
        free(text);
        free(fq);
        return;
    }
    if (call == 2) {
        edit_replace_word(e, from, body.p, body.len);
        if (e->cx > 0)
            e->cx--;                    /* between the parentheses */
        sbuf_free(&body);
        free(text);
        free(fq);
        return;
    }
    if (snippet)
        for (i = 0; i < ns; i++)
            if (strcmp(sd[i].name, text) == 0) {
                edit_expand_snippet(e, from, &sd[i]);
                free(text);
                free(fq);
                return;
            }
    edit_replace_class(e, from, text, strlen(text), fq);
    free(fq);
    free(text);
}

/* Accepting inside a snippet session: keep the current stop's length right (a new snippet
 * from the choice replaces the session; a changed line count ends it). */
/* What accepting the highlighted candidate must add to the file, if it is a symbol of the standard
 * library the file does not have yet. */
typedef struct {
    int kind;               /* 0 nothing, 1 C include, 2 Python import, 3 Python from-import,
                               4 JS/TS default import, 5 JS/TS named import, 6 C local include,
                               7 Python import from a module of the project,
                               8 PHP use of the class before ::, 9 PHP require_once,
                               10 PHP use function / use const, 11 Go import, 12 Rust use,
                               13 Java import, 14 C# using */
    char mod[256];          /* the header, or the module */
    char name[64];          /* the imported name (from-import) */
} Needs;

/* True if the document assigns or defines name somewhere in its first lines: then a word before a
 * dot is a variable and not a module. */
static int py_shadowed(const Buffer *b, const char *name)
{
    size_t y, nl = strlen(name), lim = b->nlines < 2000 ? b->nlines : 2000;

    for (y = 0; y < lim; y++) {
        const Line *l = &b->lines[y];
        size_t i = 0;

        while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t'))
            i++;
        if (l->len - i > nl + 2 && memcmp(l->s + i, name, nl) == 0 && l->s[i + nl] == ' ' &&
            l->s[i + nl + 1] == '=' && l->s[i + nl + 2] != '=')
            return 1;
        if (l->len - i > 4 && memcmp(l->s + i, "def ", 4) == 0 && memcmp(l->s + i + 4, name, nl) == 0)
            return 1;
    }
    return 0;
}

static void needs_for(const Editor *e, const Cand *c, Needs *nd)
{
    static const char *const libs[] = { "cpp", "c" };
    const char *lang = e->doc->syn ? syn_name(e->doc->syn) : "";
    const Line *ln = &e->buf->lines[e->cy];
    size_t hl = 0, k;
    int qualified = e->comp.x > 0 && e->comp.x <= ln->len && ln->s[e->comp.x - 1] == '.';

    memset(nd, 0, sizeof *nd);
    if (c->inc && strcmp(lang, "C") == 0) {
        /* a symbol of a header of the project */
        nd->kind = 6;
        snprintf(nd->mod, sizeof nd->mod, "%s", c->inc);
        return;
    }
    if (c->inc && strcmp(lang, "Python") == 0) {
        /* a name a module of the project defines */
        nd->kind = 7;
        snprintf(nd->mod, sizeof nd->mod, "%s", c->inc);
        snprintf(nd->name, sizeof nd->name, "%s", c->text);
        return;
    }
    if (c->inc && (strcmp(lang, "JavaScript") == 0 || strcmp(lang, "TypeScript") == 0)) {
        /* what a module of the project exports; "=" first: its default export */
        int def = c->inc[0] == '=';

        nd->kind = def ? 4 : 5;
        snprintf(nd->mod, sizeof nd->mod, "%s", c->inc + def);
        snprintf(nd->name, sizeof nd->name, "%s", c->text);
        return;
    }
    if (!c->inc && !c->std && qualified && e->comp.sc && (strcmp(lang, "C#") == 0 || strcmp(lang, "Java") == 0)) {
        /* TextUtils.Shout: the class before the dot may be of another namespace or package */
        size_t s = e->comp.x - 1, n;
        char q[64];

        while (s > 0 && (isalnum((unsigned char)ln->s[s - 1]) || ln->s[s - 1] == '_'))
            s--;
        n = e->comp.x - 1 - s;
        if (n > 0 && n < sizeof q && (s == 0 || ln->s[s - 1] != '.') && isupper((unsigned char)ln->s[s])) {
            memcpy(q, ln->s + s, n);
            q[n] = '\0';
            if (sym_project_class(e->comp.sc, (Doc *const *)e->docs, e->ndocs, e->doc, q, nd->mod, sizeof nd->mod)) {
                nd->kind = lang[0] == 'C' ? 14 : 13;
                if (nd->kind == 14 && sym_cs_using_known(e->comp.sc, e->doc, nd->mod))
                    nd->kind = 0;
            }
        }
        if (nd->kind)
            return;
    }
    if (c->inc && strcmp(lang, "C#") == 0) {
        /* a type of another namespace of the project */
        if (!e->comp.sc || !sym_cs_using_known(e->comp.sc, e->doc, c->inc)) {
            nd->kind = 14;
            snprintf(nd->mod, sizeof nd->mod, "%s", c->inc);
        }
        return;
    }
    if (c->inc && strcmp(lang, "Java") == 0) {
        /* a public type of another package of the project */
        nd->kind = 13;
        snprintf(nd->mod, sizeof nd->mod, "%s", c->inc);
        return;
    }
    if (c->inc && strcmp(lang, "Rust") == 0) {
        /* an item of another module of the crate: crate::net::http::Client */
        nd->kind = 12;
        snprintf(nd->mod, sizeof nd->mod, "%s", c->inc);
        return;
    }
    if (c->inc && (strcmp(lang, "PHP") == 0 || strcmp(lang, "PHP/HTML") == 0)) {
        /* "function Ns\\f" / "const Ns\\C": a use; else the path of a file to require */
        if (strncmp(c->inc, "function ", 9) == 0 || strncmp(c->inc, "const ", 6) == 0) {
            size_t w = c->inc[0] == 'f' ? 8 : 5;

            nd->kind = 10;
            snprintf(nd->name, sizeof nd->name, "%.*s", (int)w, c->inc);
            snprintf(nd->mod, sizeof nd->mod, "%s", c->inc + w + 1);
        } else {
            nd->kind = 9;
            snprintf(nd->mod, sizeof nd->mod, "%s", c->inc);
        }
        return;
    }
    if ((strcmp(lang, "PHP") == 0 || strcmp(lang, "PHP/HTML") == 0) && e->comp.x >= 2 &&
        e->comp.x <= ln->len && ln->s[e->comp.x - 1] == ':' && ln->s[e->comp.x - 2] == ':') {
        /* Route::get: the class before :: may not be imported yet */
        size_t s = e->comp.x - 2, n;
        char q[64];

        while (s > 0 && (isalnum((unsigned char)ln->s[s - 1]) || ln->s[s - 1] == '_'))
            s--;
        n = e->comp.x - 2 - s;
        if (n > 0 && n < sizeof q && (s == 0 || (ln->s[s - 1] != '\\' && ln->s[s - 1] != '$')) &&
            isupper((unsigned char)ln->s[s])) {
            memcpy(q, ln->s + s, n);
            q[n] = '\0';
            if (strcmp(q, "self") != 0 && strcmp(q, "static") != 0 && strcmp(q, "parent") != 0 &&
                e->comp.sc && sym_class_fq(e->comp.sc, e->doc, q, nd->mod, sizeof nd->mod) &&
                strchr(nd->mod, '\\'))
                nd->kind = 8;
        }
        return;
    }
    if (!c->std && qualified && strcmp(lang, "Go") == 0 && e->comp.sc) {
        /* store.Open: a package of the module, imported by its path under go.mod */
        size_t s = e->comp.x - 1, n;
        char q[64];

        while (s > 0 && (isalnum((unsigned char)ln->s[s - 1]) || ln->s[s - 1] == '_'))
            s--;
        n = e->comp.x - 1 - s;
        if (n > 0 && n < sizeof q && (s == 0 || ln->s[s - 1] != '.') && isupper((unsigned char)c->text[0])) {
            memcpy(q, ln->s + s, n);
            q[n] = '\0';
            if (sym_go_package(e->comp.sc, e->doc, q, nd->mod, sizeof nd->mod))
                nd->kind = 11;
        }
        return;
    }
    if (!c->std)
        return;
    if (strcmp(lang, "Java") == 0 || strcmp(lang, "C#") == 0) {
        int cs = lang[0] == 'C';
        StdLib *l = std_open(cs ? "cs" : "java");
        const char *h = NULL;

        if (!l)
            return;
        /* Collections.sort: the class before the dot; else the class itself (ArrayList) */
        if (qualified) {
            size_t s = e->comp.x - 1, n;
            char q[64];

            while (s > 0 && (isalnum((unsigned char)ln->s[s - 1]) || ln->s[s - 1] == '_'))
                s--;
            n = e->comp.x - 1 - s;
            if (n > 0 && n < sizeof q && (s == 0 || ln->s[s - 1] != '.') && isupper((unsigned char)ln->s[s])) {
                memcpy(q, ln->s + s, n);
                q[n] = '\0';
                h = std_header(l, q, n, &hl, 1);
            }
        } else if (c->kind == CAND_CLASS) {
            h = std_header(l, c->text, strlen(c->text), &hl, 1);
        }
        if (h && hl > 0 && hl < sizeof nd->mod) {
            nd->kind = cs ? 14 : 13;
            snprintf(nd->mod, sizeof nd->mod, "%.*s", (int)hl, h);
            /* C#: a namespace the project already gives (implicit usings, global using) */
            if (cs && e->comp.sc && sym_cs_using_known(e->comp.sc, e->doc, nd->mod))
                nd->kind = 0;
        }
        std_close(l);
        return;
    }
    if (strcmp(lang, "Go") == 0 || strcmp(lang, "Rust") == 0) {
        int go = lang[0] == 'G';
        StdLib *l = std_open(go ? "go" : "rust");
        const char *h = NULL;
        size_t s, n;
        char q[64];

        if (!l)
            return;
        /* fmt.Println, fs::read_to_string, HashMap::new: the package or module before the
         * operator; else the name itself (a package in Go, a type or module in Rust) */
        if (go ? qualified : (e->comp.x >= 2 && e->comp.x <= ln->len && ln->s[e->comp.x - 1] == ':' &&
                             ln->s[e->comp.x - 2] == ':')) {
            s = e->comp.x - (go ? 1 : 2);
            n = s;
            while (s > 0 && (isalnum((unsigned char)ln->s[s - 1]) || ln->s[s - 1] == '_'))
                s--;
            n -= s;
            if (n > 0 && n < sizeof q && (s == 0 || (ln->s[s - 1] != '.' && ln->s[s - 1] != ':'))) {
                memcpy(q, ln->s + s, n);
                q[n] = '\0';
                h = std_header(l, q, n, &hl, 1);
            }
        } else if (c->kind == CAND_CLASS) {
            h = std_header(l, c->text, strlen(c->text), &hl, 1);
            /* in Go only a package is written alone: its types come after it (strings.Builder) */
            if (h && go) {
                const char *sl;
                size_t tl = strlen(c->text);

                for (sl = h + hl; sl > h && sl[-1] != '/'; sl--)
                    ;
                if ((size_t)(h + hl - sl) != tl || memcmp(sl, c->text, tl) != 0)
                    h = NULL;
            }
        }
        if (h && hl > 0 && hl < sizeof nd->mod) {
            nd->kind = go ? 11 : 12;
            snprintf(nd->mod, sizeof nd->mod, "%.*s", (int)hl, h);
        }
        std_close(l);
        return;
    }
    if (strcmp(lang, "Python") == 0) {
        StdLib *l = std_open("py");
        const char *h;

        if (!l)
            return;
        if (qualified) {
            /* os.getcwd: the word before the dot is a module the file may not import yet */
            size_t s = e->comp.x - 1, n;
            char q[64];

            while (s > 0 && (isalnum((unsigned char)ln->s[s - 1]) || ln->s[s - 1] == '_'))
                s--;
            n = e->comp.x - 1 - s;
            if (n > 0 && n < sizeof q && (s == 0 || ln->s[s - 1] != '.')) {
                memcpy(q, ln->s + s, n);
                q[n] = '\0';
                h = std_header(l, q, n, &hl, 0);
                if (h && hl == n && memcmp(h, q, n) == 0 && !py_shadowed(e->buf, q)) {
                    nd->kind = 2;
                    snprintf(nd->mod, sizeof nd->mod, "%s", q);
                }
            }
        } else if (c->kind == CAND_CLASS || c->kind == CAND_PROP) {
            h = std_header(l, c->text, strlen(c->text), &hl, c->kind == CAND_CLASS);
            if (h && hl > 0 && hl < sizeof nd->mod) {
                snprintf(nd->mod, sizeof nd->mod, "%.*s", (int)hl, h);
                if (c->kind == CAND_CLASS) {
                    nd->kind = 3;
                    snprintf(nd->name, sizeof nd->name, "%s", c->text);
                } else if (strcmp(nd->mod, c->text) == 0) {
                    nd->kind = 2;
                }
            }
        }
        std_close(l);
        return;
    }
    if (strcmp(lang, "JavaScript") == 0 || strcmp(lang, "TypeScript") == 0) {
        StdLib *l = std_open("js");
        const char *h;

        if (!l)
            return;
        if (qualified) {
            /* fs.readFile: the word before the dot is a Node module the file may not import yet */
            size_t s = e->comp.x - 1, n;
            char q[64];

            while (s > 0 && (isalnum((unsigned char)ln->s[s - 1]) || ln->s[s - 1] == '_' || ln->s[s - 1] == '$'))
                s--;
            n = e->comp.x - 1 - s;
            if (n > 0 && n < sizeof q && (s == 0 || ln->s[s - 1] != '.')) {
                memcpy(q, ln->s + s, n);
                q[n] = '\0';
                h = std_header(l, q, n, &hl, 1);
                if (h && hl > 1 && h[0] == '=' && hl - 1 < sizeof nd->mod) {
                    nd->kind = 4;
                    snprintf(nd->mod, sizeof nd->mod, "%.*s", (int)hl - 1, h + 1);
                    snprintf(nd->name, sizeof nd->name, "%s", q);
                }
            }
        } else if (c->kind == CAND_CLASS) {
            h = std_header(l, c->text, strlen(c->text), &hl, 1);
            if (h && hl > 0 && hl < sizeof nd->mod) {
                int def = h[0] == '=';

                snprintf(nd->mod, sizeof nd->mod, "%.*s", (int)hl - def, h + def);
                nd->kind = def ? 4 : 5;
                snprintf(nd->name, sizeof nd->name, "%s", c->text);
            }
        }
        std_close(l);
        return;
    }
    if (strcmp(lang, "C") != 0 ||
        (c->kind != CAND_FUNC && c->kind != CAND_CONST && c->kind != CAND_CLASS && c->kind != CAND_PROP))
        return;
    /* a C++ document: the C++ library first (<cstdio>, <vector>), then the C one */
    for (k = sym_is_cxx(e->doc) ? 0 : 1; k < 2 && hl == 0; k++) {
        StdLib *l = std_open(libs[k]);
        const char *h;

        if (!l)
            continue;
        h = std_header(l, c->text, strlen(c->text), &hl, c->kind == CAND_CLASS);
        if (h && hl > 0 && hl < sizeof nd->mod) {
            memcpy(nd->mod, h, hl);
            nd->mod[hl] = '\0';
            nd->kind = 1;
        } else {
            hl = 0;
        }
        std_close(l);
    }
}

/* Accepts the highlighted candidate; a symbol of the standard library also gets the #include or
 * the import it needs, in the same undo step. */
static void complete_accept(Editor *e)
{
    Needs nd;
    char what[400];
    int added = 0;

    needs_for(e, &e->comp.v[e->comp.sel], &nd);
    if (nd.kind) {
        buf_undo_begin(e->buf, e->cy, e->cx);
        e->undo_hold++;
        if (nd.kind == 1 || nd.kind == 6) {
            added = imp_include(e, nd.mod, nd.kind == 6);
            snprintf(what, sizeof what, nd.kind == 6 ? "#include \"%s\"" : "#include <%s>", nd.mod);
        } else if (nd.kind == 2) {
            added = imp_python(e, nd.mod, NULL, 0);
            snprintf(what, sizeof what, "import %s", nd.mod);
        } else if (nd.kind == 11) {
            added = imp_go(e, nd.mod);
            snprintf(what, sizeof what, "import \"%s\"", nd.mod);
        } else if (nd.kind == 12) {
            added = imp_rust(e, nd.mod);
            snprintf(what, sizeof what, "use %s;", nd.mod);
        } else if (nd.kind == 13) {
            added = imp_java(e, nd.mod);
            snprintf(what, sizeof what, "import %s;", nd.mod);
        } else if (nd.kind == 14) {
            added = imp_csharp(e, nd.mod);
            snprintf(what, sizeof what, "using %s;", nd.mod);
        } else if (nd.kind == 9) {
            added = imp_php_require(e, nd.mod);
            snprintf(what, sizeof what, "require_once __DIR__ . '/%s';", nd.mod);
        } else if (nd.kind == 10) {
            added = imp_php_use_fn(e, nd.name, nd.mod);
            snprintf(what, sizeof what, "use %s %s;", nd.name, nd.mod);
        } else if (nd.kind == 8) {
            added = imp_php_use(e, nd.mod);
            snprintf(what, sizeof what, "use %s;", nd.mod);
        } else if (nd.kind == 3 || nd.kind == 7) {
            added = imp_python(e, nd.mod, nd.name, nd.kind == 7);
            snprintf(what, sizeof what, "from %s import %s", nd.mod, nd.name);
        } else {
            added = imp_js(e, nd.mod, nd.name, nd.kind == 4, what, sizeof what);
        }
    }
    accept_cand(e);
    if (nd.kind) {
        e->undo_hold--;
        buf_undo_cursor(e->buf, e->cy, e->cx);
        buf_undo_close(e->buf);
        if (added)
            editor_set_msg(e, MSG_TTL_MS, tr("Added %s"), what);
    }
}

static void accept_in_snippet(Editor *e)
{
    SnipSession old = e->snip;
    size_t y = old.on ? old.stop[old.cur].y : 0, nl = e->buf->nlines;
    size_t len = old.on ? e->buf->lines[y].len : 0;

    complete_accept(e);
    if (old.on && e->snip.on && memcmp(&old, &e->snip, sizeof old) == 0)
        snip_adjust(&e->snip, nl, e->buf->nlines, len,
                    y < e->buf->nlines ? e->buf->lines[y].len : 0, e->cy, e->cx);
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
        /* Enter takes the highlighted candidate (the first one unless moved with the arrows) */
        buf_undo_close(e->buf);
        accept_in_snippet(e);
        return 1;
    case KEY_TAB:
        /* inside a snippet Tab keeps moving between placeholders unless a choice was made */
        if (e->snip.on && !e->comp.picked) {
            complete_close(&e->comp);
            return 0;
        }
        buf_undo_close(e->buf);
        accept_in_snippet(e);
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
    /* the end of a member operator (-> :: .) opens the list of members: complete_open decides */
    int op = !(k.mods & (MOD_ALT | MOD_CTRL)) && (k.code == '>' || k.code == ':' || k.code == '.' || k.code == '\\' ||
                                                  /* env(' and $: the names of the .env variables */
                                                  k.code == '\'' || k.code == '"' || k.code == '`' ||
                                                  k.code == '$' || k.code == '{' || k.code == '[' || k.code == '(');

    /* keys already waiting (fast typing, a paste without bracketing) are not worth a scan each */
    if ((word || op || (bs && e->comp.on)) && !edit_sel_active(e) && !e->doc->readonly &&
        !cur_multi(e) && e->doc->autosuggest && !term_has_input()) {
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
            editor_set_msg(e, MSG_TTL_MS, tr("No split in that direction"));
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
    /* the start screen goes at the first key or click; the focus events leave it: many
     * terminals report the focus as soon as xted asks for them, at start */
    if (e->intro && k.code != KEY_FOCUS_IN && k.code != KEY_FOCUS_OUT && k.code != KEY_NONE) {
        e->intro = 0;
        render_invalidate(e->render);
    }
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
    cmd = k.code == KEY_CMD ? (k.mods >= 0 && (size_t)k.mods < ncommands ? k.mods : -1) : keymap_find(&e->km, k);
    mod = cmd >= 0 ? commands[cmd].modifies : modifies(k);
    if (e->doc->readonly && mod) {
        editor_set_msg(e, MSG_TTL_MS, tr("File is read-only (Alt+S to save under another name)"));
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
void editor_cmd_symbols(Editor *e) { cmd_symbols(e); }
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
        editor_set_msg(e, MSG_TTL_MS, tr("No matching bracket"));
    }
}

/* ---- indentation commands ---------------------------------------------- */

/* The user chose the style: it wins over the configuration, .editorconfig and the file. */
static void set_indent(Editor *e, int expand, int width)
{
    Doc *d = e->doc;

    d->man.set = 1;
    d->man.expand = expand;
    d->man.width = width;
    d->man.tabw = 0;
    apply_cfg(e, d);
    render_invalidate(e->render);
    if (expand)
        editor_set_msg(e, MSG_TTL_MS, tr("Indentation: %d spaces"), d->indent_w);
    else
        editor_set_msg(e, MSG_TTL_MS, tr("Indentation: tab (width %d)"), d->tabw);
}

void editor_cmd_indent_tabs(Editor *e) { set_indent(e, 0, 0); }
void editor_cmd_indent_spaces2(Editor *e) { set_indent(e, 1, 2); }
void editor_cmd_indent_spaces4(Editor *e) { set_indent(e, 1, 4); }
void editor_cmd_indent_spaces8(Editor *e) { set_indent(e, 1, 8); }

/* Spaces to tab characters, or the opposite, keeping how the text looks: the tab is drawn as
 * wide as one level of the old indentation. */
static void convert_indent(Editor *e, int to_tabs)
{
    Doc *d = e->doc;
    int level = d->expand_tab ? d->indent_w : d->tabw;
    size_t n = edit_convert_indent(e, to_tabs, (size_t)level);

    d->man.set = 1;
    d->man.expand = !to_tabs;
    d->man.width = to_tabs ? 0 : level;
    d->man.tabw = to_tabs ? level : d->tabw;
    apply_cfg(e, d);
    render_invalidate(e->render);
    editor_set_msg(e, MSG_TTL_MS, to_tabs ? tr("%zu lines: indentation with tabs")
                                          : tr("%zu lines: indentation with spaces"), n);
}

void editor_cmd_indent_to_tabs(Editor *e) { convert_indent(e, 1); }
void editor_cmd_indent_to_spaces(Editor *e) { convert_indent(e, 0); }

void editor_cmd_indent_detect(Editor *e)
{
    Doc *d = e->doc;

    memset(&d->man, 0, sizeof d->man);
    if (detect_indent(d)) {
        apply_cfg(e, d);
        editor_set_msg(e, MSG_TTL_MS, d->expand_tab ? tr("Detected indentation: %d spaces")
                                                    : tr("Detected indentation: tab (%d)"),
                       d->expand_tab ? d->indent_w : d->tabw);
    } else {
        apply_cfg(e, d);
        editor_set_msg(e, MSG_TTL_MS, tr("Indentation not recognised: the settings apply"));
    }
    render_invalidate(e->render);
}

void editor_cmd_theme(Editor *e)
{
    e->theme_idx++;
    e->theme = theme_get(e->theme_idx);
    e->pend |= PEND_THEME;
    render_invalidate(e->render);
    editor_set_msg(e, MSG_TTL_MS, tr("Theme: %s"), e->theme->name);
}

static const char *colors_name(int mode)
{
    return mode == COLOR_TRUE ? "truecolor" : mode == COLOR_256 ? "256" : mode == COLOR_16 ? "16" :
           mode == COLOR_NONE ? "none" : "auto";
}

void editor_colors_label(const Editor *e, char *out, size_t n)
{
    if (e->colors_mode < 0)
        snprintf(out, n, "%s (%s)", tr("auto"), e->color_depth == COLOR_NONE ? tr("none") : colors_name(e->color_depth));
    else
        snprintf(out, n, "%s", e->colors_mode == COLOR_NONE ? tr("none") : colors_name(e->colors_mode));
}

/* auto -> truecolor -> 256 -> 16 -> none -> auto: what the terminal is told to show. */
void editor_cmd_colors(Editor *e)
{
    static const int order[] = { -1, COLOR_TRUE, COLOR_256, COLOR_16, COLOR_NONE };
    size_t i, n = sizeof order / sizeof order[0];
    char lab[64];

    for (i = 0; i < n && order[i] != e->colors_mode; i++)
        ;
    e->colors_mode = order[(i + 1) % n];
    e->color_depth = e->colors_mode < 0 ? theme_detect_depth() : e->colors_mode;
    e->pend |= PEND_COLORS;
    render_invalidate(e->render);
    editor_colors_label(e, lab, sizeof lab);
    editor_set_msg(e, MSG_TTL_MS, tr("Colors: %s"), lab);
}

const char *editor_glyphs_name(int g)
{
    return g == GLYPHS_ASCII ? "ascii" : g == GLYPHS_WINDOWS ? "windows" : "unicode";
}

/* The symbols of the file browser, the pane borders and the palette: unicode -> windows ->
 * ascii -> unicode. */
void editor_cmd_glyphs(Editor *e)
{
    e->glyphs = (e->glyphs + 1) % 3;
    e->pend |= PEND_GLYPHS;
    render_invalidate(e->render);
    editor_set_msg(e, MSG_TTL_MS, tr("Glyphs: %s"), editor_glyphs_name(e->glyphs));
}

void editor_cmd_redraw(Editor *e)
{
    render_invalidate(e->render);
}

/* Where the language in use comes from, for the palette: the catalog file (with ~ for the home
 * folder, cut at the front if long), or "built in" for English. */
static void lang_label(char *out, size_t n)
{
    const char *path = i18n_path(), *home = getenv("HOME");
    char tmp[PATH_MAX];
    size_t hl = home ? strlen(home) : 0, len;

    if (!path) {
        snprintf(out, n, "%s", tr("built in"));
        return;
    }
    if (hl > 1 && strncmp(path, home, hl) == 0 && path[hl] == '/')
        snprintf(tmp, sizeof tmp, "~%s", path + hl);
    else
        snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len > 44) {
        const char *cut = strchr(tmp + len - 43, '/');

        snprintf(out, n, "\xe2\x80\xa6%.60s", cut ? cut : tmp + len - 43);
    } else {
        snprintf(out, n, "%.60s", tmp);
    }
}

/* Opens the palette with the query typed so far, on the command used last. */
static void palette_open(Editor *e, const char *query)
{
    size_t i, k = 0;
    int g, last = -1;
    char (*val)[24] = xmalloc(ncommands * sizeof *val);
    int *on = xmalloc(ncommands * sizeof *on);

    picker_open(&e->picker, tr("Command"));
    e->picker_kind = PK_COMMANDS;
    free(e->pal);
    e->pal = xmalloc((ncommands + G_COUNT) * sizeof *e->pal);
    e->pal_w1 = e->pal_w2 = e->pal_w3 = 0;
    for (i = 0; i < ncommands; i++) {
        const char *nm = tr(commands[i].name);
        int w = (int)utf8_cols(nm, strlen(nm), strlen(nm), 0);

        char label[100];

        on[i] = command_state(e, (int)i, val[i], sizeof val[i]);
        keymap_label(&e->km, (int)i, label, sizeof label);
        if (commands[i].state != ST_LANG && (int)strlen(label) > e->pal_w3)
            e->pal_w3 = (int)strlen(label);
        if (w > e->pal_w1)
            e->pal_w1 = w;
        w = (int)utf8_cols(val[i], strlen(val[i]), strlen(val[i]), 0);
        if (w > e->pal_w2)
            e->pal_w2 = w;
    }
    /* Item: marker (1 on, 0 off, space none), name, value, shortcut; the last three are cut at
     * the unit separator by the renderer, so they line up in columns. */
    for (i = 0; i < ncommands; i++) {
        char item[260], label[100];

        g = commands[i].group;
        if (g != last) {
            snprintf(item, sizeof item, "%c%s%s%s", PICKER_HEAD,
                     g == G_SETTINGS ? "" : tr("Actions"), g == G_SETTINGS ? "" : e->glyphs == GLYPHS_ASCII ? " - " : " \xc2\xb7 ",
                     tr(command_groups[g]));
            picker_add(&e->picker, item);
            e->pal[k++] = 0;
            last = g;
        }
        if (commands[i].state == ST_LANG)
            lang_label(label, sizeof label);
        else
            keymap_label(&e->km, (int)i, label, sizeof label);
        snprintf(item, sizeof item, "%c%s\x1f%s\x1f%s", on[i] == 1 ? '1' : on[i] == 0 ? '0' : ' ',
                 tr(commands[i].name), val[i], label);
        picker_add(&e->picker, item);
        e->pal[k++] = i;
    }
    free(val);
    free(on);
    snprintf(e->picker.query, sizeof e->picker.query, "%s", query);
    e->picker.qlen = strlen(e->picker.query);
    picker_filter(&e->picker);
    for (i = 0; i < k; i++)
        if (e->pal[i] == e->pal_last && e->picker.items[i][0] != PICKER_HEAD) {
            picker_select_item(&e->picker, i);
            /* in the middle of the window, not on its last row */
            if (e->picker.sel > PICKER_ROWS / 2) {
                size_t top = e->picker.sel - PICKER_ROWS / 2;

                if (top + PICKER_ROWS > e->picker.nshown)
                    top = e->picker.nshown > PICKER_ROWS ? e->picker.nshown - PICKER_ROWS : 0;
                e->picker.top = top;
            }
            break;
        }
}

void editor_cmd_palette(Editor *e)
{
    palette_open(e, "");
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
        editor_set_msg(e, MSG_TTL_MS, tr("No other pane to resize"));
        return;
    }
    e->resize_mode = 1;
    editor_set_msg(e, 0,
                   tr("Resize: arrows move the border (Shift x10)  = equalize  Esc to exit"));
}

void editor_cmd_equalize(Editor *e)
{
    layout_equalize(&e->layout);
}

void editor_cmd_toggle_mouse(Editor *e)
{
    e->mouse_on = !e->mouse_on;
    e->pend |= PEND_MOUSE;
    term_mouse(e->mouse_on);
    editor_set_msg(e, MSG_TTL_MS, e->mouse_on ? tr("Mouse on") : tr("Mouse off"));
}

/* ---- switches and saving the settings ----------------------------------- */

static void apply_all(Editor *e)
{
    size_t i;

    for (i = 0; i < e->ndocs; i++)
        apply_cfg(e, e->docs[i]);
    render_invalidate(e->render);
}

void editor_cmd_toggle_autosuggest(Editor *e)
{
    e->doc->man_sug = e->doc->autosuggest ? 2 : 1;
    apply_cfg(e, e->doc);
    editor_set_msg(e, MSG_TTL_MS, "%s", e->doc->autosuggest ? tr("Autosuggest on") : tr("Autosuggest off"));
}

void editor_cmd_toggle_autopair(Editor *e)
{
    e->doc->man_pair = e->doc->autopair ? 2 : 1;
    apply_cfg(e, e->doc);
    editor_set_msg(e, MSG_TTL_MS, "%s", e->doc->autopair ? tr("Auto-pairs on") : tr("Auto-pairs off"));
}

/* The Git marks for the present e->cfg.git: computed again, or cleared. */
static void git_apply(Editor *e, int relay)
{
    size_t i, k;

    for (i = 0; i < e->ndocs; i++) {
        Doc *d = e->docs[i];

        if (e->cfg.git) {
            e->git_off = 0;
            git_refresh_doc(e, d);
        } else {
            for (k = 0; k < d->buf.nlines; k++)
                d->buf.lines[k].git = GIT_NONE;
            d->gutter = 0;
        }
    }
    if (relay)
        relayout(e);
}

void editor_cmd_toggle_git(Editor *e)
{
    e->cfg.git = e->cfg.git == 0 ? 1 : 0;
    e->pend |= PEND_GIT;
    git_apply(e, 1);
    editor_set_msg(e, MSG_TTL_MS, "%s", e->cfg.git ? tr("Git markers on") : tr("Git markers off"));
}

void editor_cmd_help(Editor *e)
{
    Doc *d = NULL;
    SBuf t = SBUF_INIT;
    size_t i, y = 0, x = 0, kw = 0;
    int g, last = -1;
    char label[100], cfg[PATH_MAX + 16];

    for (i = 0; i < e->ndocs; i++)
        if (e->docs[i]->scratch && e->docs[i]->title && strcmp(e->docs[i]->title, "[Help]") == 0)
            d = e->docs[i];
    if (!d) {
        d = doc_new();
        d->title = xstrdup("[Help]");
        d->scratch = 1;
        d->readonly = 1;
        if (e->ndocs == e->docs_cap) {
            e->docs_cap = e->docs_cap ? e->docs_cap * 2 : 8;
            e->docs = xrealloc(e->docs, e->docs_cap * sizeof *e->docs);
        }
        e->docs[e->ndocs++] = d;
    }
    for (i = 0; i < ncommands; i++) {
        keymap_label(&e->km, (int)i, label, sizeof label);
        if (strlen(label) > kw)
            kw = strlen(label);
    }
    if (kw > 30)
        kw = 30;
    sbuf_addf(&t, tr("xted %s: help\n"), XTED_VERSION);
    sbuf_addf(&t, tr("Copyright (C) %s %s, %s license\n"), XTED_YEAR, XTED_AUTHOR, XTED_LICENSE);
    sbuf_adds(&t, tr("\nF1 shows this page, Ctrl+W closes it, Ctrl+F searches in it. Every key can be\n"
                     "changed in the [keys] section of the configuration (see Files below).\n"));
    for (i = 0; i < ncommands; i++) {
        g = commands[i].group;
        if (g != last) {
            sbuf_addf(&t, "\n%s\n", tr(command_groups[g]));
            last = g;
        }
        keymap_label(&e->km, (int)i, label, sizeof label);
        sbuf_addf(&t, "  %-*s  %s\n", (int)kw, label, tr(commands[i].name));
    }
    sbuf_addf(&t, "\n%s\n", tr("Search bar"));
    sbuf_adds(&t, tr("  Alt+C  match case          Alt+W  whole words      Alt+R  regular expression\n"
                     "  Up, Down  previous and next match     Enter  keep it     Esc  go back\n"
                     "  Replacing: y replaces, n skips, a replaces all the others, Esc stops.\n"));
    sbuf_addf(&t, "\n%s\n", tr("Lists (palette, buffers, symbols, files)"));
    sbuf_adds(&t, tr("  Type to filter; Up, Down, PgUp, PgDn move; Enter chooses; Esc closes.\n"
                     "  In the palette Enter on a setting changes it and leaves the palette open.\n"));
    sbuf_addf(&t, "\n%s\n", tr("Suggestions"));
    sbuf_adds(&t, tr("  They appear while typing: Up and Down choose, Tab or Enter accept, Esc closes.\n"
                     "  Accepting a name of a library or of another file of the project also adds its\n"
                     "  #include, import, use or require. In a snippet Tab goes to the next placeholder.\n"));
    sbuf_addf(&t, "\n%s\n", tr("Mouse"));
    sbuf_adds(&t, tr("  Click moves the cursor, a double click selects a word, a triple click a line;\n"
                     "  dragging selects; Alt+click adds a cursor; the wheel scrolls; dragging a border\n"
                     "  resizes the panes.\n"));
    sbuf_addf(&t, "\n%s\n", tr("Files"));
    snprintf(cfg, sizeof cfg, "%s/config", e->cfgdir ? e->cfgdir : "~/.config/xted");
    sbuf_addf(&t, tr("  Configuration: %s (and .xted in a project)\n"), cfg);
    sbuf_addf(&t, tr("  Language: %s, %s\n"), i18n_name(), i18n_path() ? i18n_path() : tr("built in"));
    sbuf_adds(&t, tr("  Manual: man xted\n"));
    /* the page is made again each time: the keys or the language may have changed */
    if (d->buf.nlines > 1 || d->buf.lines[0].len > 0)
        buf_delete_range(&d->buf, 0, 0, d->buf.nlines - 1, d->buf.lines[d->buf.nlines - 1].len);
    buf_insert_text(&d->buf, &y, &x, t.p, t.len);
    sbuf_free(&t);
    undo_free(&d->buf.undo);
    d->buf.modified = 0;
    editor_activate(e, d);
    e->cy = e->cx = 0;
    e->rowoff = 0;
    render_invalidate(e->render);
}

/* A read-only page of its own (Ctrl+W closes it): the one called title, made again with text. */
static void info_page(Editor *e, const char *title, const char *text)
{
    Doc *d = NULL;
    size_t i, y = 0, x = 0;

    for (i = 0; i < e->ndocs; i++)
        if (e->docs[i]->scratch && e->docs[i]->title && strcmp(e->docs[i]->title, title) == 0)
            d = e->docs[i];
    if (!d) {
        d = doc_new();
        d->title = xstrdup(title);
        d->scratch = 1;
        d->readonly = 1;
        if (e->ndocs == e->docs_cap) {
            e->docs_cap = e->docs_cap ? e->docs_cap * 2 : 8;
            e->docs = xrealloc(e->docs, e->docs_cap * sizeof *e->docs);
        }
        e->docs[e->ndocs++] = d;
    }
    if (d->buf.nlines > 1 || d->buf.lines[0].len > 0)
        buf_delete_range(&d->buf, 0, 0, d->buf.nlines - 1, d->buf.lines[d->buf.nlines - 1].len);
    buf_insert_text(&d->buf, &y, &x, text, strlen(text));
    undo_free(&d->buf.undo);
    d->buf.modified = 0;
    editor_activate(e, d);
    e->cy = e->cx = 0;
    e->rowoff = 0;
    render_invalidate(e->render);
}

void editor_cmd_about(Editor *e)
{
    SBuf t = SBUF_INIT;

    sbuf_addf(&t, "xted %s\n\n", XTED_VERSION);
    sbuf_adds(&t, tr("A text editor for the terminal, written in C99 with only the POSIX C library.\n\n"));
    sbuf_addf(&t, tr("Copyright (C) %s %s\n"), XTED_YEAR, XTED_AUTHOR);
    sbuf_addf(&t, tr("%s license: free to use, copy, change and distribute, keeping this notice.\n"),
              XTED_LICENSE);
    sbuf_adds(&t, tr("\nCtrl+W closes this page, F1 shows the help.\n"));
    sbuf_add(&t, "", 1);
    info_page(e, "[About]", t.p);
    sbuf_free(&t);
}

void editor_cmd_language(Editor *e)
{
    char codes[16][16];
    size_t n = i18n_available(codes, 16), i, cur = 0;

    for (i = 0; i < n; i++)
        if (strcmp(codes[i], i18n_code()) == 0)
            cur = i;
    i18n_set(codes[(cur + 1) % n]);
    e->pend |= PEND_LANG;
    render_invalidate(e->render);
    editor_set_msg(e, MSG_TTL_MS, tr("Language: %s"), i18n_name());
}

void editor_cmd_toggle_clipboard(Editor *e)
{
    e->clipboard_osc52 = !e->clipboard_osc52;
    e->pend |= PEND_CLIP;
    editor_set_msg(e, MSG_TTL_MS, "%s", e->clipboard_osc52 ? tr("System clipboard on") : tr("System clipboard off"));
}

/* The global settings of the project's .xted: they win over the user's file; with live they also
 * change the editor now (at start), else only e->cfg (after the user's file was read again). */
static void project_globals(Editor *e, int live)
{
    Config c;

    if (!e->proj)
        return;
    config_init(&c);
    config_load_file(&c, e->proj);
    if (c.clipboard >= 0) {
        e->cfg.clipboard = c.clipboard;
        if (live)
            e->clipboard_osc52 = c.clipboard;
    }
    if (c.mouse >= 0) {
        e->cfg.mouse = c.mouse;
        if (live)
            e->mouse_on = c.mouse;
    }
    if (c.colors >= 0) {
        e->cfg.colors = c.colors;
        if (live) {
            e->colors_mode = c.colors;
            e->color_depth = c.colors;
        }
    }
    if (c.glyphs >= 0) {
        e->cfg.glyphs = c.glyphs;
        if (live)
            e->glyphs = c.glyphs;
    }
    if (c.git >= 0) {
        int changed = c.git != (e->cfg.git != 0);

        e->cfg.git = c.git;
        if (live && changed)
            git_apply(e, 0);
    }
    if (c.language[0]) {
        snprintf(e->cfg.language, sizeof e->cfg.language, "%s", c.language);
        if (live)
            i18n_set(c.language);
    }
    if (c.theme[0]) {
        int t = theme_by_name(c.theme);

        if (t >= 0) {
            snprintf(e->cfg.theme, sizeof e->cfg.theme, "%s", c.theme);
            if (live) {
                e->theme_idx = (size_t)t;
                e->theme = theme_get((size_t)t);
            }
        } else {
            char msg[100];

            snprintf(msg, sizeof msg, tr("unknown theme «%s» in .xted"), c.theme);
            config_error(&e->cfg, 0, msg);
        }
    }
    config_free(&c);
}

void editor_load_project(Editor *e)
{
    const Doc *d = e->doc;

    free(e->proj);
    e->proj = cfgsave_find_project(d && d->buf.path && !d->scratch ? d->buf.path : "./.");
    project_globals(e, 1);
}

/* What was changed with commands and could be saved: the keys of the document's language (with the
 * CFG_* bits they stand for) and the global ones, and the same keys of the other open documents
 * (one per language, a language the active document has changes for left out). */
typedef struct {
    Doc *d;
    const char *lang;
    CfgPair lp[6];
    char val[6][16];
    size_t nl;
} OtherLang;

typedef struct {
    CfgPair lp[6], gp[7];
    char val[6][16];
    size_t nl, ng;
    unsigned mask;
    OtherLang more[6];
    size_t nmore;
    unsigned omask;         /* CFG_* of all the other documents */
} Pending;

static void add_lang(CfgPair *lp, char (*val)[16], size_t *n, unsigned *mask, const char *key,
                     const char *v, unsigned bit)
{
    snprintf(val[*n], sizeof val[0], "%s", v);
    lp[*n].key = key;
    lp[*n].value = val[*n];
    (*n)++;
    *mask |= bit;
}

/* The settings of one document that a command changed. */
static void doc_pending(const Doc *d, CfgPair *lp, char (*val)[16], size_t *nl, unsigned *mask)
{
    char num[16];

    if (d->man.set) {
        add_lang(lp, val, nl, mask, "expand_tab", d->man.expand ? "true" : "false", CFG_EXPAND_TAB);
        snprintf(num, sizeof num, "%d", d->man.expand ? d->man.width : d->man.tabw);
        if (d->man.expand && d->man.width > 0)
            add_lang(lp, val, nl, mask, "indent_width", num, CFG_INDENT_WIDTH);
        else if (!d->man.expand && d->man.tabw > 0)
            add_lang(lp, val, nl, mask, "tab_width", num, CFG_TAB_WIDTH);
    }
    if (d->man_sug)
        add_lang(lp, val, nl, mask, "autosuggest", d->man_sug == 1 ? "true" : "false", CFG_AUTOSUGGEST);
    if (d->man_pair)
        add_lang(lp, val, nl, mask, "autopair", d->man_pair == 1 ? "true" : "false", CFG_AUTOPAIR);
}

static void pending_get(const Editor *e, Pending *p)
{
    const char *mine = e->doc->syn ? syn_name(e->doc->syn) : NULL;
    size_t i, k;

    memset(p, 0, sizeof *p);
    doc_pending(e->doc, p->lp, p->val, &p->nl, &p->mask);
    if (e->pend & PEND_THEME) {
        p->gp[p->ng].key = "theme";
        p->gp[p->ng++].value = e->theme->name;
    }
    if (e->pend & PEND_MOUSE) {
        p->gp[p->ng].key = "mouse";
        p->gp[p->ng++].value = e->mouse_on ? "true" : "false";
    }
    if (e->pend & PEND_GIT) {
        p->gp[p->ng].key = "git";
        p->gp[p->ng++].value = e->cfg.git ? "true" : "false";
    }
    if (e->pend & PEND_CLIP) {
        p->gp[p->ng].key = "clipboard";
        p->gp[p->ng++].value = e->clipboard_osc52 ? "true" : "false";
    }
    if (e->pend & PEND_LANG) {
        p->gp[p->ng].key = "language";
        p->gp[p->ng++].value = i18n_code();
    }
    if (e->pend & PEND_COLORS) {
        p->gp[p->ng].key = "colors";
        p->gp[p->ng++].value = colors_name(e->colors_mode);
    }
    if (e->pend & PEND_GLYPHS) {
        p->gp[p->ng].key = "glyphs";
        p->gp[p->ng++].value = editor_glyphs_name(e->glyphs);
    }
    for (i = 0; i < e->ndocs && p->nmore < 6; i++) {
        Doc *o = e->docs[i];
        const char *lg = o->syn ? syn_name(o->syn) : NULL;
        OtherLang *m = &p->more[p->nmore];
        unsigned mk = 0;
        int dup = 0;

        if (o == e->doc || o->scratch || !lg || (p->nl > 0 && mine && strcmp(lg, mine) == 0))
            continue;
        for (k = 0; k < p->nmore; k++)
            if (strcmp(p->more[k].lang, lg) == 0)
                dup = 1;
        if (dup)
            continue;
        m->nl = 0;
        doc_pending(o, m->lp, m->val, &m->nl, &mk);
        if (m->nl == 0)
            continue;
        m->d = o;
        m->lang = lg;
        p->omask |= mk;
        p->nmore++;
    }
}

/* What a command or the other documents changed is saved: forgets the pending state of a document. */
static void doc_saved(Doc *d, int keep_flags)
{
    d->man.set = 0;
    if (!keep_flags)
        d->man_sug = d->man_pair = 0;
}

static void add_dest(Editor *e, int kind, int lang, const char *path, const char *what, const char *note)
{
    char item[PATH_MAX + 160], shown[PATH_MAX + 2];
    const char *home = getenv("HOME"), *label = kind == DEST_USER ? tr("User") :
                       kind == DEST_PROJECT ? tr("Project") : ".editorconfig";
    size_t hl = home ? strlen(home) : 0;

    if (hl > 1 && strncmp(path, home, hl) == 0 && path[hl] == '/')
        snprintf(shown, sizeof shown, "~%s", path + hl);
    else
        snprintf(shown, sizeof shown, "%s", path);
    /* label, scope, note and path, cut at the unit separator by the renderer into columns (the
     * path last: it is the part that may be cut) */
    if (note[0] == ' ')
        note++;
    snprintf(item, sizeof item, "%s\x1f%s\x1f%s\x1f%s", label, what, note, shown);
    {
        int w1 = (int)strlen(label), w2 = (int)strlen(what), w3 = (int)strlen(note);

        e->pal_w1 = w1 > e->pal_w1 ? w1 : e->pal_w1;
        e->pal_w2 = w2 > e->pal_w2 ? w2 : e->pal_w2;
        e->pal_w3 = w3 > e->pal_w3 ? w3 : e->pal_w3;
    }
    picker_add(&e->picker, item);
    e->dests = xrealloc(e->dests, (e->ndests + 1) * sizeof *e->dests);
    e->dests[e->ndests].kind = kind;
    e->dests[e->ndests].lang = lang;
    e->ndests++;
}

/* The file a .xted of the document's project is (or would be) at; a document without a file
 * counts as being in the current folder. */
static char *project_file(const Doc *d)
{
    const char *path = d->buf.path && !d->scratch ? d->buf.path : "./.";
    char *f = cfgsave_find_project(path), *root;
    size_t n;

    if (f)
        return f;
    root = cfgsave_project_root(path);
    n = strlen(root) + 8;
    f = xmalloc(n);
    snprintf(f, n, "%s/.xted", root);
    free(root);
    return f;
}

/* 1 if the project's .xted already sets one of the global keys about to be saved. */
static int proj_sets_globals(const Editor *e, const Pending *p)
{
    Config c;
    size_t i;
    int r = 0;

    if (!e->proj || !p->ng)
        return 0;
    config_init(&c);
    config_load_file(&c, e->proj);
    for (i = 0; i < p->ng; i++) {
        const char *k = p->gp[i].key;

        if ((strcmp(k, "theme") == 0 && c.theme[0]) || (strcmp(k, "mouse") == 0 && c.mouse >= 0) ||
            (strcmp(k, "git") == 0 && c.git >= 0) || (strcmp(k, "clipboard") == 0 && c.clipboard >= 0) ||
            (strcmp(k, "language") == 0 && c.language[0]) || (strcmp(k, "colors") == 0 && c.colors >= 0) ||
            (strcmp(k, "glyphs") == 0 && c.glyphs >= 0))
            r = 1;
    }
    config_free(&c);
    return r;
}

/* " + PHP, JS": the languages of the other documents that have changes. */
static void others_note(const Pending *p, char *out, size_t n)
{
    size_t i;

    out[0] = '\0';
    for (i = 0; i < p->nmore; i++)
        snprintf(out + strlen(out), n - strlen(out), "%s%s", i ? ", " : " + ", p->more[i].lang);
}

void editor_cmd_save_settings(Editor *e)
{
    Pending p;
    const Doc *d = e->doc;
    const char *lang = d->syn ? syn_name(d->syn) : NULL;
    const unsigned ec_bits = CFG_TAB_WIDTH | CFG_EXPAND_TAB | CFG_INDENT_WIDTH;
    unsigned u_set = config_lang(&e->cfg, lang).set, p_set = 0;
    int has_path = !d->scratch && d->buf.path != NULL, scope, nscope, over;
    char what[80], note[200], more[80], keys[200] = "", *f;
    const char *ecpath = NULL;
    size_t i;

    pending_get(e, &p);
    if (p.nl == 0 && p.ng == 0 && p.nmore == 0) {
        editor_set_msg(e, MSG_TTL_MS, tr("Nothing to save: change something first (indentation, theme, switches)"));
        return;
    }
    if (has_path) {
        LangCfg pl;

        if (project_lang(d, &pl))
            p_set = pl.set;
    }
    others_note(&p, more, sizeof more);
    over = (p.nl > 0 && (p_set & p.mask)) || proj_sets_globals(e, &p);
    free(e->dests);
    e->dests = NULL;
    e->ndests = 0;
    picker_open(&e->picker, tr("Save where"));
    e->picker_kind = PK_SAVECFG;
    e->pal_w1 = e->pal_w2 = e->pal_w3 = 0;
    nscope = p.nl > 0 && lang ? 2 : 1;
    if (e->cfgdir) {
        size_t n = strlen(e->cfgdir) + 8;

        f = xmalloc(n);
        snprintf(f, n, "%s/config", e->cfgdir);
        for (scope = 0; scope < nscope; scope++) {
            what[0] = '\0';
            if (p.nl > 0)
                snprintf(what, sizeof what, scope ? tr("only %s") : tr("all languages"), lang);
            snprintf(note, sizeof note, "%s%s", over ? tr(" (the project's .xted wins)") : "", more);
            add_dest(e, DEST_USER, scope, f, what, note);
        }
        free(f);
    }
    f = project_file(d);
    for (scope = 0; scope < nscope; scope++) {
        what[0] = '\0';
        if (p.nl > 0)
            snprintf(what, sizeof what, scope ? tr("only %s") : tr("all languages"), lang);
        add_dest(e, DEST_PROJECT, scope, f, what, more);
    }
    free(f);
    if (has_path && (p.mask & ec_bits))
        ecpath = d->buf.path;
    for (i = 0; !ecpath && i < p.nmore; i++)
        if (p.more[i].d->buf.path && (p.omask & ec_bits))
            ecpath = p.more[i].d->buf.path;
    if (ecpath) {
        f = cfgsave_editorconfig_path(ecpath);
        if (has_path && ((u_set | p_set) & p.mask & ec_bits))
            snprintf(note, sizeof note, tr(" (ignored: %s wins)"),
                     p_set & p.mask & ec_bits ? tr("the project's .xted") : tr("the user file"));
        else
            snprintf(note, sizeof note, "%s%s", (p.mask & ~ec_bits) || p.ng ? tr(" (indentation only)") : "",
                     more);
        add_dest(e, DEST_EDITORCONFIG, 0, f, "", note);
        free(f);
    }
    for (i = 0; i < p.nl; i++)
        snprintf(keys + strlen(keys), sizeof keys - strlen(keys), "%s%s", i ? ", " : "", p.lp[i].key);
    for (i = 0; i < p.ng; i++)
        snprintf(keys + strlen(keys), sizeof keys - strlen(keys), "%s%s", p.nl || i ? ", " : "",
                 p.gp[i].key);
    if (p.nmore)
        snprintf(keys + strlen(keys), sizeof keys - strlen(keys), "%s%s", keys[0] ? "," : "", more);
    if (e->ndests == 0) {
        picker_close(&e->picker);
        editor_set_msg(e, MSG_TTL_MS, tr("No destination available for these settings"));
        return;
    }
    picker_filter(&e->picker);
    editor_set_msg(e, MSG_TTL_MS, tr("To save: %s"), keys);
}

/* The language sections of the other documents, then the global keys, in a file of our own format. */
static int write_more(const Pending *p, const char *file, char *err, size_t errn)
{
    size_t i;
    int r = 0;

    for (i = 0; r == 0 && i < p->nmore; i++)
        r = cfgsave_write(file, p->more[i].lang, p->more[i].lp, p->more[i].nl, err, errn);
    return r;
}

static void save_settings_to(Editor *e, size_t idx)
{
    Pending p;
    Doc *d = e->doc;
    const SaveDest *dst;
    const char *lang = d->syn ? syn_name(d->syn) : NULL;
    char err[240], file[PATH_MAX + 32], gfile[PATH_MAX + 32], *f = NULL, keys[200] = "";
    int r = 0, git_now = e->cfg.git;
    size_t i;

    if (idx >= e->ndests)
        return;
    dst = &e->dests[idx];
    pending_get(e, &p);
    file[0] = gfile[0] = '\0';
    err[0] = '\0';
    if (dst->kind == DEST_USER) {
        if (!e->cfgdir) {
            editor_set_msg(e, MSG_TTL_MS, tr("No configuration folder (HOME is not set)"));
            return;
        }
        snprintf(file, sizeof file, "%s/config", e->cfgdir);
        if (p.nl)
            r = cfgsave_write(file, dst->lang ? lang : NULL, p.lp, p.nl, err, sizeof err);
        if (r == 0)
            r = write_more(&p, file, err, sizeof err);
        if (r == 0 && p.ng)
            r = cfgsave_write(file, NULL, p.gp, p.ng, err, sizeof err);
    } else if (dst->kind == DEST_PROJECT) {
        f = project_file(d);
        snprintf(file, sizeof file, "%s", f);
        free(f);
        /* the global keys go to the .xted read at start, if there is one */
        snprintf(gfile, sizeof gfile, "%s", e->proj ? e->proj : file);
        if (p.nl)
            r = cfgsave_write(file, dst->lang ? lang : NULL, p.lp, p.nl, err, sizeof err);
        if (r == 0)
            r = write_more(&p, file, err, sizeof err);
        if (r == 0 && p.ng)
            r = cfgsave_write(gfile, NULL, p.gp, p.ng, err, sizeof err);
    } else {
        int written = 0, k;
        char of[PATH_MAX + 32];

        if (d->buf.path && !d->scratch && p.nl) {
            k = cfgsave_editorconfig(d->buf.path, p.lp, p.nl, file, sizeof file, err, sizeof err);
            r = k < 0 ? -1 : 0;
            written += k > 0 ? k : 0;
        }
        for (i = 0; r == 0 && i < p.nmore; i++) {
            if (!p.more[i].d->buf.path)
                continue;
            k = cfgsave_editorconfig(p.more[i].d->buf.path, p.more[i].lp, p.more[i].nl, of, sizeof of,
                                     err, sizeof err);
            r = k < 0 ? -1 : 0;
            if (k > 0) {
                written += k;
                if (!file[0])
                    snprintf(file, sizeof file, "%s", of);
            }
        }
        if (r == 0 && written == 0) {
            editor_set_msg(e, MSG_TTL_MS, tr(".editorconfig cannot hold these settings"));
            return;
        }
    }
    if (r < 0) {
        editor_set_msg(e, MSG_TTL_MS, "%s", err);
        return;
    }
    for (i = 0; i < p.nl; i++)
        if (dst->kind != DEST_EDITORCONFIG || strcmp(p.lp[i].key, "autosuggest") != 0)
            snprintf(keys + strlen(keys), sizeof keys - strlen(keys), "%s%s", keys[0] ? ", " : "",
                     p.lp[i].key);
    doc_saved(d, dst->kind == DEST_EDITORCONFIG);
    for (i = 0; i < p.nmore; i++)
        doc_saved(p.more[i].d, dst->kind == DEST_EDITORCONFIG);
    if (p.nmore) {
        char more[80];

        others_note(&p, more, sizeof more);
        snprintf(keys + strlen(keys), sizeof keys - strlen(keys), "%s", more);
    }
    if (dst->kind == DEST_USER || dst->kind == DEST_PROJECT) {
        for (i = 0; i < p.ng; i++)
            snprintf(keys + strlen(keys), sizeof keys - strlen(keys), "%s%s", keys[0] ? ", " : "",
                     p.gp[i].key);
        if (p.ng)
            e->pend = 0;
    }
    if (dst->kind == DEST_PROJECT && p.ng) {
        free(e->proj);
        e->proj = xstrdup(gfile);
    }
    if (dst->kind == DEST_USER && e->cfgdir) {
        /* the file was written again: read it, keep what the session shows and the project says */
        config_free(&e->cfg);
        config_init(&e->cfg);
        config_load(&e->cfg, e->cfgdir);
        project_globals(e, 0);
        e->cfg.git = git_now;
    }
    apply_all(e);
    editor_set_msg(e, MSG_TTL_MS, tr("Saved in %s: %s"), file, keys);
}

void editor_run(Editor *e)
{
    resize(e);
    term_mouse(e->mouse_on);

    while (!e->quit) {
        Key k;

        if (next_key(e, &k) < 0)
            break;
        handle_key(e, k);
    }
}
