#include "run.h"
#include "i18n.h"
#include "errors.h"
#include "files.h"
#include "utf8.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* ---- the output document ---------------------------------------------------- */

static Doc *out_doc(Editor *e)
{
    Run *r = &e->run;

    if (!r->out) {
        Doc *d = doc_new();

        d->title = xstrdup("[Output]");
        d->scratch = 1;
        d->readonly = 1;
        if (e->ndocs == e->docs_cap) {
            e->docs_cap = e->docs_cap ? e->docs_cap * 2 : 8;
            e->docs = xrealloc(e->docs, e->docs_cap * sizeof *e->docs);
        }
        e->docs[e->ndocs++] = d;
        r->out = d;
    }
    return r->out;
}

/* The panes that show the output. */
static size_t out_wins(Editor *e, Win **w, size_t max)
{
    Win *all[64];
    size_t n = layout_wins(&e->layout, all, 64), i, k = 0;

    for (i = 0; i < n && i < 64; i++)
        if (all[i]->doc == e->run.out && k < max)
            w[k++] = all[i];
    return k;
}

/* Makes the output pane visible, below the active pane, and leaves the focus where it was. */
static void show_output(Editor *e)
{
    Win *w[64], *prev = e->layout.active;
    Doc *d = out_doc(e);

    if (out_wins(e, w, 64) > 0)
        return;
    if (e->doc == d)
        return;
    if (editor_split_at(e, 0, 70) < 0)
        return;                         /* no room: the output stays reachable as a buffer */
    editor_activate(e, d);
    editor_focus(e, prev);
}

/* Flushes pending text of the current line. */
static void flush(Buffer *b, SBuf *seg)
{
    if (seg->len > 0) {
        buf_insert(b, b->nlines - 1, b->lines[b->nlines - 1].len, seg->p, seg->len);
        seg->len = 0;
    }
}

void run_append(Editor *e, const char *s, size_t n)
{
    Run *r = &e->run;
    Buffer *b;
    SBuf seg = SBUF_INIT;
    Win *w[64];
    size_t nw, i, old_nlines;
    unsigned char c;
    int was_end[64];

    if (!r->out || n == 0)
        return;
    b = &r->out->buf;
    editor_store_view(e);
    old_nlines = b->nlines;
    nw = out_wins(e, w, 64);
    for (i = 0; i < nw; i++)
        was_end[i] = w[i]->view.cy + 1 >= old_nlines;

    for (i = 0; i < n; i++) {
        c = (unsigned char)s[i];
        if (r->bytes >= RUN_MAX_OUTPUT) {
            if (!r->trunc) {
                flush(b, &seg);
                buf_split(b, b->nlines - 1, b->lines[b->nlines - 1].len);
                sbuf_adds(&seg, tr("[output truncated at 5 MB]"));
                flush(b, &seg);
                r->trunc = 1;
            }
            break;
        }
        /* terminal escape sequences are dropped: colours from make or grep would show as junk */
        if (r->ansi == 1) {
            r->ansi = c == '[' ? 2 : 0;
            continue;
        }
        if (r->ansi == 2) {
            if (c >= 0x40 && c <= 0x7E)
                r->ansi = 0;
            continue;
        }
        if (c == 0x1b) {
            r->ansi = 1;
            continue;
        }
        if (c == '\r')
            continue;
        r->bytes++;
        if (c == '\n') {
            flush(b, &seg);
            buf_split(b, b->nlines - 1, b->lines[b->nlines - 1].len);
            continue;
        }
        if ((c < 0x20 && c != '\t') || c == 0x7F)
            c = '?';
        sbuf_add(&seg, (const char *)&c, 1);
    }
    flush(b, &seg);
    sbuf_free(&seg);
    /* the output is not an edit history, and never counts as unsaved work */
    buf_undo_close(b);
    undo_free(&b->undo);
    b->modified = 0;

    for (i = 0; i < nw; i++)
        if (was_end[i]) {
            w[i]->view.cy = b->nlines - 1;
            w[i]->view.cx = b->lines[b->nlines - 1].len;
            w[i]->view.sel_on = 0;
        }
    if (e->layout.active->doc == r->out)
        editor_reload_view(e);
}

static void out_clear(Editor *e)
{
    Run *r = &e->run;
    Win *w[64];
    size_t nw, i;

    editor_store_view(e);
    buf_free(&r->out->buf);
    buf_init(&r->out->buf);
    r->out->buf.stable = 0;
    r->bytes = 0;
    r->trunc = 0;
    r->ansi = 0;
    r->err_line = 0;
    nw = out_wins(e, w, 64);
    for (i = 0; i < nw; i++) {
        memset(&w[i]->view, 0, sizeof w[i]->view);
    }
    r->out->last.cy = r->out->last.cx = r->out->last.rowoff = 0;
    if (e->layout.active->doc == r->out)
        editor_reload_view(e);
}

/* ---- running ------------------------------------------------------------------ */

static void finish(Editor *e)
{
    Run *r = &e->run;
    long ms = now_ms() - r->t0;
    int code = shell_exit_code(&r->job);
    char line[96];

    if (code >= 0)
        snprintf(line, sizeof line, tr("\n[exited with %d, %ld.%ld s]\n"), code, ms / 1000,
                 (ms % 1000) / 100);
    else
        snprintf(line, sizeof line, tr("\n[finished, %ld.%ld s]\n"), ms / 1000, (ms % 1000) / 100);
    r->bytes = 0;
    if (!r->trunc)
        run_append(e, line, strlen(line));
    editor_set_msg(e, MSG_TTL_MS, "%s: %s", r->cmd,
                   code == 0 ? tr("ok") : code < 0 ? tr("finished") : code > 128 ? tr("interrupted") : tr("error"));
    shell_close(&r->job);
    r->on = 0;
}

void run_pump(Editor *e)
{
    Run *r = &e->run;
    char buf[8192];
    int guard = 0;

    if (!r->on)
        return;
    while (r->job.fd >= 0 && guard++ < 64) {
        long n = shell_read(&r->job, buf, sizeof buf);

        if (n < 0)
            return;
        if (n == 0)
            break;
        run_append(e, buf, (size_t)n);
    }
    if (r->job.fd >= 0 && guard >= 64)
        return;                     /* more to read: the loop comes back */
    /* the pipe has ended: wait for the status (normally immediate) */
    if (r->job.fd >= 0) {
        close(r->job.fd);
        r->job.fd = -1;
    }
    if (shell_reap(&r->job))
        finish(e);
}

int run_fd(const Editor *e)
{
    return e->run.on ? e->run.job.fd : -1;
}

int run_timeout(const Editor *e)
{
    /* the output ended but the process has not been collected yet */
    return e->run.on && e->run.job.fd < 0 ? 50 : -1;
}

void run_command(Editor *e, const char *cmd)
{
    Run *r = &e->run;
    char err[200], header[560], mine[sizeof r->cmd];

    /* cmd may be r->cmd itself (run again): copy before touching it */
    snprintf(mine, sizeof mine, "%s", cmd);
    cmd = mine;
    if (r->on) {
        shell_stop(&r->job);
        shell_close(&r->job);
        r->on = 0;
    }
    snprintf(r->cmd, sizeof r->cmd, "%s", cmd);
    out_doc(e);
    out_clear(e);
    show_output(e);
    snprintf(header, sizeof header, "$ %s\n", cmd);
    if (shell_start(&r->job, cmd, e->root, err, sizeof err) < 0) {
        snprintf(header + strlen(header), sizeof header - strlen(header), tr("[not started: %s]\n"),
                 err);
        run_append(e, header, strlen(header));
        editor_set_msg(e, MSG_TTL_MS, tr("Cannot start the command: %s"), err);
        return;
    }
    r->on = 1;
    r->t0 = now_ms();
    run_append(e, header, strlen(header));
    editor_set_msg(e, 0, tr("Running: %s (Shift+F5 stops)"), cmd);
}

void run_prompt(Editor *e)
{
    char text[sizeof e->run.cmd];

    if (e->run.on) {
        int c = editor_ask(e, tr("A command is running. Stop it? (y = yes, n = no)"), "sn");

        if (c != 's')
            return;
    }
    if (editor_prompt(e, tr("Run: "), e->run.cmd, text, sizeof text) == 0)
        return;
    run_command(e, text);
}

void run_again(Editor *e)
{
    if (!e->run.cmd[0]) {
        editor_set_msg(e, MSG_TTL_MS, tr("No command has been run (Alt+R)"));
        return;
    }
    run_command(e, e->run.cmd);
}

void run_stop(Editor *e)
{
    if (!e->run.on) {
        editor_set_msg(e, MSG_TTL_MS, tr("No command is running"));
        return;
    }
    shell_stop(&e->run.job);
    editor_set_msg(e, MSG_TTL_MS, tr("Stop requested"));
}

int run_is_output(const Editor *e, const Doc *d)
{
    return e->run.out == d;
}

void run_doc_removed(Editor *e, Doc *d)
{
    if (e->run.out != d)
        return;
    if (e->run.on) {
        shell_close(&e->run.job);
        e->run.on = 0;
    }
    e->run.out = NULL;
}

void run_kill(Editor *e)
{
    if (e->run.on) {
        shell_close(&e->run.job);
        e->run.on = 0;
    }
}

void run_shutdown(Editor *e)
{
    Win *w[64];
    size_t nw, i;

    if (e->run.on) {
        shell_close(&e->run.job);
        e->run.on = 0;
    }
    if (!e->run.out)
        return;
    nw = out_wins(e, w, 64);
    for (i = 0; i < nw; i++)
        if (layout_count(&e->layout) > 1) {
            editor_focus(e, w[i]);
            layout_close(&e->layout, w[i]);
            editor_reload_view(e);
        }
}

/* ---- errors ------------------------------------------------------------------- */

/* If output line y names an existing file, returns its path (malloc'ed) and position. */
static char *line_error(const Editor *e, size_t y, size_t *ln, size_t *col)
{
    const Line *l = &e->run.out->buf.lines[y];
    size_t po, pl;
    char *rel, *full;
    struct stat st;

    if (y == 0)
        return NULL;                /* the "$ command" line may mention places, but is not an error */
    if (!err_parse(l->s ? l->s : "", l->len, &po, &pl, ln, col))
        return NULL;
    rel = xmalloc(pl + 1);
    memcpy(rel, l->s + po, pl);
    rel[pl] = '\0';
    full = rel[0] == '/' ? xstrdup(rel) : files_join(e->root ? e->root : ".", rel);
    free(rel);
    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        free(full);
        return NULL;
    }
    return full;
}

static void open_error(Editor *e, size_t y)
{
    Win *w[64], *all[64], *target = NULL;
    size_t ln, col, n, i, nw;
    char err[200], *path = line_error(e, y, &ln, &col);

    if (!path)
        return;
    n = layout_wins(&e->layout, all, 64);
    for (i = 0; i < n && i < 64; i++)
        if (all[i]->doc != e->run.out) {
            target = all[i];
            break;
        }
    if (target && target != e->layout.active)
        editor_focus(e, target);
    if (!editor_open_doc(e, path, err, sizeof err)) {
        editor_set_msg(e, MSG_TTL_MS, "%s: %s", path, err);
        free(path);
        return;
    }
    editor_goto(e, ln, col ? col : 1);
    free(path);
    e->run.err_line = y + 1;
    editor_store_view(e);
    nw = out_wins(e, w, 64);
    for (i = 0; i < nw; i++) {
        w[i]->view.cy = y;
        w[i]->view.cx = 0;
        w[i]->view.sel_on = 0;
    }
}

static void step_error(Editor *e, int dir)
{
    Run *r = &e->run;
    const Buffer *b;
    long y;

    if (!r->out || r->out->buf.nlines <= 1) {
        editor_set_msg(e, MSG_TTL_MS, tr("No output to search"));
        return;
    }
    b = &r->out->buf;
    if (dir > 0)
        y = (long)r->err_line;
    else
        y = r->err_line == 0 ? (long)b->nlines - 1 : (long)r->err_line - 2;
    for (; y >= 0 && y < (long)b->nlines; y += dir) {
        size_t ln, col;
        char *p = line_error(e, (size_t)y, &ln, &col);

        if (p) {
            free(p);
            open_error(e, (size_t)y);
            return;
        }
    }
    editor_set_msg(e, MSG_TTL_MS, dir > 0 ? tr("No more errors") : tr("No previous error"));
}

void run_next_error(Editor *e)
{
    step_error(e, 1);
}

void run_prev_error(Editor *e)
{
    step_error(e, -1);
}

int run_enter(Editor *e)
{
    size_t ln, col;
    char *p;

    if (!e->run.out || e->doc != e->run.out)
        return 0;
    p = line_error(e, e->cy, &ln, &col);
    if (!p) {
        editor_set_msg(e, MSG_TTL_MS, tr("No error on this line"));
        return 1;
    }
    free(p);
    open_error(e, e->cy);
    return 1;
}
