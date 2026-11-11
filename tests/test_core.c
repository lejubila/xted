#include "buffer.h"
#include "clip.h"
#include "commands.h"
#include "config.h"
#include "cursors.h"
#include "complete.h"
#include "doc.h"
#include "keymap.h"
#include "errors.h"
#include "edit.h"
#include "files.h"
#include "fold.h"
#include "fuzzy.h"
#include "git.h"
#include "langs.h"
#include "layout.h"
#include "search.h"
#include "picker.h"
#include "run.h"
#include "session.h"
#include "shell.h"
#include "snippet.h"
#include "editor.h"
#include "mouse.h"
#include "syntax.h"
#include "tags.h"
#include "theme.h"
#include "util.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(c) do { \
    if (!(c)) { fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); failures++; } \
} while (0)

static int line_is(const Buffer *b, size_t y, const char *s)
{
    return y < b->nlines && b->lines[y].len == strlen(s) &&
           (b->lines[y].len == 0 || memcmp(b->lines[y].s, s, b->lines[y].len) == 0);
}

static void test_utf8(void)
{
    uint32_t cp;
    char b[4];
    const char *s;

    CHECK(utf8_decode("a", 1, &cp) == 1 && cp == 'a');
    CHECK(utf8_decode("\xC3\xA9", 2, &cp) == 2 && cp == 0xE9);
    CHECK(utf8_decode("\xE2\x82\xAC", 3, &cp) == 3 && cp == 0x20AC);
    CHECK(utf8_decode("\xF0\x9F\x98\x80", 4, &cp) == 4 && cp == 0x1F600);
    /* invalid: lone continuation, overlong, surrogate, truncated */
    CHECK(utf8_decode("\x80", 1, &cp) == 1 && cp == 0xFFFD);
    CHECK(utf8_decode("\xC0\x80", 2, &cp) == 1 && cp == 0xFFFD);
    CHECK(utf8_decode("\xED\xA0\x80", 3, &cp) == 1 && cp == 0xFFFD);
    CHECK(utf8_decode("\xE2\x82", 2, &cp) == 1 && cp == 0xFFFD);

    CHECK(utf8_encode(0x20AC, b) == 3 && memcmp(b, "\xE2\x82\xAC", 3) == 0);
    CHECK(utf8_encode(0x1F600, b) == 4 && memcmp(b, "\xF0\x9F\x98\x80", 4) == 0);

    CHECK(cp_width('a') == 1);
    CHECK(cp_width(0x4E2D) == 2);      /* CJK */
    CHECK(cp_width(0x1F600) == 2);     /* emoji */
    CHECK(cp_width(0x0301) == 0);      /* combining acute */
    CHECK(char_cols('\t', 3, 8) == 5);
    CHECK(char_cols(1, 0, 8) == 2);

    s = "a\xE4\xB8\xAD" "b";            /* a, 中, b */
    CHECK(utf8_cols(s, 5, 4, 8) == 3);
    CHECK(utf8_cols(s, 5, 5, 8) == 4);
    CHECK(utf8_byte_at_col(s, 5, 2, 8) == 1);   /* inside the wide char */
    CHECK(utf8_byte_at_col(s, 5, 3, 8) == 4);
    CHECK(utf8_byte_at_col(s, 5, 9, 8) == 5);
    CHECK(utf8_prev(s, 4) == 1);
    CHECK(utf8_next(s, 5, 1) == 4);

    s = "e\xCC\x81x";                   /* e + combining acute, x */
    CHECK(grapheme_next(s, 4, 0) == 3);
    CHECK(grapheme_prev(s, 4, 3) == 0);
}

static void test_edit(void)
{
    Buffer b;

    buf_init(&b);
    CHECK(b.nlines == 1 && !b.modified);
    buf_insert(&b, 0, 0, "hello", 5);
    buf_insert(&b, 0, 5, " world", 6);
    CHECK(line_is(&b, 0, "hello world") && b.modified);
    buf_split(&b, 0, 5);
    CHECK(b.nlines == 2 && line_is(&b, 0, "hello") && line_is(&b, 1, " world"));
    buf_split(&b, 1, 0);
    CHECK(b.nlines == 3 && line_is(&b, 1, "") && line_is(&b, 2, " world"));
    buf_join(&b, 1);
    CHECK(b.nlines == 2 && line_is(&b, 1, " world"));
    buf_delete(&b, 1, 0, 1);
    CHECK(line_is(&b, 1, "world"));
    buf_join(&b, 0);
    CHECK(b.nlines == 1 && line_is(&b, 0, "helloworld"));
    buf_free(&b);
}

static void write_file(const char *path, const char *data, size_t n)
{
    FILE *f = fopen(path, "wb");

    if (!f || fwrite(data, 1, n, f) != n)
        abort();
    fclose(f);
}

static char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *p = malloc(1 << 16);

    if (!f || !p)
        abort();
    *n = fread(p, 1, 1 << 16, f);
    fclose(f);
    return p;
}

/* Loads data, saves it unchanged, and checks the bytes survive. */
static void roundtrip(const char *path, const char *data, size_t n, int crlf, int final_nl)
{
    Buffer b;
    char err[128], *out;
    size_t outn;

    write_file(path, data, n);
    buf_init(&b);
    CHECK(buf_load(&b, path, err, sizeof err) == 0);
    CHECK(b.crlf == crlf);
    CHECK(b.final_newline == final_nl);
    CHECK(buf_save(&b, NULL, err, sizeof err) == (long)n);
    out = read_file(path, &outn);
    CHECK(outn == n && memcmp(out, data, n) == 0);
    free(out);
    buf_free(&b);
}

static void test_files(void)
{
    char dir[] = "/tmp/ked_test_XXXXXX";
    char path[64], err[128];
    Buffer b;
    struct stat st;
    size_t n;
    char *out;

    if (!mkdtemp(dir))
        abort();
    snprintf(path, sizeof path, "%s/f", dir);

    roundtrip(path, "a\nb\n", 4, 0, 1);
    roundtrip(path, "a\nb", 3, 0, 0);
    roundtrip(path, "a\r\nb\r\n", 6, 1, 1);
    roundtrip(path, "\n", 1, 0, 1);
    roundtrip(path, "\n\n\n", 3, 0, 1);
    roundtrip(path, "", 0, 0, 1);       /* an untouched empty file stays empty */

    /* ...but text typed into it gets the usual final newline */
    write_file(path, "", 0);
    buf_init(&b);
    CHECK(buf_load(&b, path, err, sizeof err) == 0);
    buf_insert(&b, 0, 0, "a", 1);
    CHECK(buf_save(&b, NULL, err, sizeof err) == 2);
    buf_free(&b);

    /* a new buffer saved empty creates an empty file */
    snprintf(path, sizeof path, "%s/new", dir);
    buf_init(&b);
    CHECK(buf_save(&b, path, err, sizeof err) == 0);
    CHECK(stat(path, &st) == 0 && st.st_size == 0);
    buf_free(&b);
    snprintf(path, sizeof path, "%s/f", dir);

    /* mode is preserved across the atomic rename */
    write_file(path, "x\n", 2);
    chmod(path, 0640);
    buf_init(&b);
    CHECK(buf_load(&b, path, err, sizeof err) == 0);
    buf_insert(&b, 0, 1, "y", 1);
    CHECK(buf_save(&b, NULL, err, sizeof err) == 3);
    out = read_file(path, &n);
    CHECK(n == 3 && memcmp(out, "xy\n", 3) == 0);
    CHECK(stat(path, &st) == 0 && (st.st_mode & 07777) == 0640);
    free(out);
    buf_free(&b);

    /* nonexistent path -> new buffer; directories are rejected */
    buf_init(&b);
    snprintf(path, sizeof path, "%s/missing", dir);
    CHECK(buf_load(&b, path, err, sizeof err) == 0 && b.is_new);
    buf_free(&b);
    buf_init(&b);
    CHECK(buf_load(&b, dir, err, sizeof err) < 0);
    buf_free(&b);

    snprintf(path, sizeof path, "rm -rf %s", dir);
    if (system(path) != 0)
        fprintf(stderr, "warning: cleanup of %s failed\n", dir);
}

/* Whole buffer as text, lines joined by '\n'. */
static int buf_is(const Buffer *b, const char *text)
{
    size_t n;
    char *t = buf_get_range(b, 0, 0, b->nlines - 1, b->lines[b->nlines - 1].len, &n);
    int ok = n == strlen(text) && (n == 0 || memcmp(t, text, n) == 0);

    free(t);
    return ok;
}

static void test_undo(void)
{
    Buffer b;
    size_t y, x;

    buf_init(&b);

    /* typing merges into a single operation */
    buf_undo_begin(&b, 0, 0);
    buf_insert(&b, 0, 0, "a", 1);
    buf_insert(&b, 0, 1, "b", 1);
    buf_insert(&b, 0, 2, "c", 1);
    buf_undo_cursor(&b, 0, 3);
    buf_undo_close(&b);
    CHECK(b.undo.ngroups == 1 && b.undo.groups[0].nops == 1);
    CHECK(buf_undo(&b, &y, &x) && buf_is(&b, "") && y == 0 && x == 0 && !b.modified);
    CHECK(buf_redo(&b, &y, &x) && buf_is(&b, "abc") && y == 0 && x == 3 && b.modified);
    CHECK(!buf_redo(&b, &y, &x));

    /* Backspace and Delete runs merge too */
    buf_undo_begin(&b, 0, 3);
    buf_delete(&b, 0, 2, 1);
    buf_delete(&b, 0, 1, 1);
    buf_undo_close(&b);
    CHECK(buf_is(&b, "a") && b.undo.groups[1].nops == 1 && b.undo.groups[1].ops[0].n == 2);
    CHECK(buf_undo(&b, &y, &x) && buf_is(&b, "abc") && x == 3);
    buf_undo_begin(&b, 0, 0);
    buf_delete(&b, 0, 0, 1);
    buf_delete(&b, 0, 0, 1);
    buf_undo_close(&b);
    CHECK(buf_is(&b, "c") && b.undo.groups[1].nops == 1);
    CHECK(buf_undo(&b, &y, &x) && buf_is(&b, "abc"));

    /* split and join are undone exactly */
    buf_undo_begin(&b, 0, 1);
    buf_split(&b, 0, 1);
    buf_undo_close(&b);
    CHECK(b.nlines == 2 && buf_is(&b, "a\nbc"));
    buf_undo_begin(&b, 0, 1);
    buf_join(&b, 0);
    buf_undo_close(&b);
    CHECK(b.nlines == 1 && buf_is(&b, "abc"));
    CHECK(buf_undo(&b, &y, &x) && buf_is(&b, "a\nbc"));
    CHECK(buf_undo(&b, &y, &x) && buf_is(&b, "abc"));
    CHECK(buf_redo(&b, &y, &x) && buf_redo(&b, &y, &x) && buf_is(&b, "abc") && b.nlines == 1);

    /* a new edit after an undo drops the redo list */
    CHECK(buf_undo(&b, &y, &x));
    buf_undo_begin(&b, 0, 0);
    buf_insert(&b, 0, 0, "Z", 1);
    buf_undo_close(&b);
    CHECK(b.undo.cur == b.undo.ngroups);
    CHECK(!buf_redo(&b, &y, &x));

    /* a command that changes nothing leaves no group and keeps the redo list */
    CHECK(buf_undo(&b, &y, &x));
    buf_undo_begin(&b, 0, 0);
    buf_undo_close(&b);
    CHECK(buf_redo(&b, &y, &x));
    buf_free(&b);
}

static void test_undo_saved(void)
{
    char dir[] = "/tmp/ked_test_XXXXXX";
    char path[64], err[128];
    Buffer b;
    size_t y, x;

    if (!mkdtemp(dir))
        abort();
    snprintf(path, sizeof path, "%s/f", dir);
    write_file(path, "one\n", 4);
    buf_init(&b);
    CHECK(buf_load(&b, path, err, sizeof err) == 0);

    buf_undo_begin(&b, 0, 0);
    buf_insert(&b, 0, 0, "A", 1);
    buf_undo_close(&b);
    CHECK(buf_save(&b, NULL, err, sizeof err) == 5 && !b.modified);

    /* editing after a save starts a new group; undoing back to the save clears `modified` */
    buf_undo_begin(&b, 0, 1);
    buf_insert(&b, 0, 1, "B", 1);
    buf_undo_close(&b);
    CHECK(b.modified && b.undo.ngroups == 2);
    CHECK(buf_undo(&b, &y, &x) && !b.modified);
    CHECK(buf_undo(&b, &y, &x) && b.modified);
    CHECK(buf_redo(&b, &y, &x) && !b.modified);

    /* an edit that discards the saved state's redo entry makes it unreachable */
    CHECK(buf_undo(&b, &y, &x) && b.modified);
    buf_undo_begin(&b, 0, 0);
    buf_insert(&b, 0, 0, "C", 1);
    buf_undo_close(&b);
    CHECK(b.undo.saved_at == SIZE_MAX && b.modified);
    CHECK(buf_undo(&b, &y, &x) && b.modified);
    CHECK(buf_redo(&b, &y, &x) && b.modified);
    buf_free(&b);

    snprintf(path, sizeof path, "rm -rf %s", dir);
    if (system(path) != 0)
        fprintf(stderr, "warning: cleanup of %s failed\n", dir);
}

static void test_ranges(void)
{
    Buffer b;
    size_t n, y, x;
    char *t;

    buf_init(&b);
    y = x = 0;
    buf_insert_text(&b, &y, &x, "hello\nworld\nagain", 17);
    CHECK(b.nlines == 3 && y == 2 && x == 5 && buf_is(&b, "hello\nworld\nagain"));
    buf_undo_close(&b);

    t = buf_get_range(&b, 0, 2, 2, 3, &n);
    CHECK(n == 13 && memcmp(t, "llo\nworld\naga", 13) == 0);
    free(t);
    t = buf_get_range(&b, 0, 2, 2, 2, &n);
    CHECK(n == 12 && memcmp(t, "llo\nworld\nag", 12) == 0);
    free(t);

    buf_undo_begin(&b, 0, 0);
    buf_delete_range(&b, 0, 2, 2, 2);
    buf_undo_close(&b);
    CHECK(b.nlines == 1 && buf_is(&b, "heain"));
    CHECK(buf_undo(&b, &y, &x) && buf_is(&b, "hello\nworld\nagain"));
    CHECK(buf_redo(&b, &y, &x) && buf_is(&b, "heain"));
    CHECK(buf_undo(&b, &y, &x));

    /* single-line range */
    buf_undo_begin(&b, 0, 0);
    buf_delete_range(&b, 1, 1, 1, 4);
    buf_undo_close(&b);
    CHECK(buf_is(&b, "hello\nwd\nagain"));
    CHECK(buf_undo(&b, &y, &x));

    /* whole lines: middle, first, last, all */
    buf_undo_begin(&b, 0, 0);
    buf_delete_lines(&b, 1, 1);
    buf_undo_close(&b);
    CHECK(buf_is(&b, "hello\nagain"));
    CHECK(buf_undo(&b, &y, &x) && buf_is(&b, "hello\nworld\nagain"));
    buf_delete_lines(&b, 0, 0);
    CHECK(buf_is(&b, "world\nagain"));
    buf_undo_close(&b);
    CHECK(buf_undo(&b, &y, &x));
    buf_delete_lines(&b, 1, 2);
    CHECK(buf_is(&b, "hello"));
    buf_undo_close(&b);
    CHECK(buf_undo(&b, &y, &x));
    buf_delete_lines(&b, 0, 2);
    CHECK(b.nlines == 1 && buf_is(&b, ""));
    buf_undo_close(&b);
    CHECK(buf_undo(&b, &y, &x) && buf_is(&b, "hello\nworld\nagain"));

    /* inserting whole lines before and after */
    buf_insert_lines(&b, 1, 1, "X\nY", 3);
    CHECK(buf_is(&b, "hello\nworld\nX\nY\nagain"));
    buf_insert_lines(&b, 0, 0, "T", 1);
    CHECK(buf_is(&b, "T\nhello\nworld\nX\nY\nagain"));
    buf_free(&b);
}

static void test_clip(void)
{
    Clip c;
    char out[16];
    SBuf osc = SBUF_INIT;
    int i;

    memset(&c, 0, sizeof c);
    clip_push(&c, strdup("one"), 3, 0);
    clip_push(&c, strdup("two\n"), 4, 1);
    CHECK(c.count == 2 && clip_get(&c, 0)->n == 4 && clip_get(&c, 0)->whole_line);
    CHECK(memcmp(clip_get(&c, 1)->text, "one", 3) == 0 && !clip_get(&c, 2));
    for (i = 0; i < 40; i++)
        clip_push(&c, strdup("x"), 1, 0);
    CHECK(c.count == CLIP_MAX);
    clip_free(&c);

    /* RFC 4648 test vectors */
    CHECK(base64_encode("", 0, out) == 0);
    CHECK(base64_encode("f", 1, out) == 4 && memcmp(out, "Zg==", 4) == 0);
    CHECK(base64_encode("fo", 2, out) == 4 && memcmp(out, "Zm8=", 4) == 0);
    CHECK(base64_encode("foo", 3, out) == 4 && memcmp(out, "Zm9v", 4) == 0);
    CHECK(base64_encode("foob", 4, out) == 8 && memcmp(out, "Zm9vYg==", 8) == 0);
    CHECK(base64_encode("fooba", 5, out) == 8 && memcmp(out, "Zm9vYmE=", 8) == 0);
    CHECK(base64_encode("foobar", 6, out) == 8 && memcmp(out, "Zm9vYmFy", 8) == 0);

    CHECK(clip_osc52(&osc, "foo", 3) == 0);
    CHECK(osc.len == 12 && memcmp(osc.p, "\x1b]52;c;Zm9v\x07", 12) == 0);
    sbuf_free(&osc);
    CHECK(clip_osc52(&osc, "x", OSC52_MAX + 1) == -1 && osc.len == 0);
}

/* An editor with the given text, cursor at the start, no undo history. */
static void ed_load(Editor *e, const char *text)
{
    size_t y = 0, x = 0;

    editor_init(e);
    e->clipboard_osc52 = 0;
    buf_insert_text(e->buf, &y, &x, text, strlen(text));
    buf_undo_close(e->buf);
    undo_free(&e->buf->undo);
    e->buf->modified = 0;
}

static void ed_sel(Editor *e, size_t ay, size_t ax, size_t cy, size_t cx)
{
    e->sel_on = 1;
    e->ay = ay;
    e->ax = ax;
    e->cy = cy;
    e->cx = cx;
}

static void test_edit_cmds(void)
{
    Editor e;
    size_t y0, x0, y1, x1, from, to;
    int nl;

    /* autoindent copies the indentation to the left of the cursor */
    ed_load(&e, "\tfoo");
    e.cx = 4;
    edit_newline(&e);
    CHECK(buf_is(e.buf, "\tfoo\n\t") && e.cy == 1 && e.cx == 1);
    e.cy = 0;
    e.cx = 1;
    edit_newline(&e);                    /* cursor inside the indentation */
    CHECK(buf_is(e.buf, "\t\n\tfoo\n\t") && e.cy == 1 && e.cx == 1);
    editor_free(&e);

    /* typing over a selection replaces it, as one undo step */
    ed_load(&e, "hello");
    ed_sel(&e, 0, 0, 0, 3);
    CHECK(edit_sel_active(&e));
    edit_insert_cp(&e, 'X');
    edit_insert_cp(&e, 'Y');
    CHECK(buf_is(e.buf, "XYlo") && !e.sel_on && e.cx == 2);
    edit_undo(&e);
    CHECK(buf_is(e.buf, "hello") && e.cy == 0 && e.cx == 0);
    edit_redo(&e);
    CHECK(buf_is(e.buf, "XYlo"));
    editor_free(&e);

    /* selection geometry and collapse */
    ed_load(&e, "ab\ncd\nef");
    ed_sel(&e, 2, 1, 0, 1);
    edit_sel_range(&e, &y0, &x0, &y1, &x1);
    CHECK(y0 == 0 && x0 == 1 && y1 == 2 && x1 == 1);
    CHECK(edit_sel_line(&e, 0, &from, &to, &nl) && from == 1 && to == 2 && nl);
    CHECK(edit_sel_line(&e, 1, &from, &to, &nl) && from == 0 && to == 2 && nl);
    CHECK(edit_sel_line(&e, 2, &from, &to, &nl) && from == 0 && to == 1 && !nl);
    edit_sel_collapse(&e, 1);
    CHECK(!e.sel_on && e.cy == 2 && e.cx == 1);
    edit_select_all(&e);
    CHECK(e.ay == 0 && e.ax == 0 && e.cy == 2 && e.cx == 2);
    e.sel_on = 0;
    e.cy = 1;
    edit_select_line(&e);
    CHECK(e.ay == 1 && e.cy == 2 && e.cx == 0);
    editor_free(&e);

    ed_load(&e, "foo bar_baz  qux");
    e.cx = 6;
    edit_select_word(&e);
    CHECK(e.ax == 4 && e.cx == 11);
    editor_free(&e);

    /* copy/cut/paste, with and without a selection */
    ed_load(&e, "a\nb\nc");
    e.cy = 1;
    edit_copy(&e);
    CHECK(e.clip.count == 1 && e.clip.e[0].whole_line && e.clip.e[0].n == 2);
    e.cy = 2;
    edit_paste(&e);                     /* whole line goes above the cursor line */
    CHECK(buf_is(e.buf, "a\nb\nb\nc") && e.cy == 3 && e.cx == 0);
    edit_undo(&e);
    CHECK(buf_is(e.buf, "a\nb\nc"));
    e.cy = 0;
    edit_cut(&e);
    CHECK(buf_is(e.buf, "b\nc") && e.cy == 0 && e.clip.count == 2);
    edit_undo(&e);
    CHECK(buf_is(e.buf, "a\nb\nc"));
    ed_sel(&e, 0, 0, 1, 1);
    edit_cut(&e);
    CHECK(buf_is(e.buf, "\nc") && !e.clip.e[0].whole_line && e.clip.e[0].n == 3);
    e.cy = 1;
    e.cx = 1;
    edit_paste(&e);
    CHECK(buf_is(e.buf, "\nca\nb") && e.cy == 2 && e.cx == 1);
    editor_free(&e);

    /* Alt+V walks back through the kill ring */
    ed_load(&e, "x");
    clip_push(&e.clip, strdup("old"), 3, 0);
    clip_push(&e.clip, strdup("new"), 3, 0);
    e.cx = 1;
    edit_paste(&e);
    CHECK(buf_is(e.buf, "xnew"));
    edit_paste_older(&e);
    CHECK(buf_is(e.buf, "xold"));
    edit_paste_older(&e);
    CHECK(buf_is(e.buf, "xnew"));
    edit_undo(&e);
    CHECK(buf_is(e.buf, "xold"));
    editor_free(&e);

    /* bracketed paste: no autoindent, \r\n normalised, controls dropped */
    ed_load(&e, "\tq");
    e.cx = 2;
    edit_paste_text(&e, "a\r\nb\rc\x01\td", 9);
    CHECK(buf_is(e.buf, "\tqa\nb\nc\td"));
    edit_undo(&e);
    CHECK(buf_is(e.buf, "\tq"));
    editor_free(&e);

    /* move, duplicate, delete, join */
    ed_load(&e, "1\n2\n3");
    e.cy = 1;
    edit_move_lines(&e, 0);
    CHECK(buf_is(e.buf, "2\n1\n3") && e.cy == 0);
    edit_move_lines(&e, 0);             /* already first: nothing happens */
    CHECK(buf_is(e.buf, "2\n1\n3") && e.cy == 0);
    edit_move_lines(&e, 1);
    CHECK(buf_is(e.buf, "1\n2\n3") && e.cy == 1);
    e.cy = 2;
    edit_move_lines(&e, 1);             /* already last */
    CHECK(buf_is(e.buf, "1\n2\n3"));
    ed_sel(&e, 0, 0, 1, 1);
    edit_move_lines(&e, 1);
    CHECK(buf_is(e.buf, "3\n1\n2") && e.ay == 1 && e.cy == 2);
    edit_undo(&e);
    CHECK(buf_is(e.buf, "1\n2\n3"));
    e.sel_on = 0;
    e.cy = 0;
    edit_duplicate_lines(&e, 1);
    CHECK(buf_is(e.buf, "1\n1\n2\n3") && e.cy == 1);
    edit_duplicate_lines(&e, 0);
    CHECK(buf_is(e.buf, "1\n1\n1\n2\n3") && e.cy == 1);
    e.cy = 4;
    edit_delete_lines(&e);
    CHECK(buf_is(e.buf, "1\n1\n1\n2") && e.cy == 3);
    editor_free(&e);

    ed_load(&e, "foo\n   bar\nbaz");
    edit_join_lines(&e);
    CHECK(buf_is(e.buf, "foo bar\nbaz") && e.cy == 0 && e.cx == 3);
    e.cy = 1;
    edit_join_lines(&e);                /* last line: nothing to join */
    CHECK(buf_is(e.buf, "foo bar\nbaz"));
    edit_undo(&e);
    CHECK(buf_is(e.buf, "foo\n   bar\nbaz"));
    editor_free(&e);

    /* indent and dedent a selection */
    ed_load(&e, "a\n  b\n        c\nd");
    ed_sel(&e, 0, 0, 2, 3);
    edit_tab(&e);
    CHECK(buf_is(e.buf, "\ta\n\t  b\n\t        c\nd") && e.cx == 4);
    edit_dedent(&e);
    CHECK(buf_is(e.buf, "a\n  b\n        c\nd"));
    edit_dedent(&e);                    /* spaces: up to tabw of them */
    CHECK(buf_is(e.buf, "a\nb\nc\nd"));
    edit_undo(&e);
    edit_undo(&e);
    edit_undo(&e);
    CHECK(buf_is(e.buf, "a\n  b\n        c\nd"));
    e.sel_on = 0;
    e.cy = 3;
    e.cx = 0;
    edit_tab(&e);                       /* no selection: a tab character */
    CHECK(buf_is(e.buf, "a\n  b\n        c\n\td"));
    editor_free(&e);
}

/* ---- syntax ------------------------------------------------------------ */

static Syntax *lang(const char *name)
{
    Buffer b;
    Syntax *s;
    char path[64];

    buf_init(&b);
    snprintf(path, sizeof path, "x.%s", name);
    b.path = xstrdup(path);
    s = syn_detect(&b);
    buf_free(&b);
    return s;
}

/* One letter per byte: the class of each byte of text. */
static const char *cls_str(const Syntax *s, uint16_t *state, const char *text)
{
    static char out[256];
    uint8_t cls[256];
    static const char letters[] = ".cskTCnpfovhedl";
    size_t i, n = strlen(text);

    *state = syn_line(s, *state, text, n, cls);
    for (i = 0; i < n; i++)
        out[i] = letters[cls[i]];
    out[n] = '\0';
    return out;
}

#define CLS(s, st, text, want) CHECK(strcmp(cls_str((s), &(st), (text)), (want)) == 0)

static void test_syntax_lines(void)
{
    Syntax *c = lang("c"), *py = lang("py"), *sh = lang("sh"), *md = lang("md"), *js = lang("js");
    uint16_t st = 0;

    CHECK(c && py && sh && md && js);
    CLS(c, st, "if (a) return 1;", "kk.....kkkkkk.n.");
    CLS(c, st, "a=b", ".o.");
    CLS(c, st, "iffy int", ".....TTT");
    CLS(c, st, "foo(1)", "fff.n.");
    CLS(c, st, "1.5e3f 0xFF", "nnnnnn.nnnn");
    CLS(c, st, "\"a\\\"b\" x", "ssssss..");
    CLS(c, st, "  #include <x>", "..pppppppppppp");
    CLS(c, st, "x // y", "..cccc");
    CLS(c, st, "x #y", "....");               /* # is only a directive at line start */
    CHECK(st == 0);
    CLS(c, st, "x /* a", "..cccc");
    CHECK(st != 0);
    CLS(c, st, " b */ c", "ccccc..");
    CHECK(st == 0);
    CLS(c, st, "'\\'' z", "ssss..");
    CLS(c, st, "NULL", "CCCC");

    CLS(py, st, "x = \"\"\"a", "..o.ssss");
    CHECK(st != 0);
    CLS(py, st, "b\"\"\" + 1", "ssss.o.n");
    CHECK(st == 0);
    CLS(py, st, "@dec", "pppp");
    CLS(py, st, "# c", "ccc");
    CLS(py, st, "def f(self):", "kkk.f.vvvv.o");
    CLS(py, st, "a @ b", ".....");

    CLS(sh, st, "echo $HOME ${x} # c", "TTTT.vvvvv.vvvv.ccc");
    CLS(sh, st, "echo $# \"a b\"", "TTTT.vv.sssss");
    CLS(sh, st, "if [ 1 ]; then", "kk...n..o.kkkk");

    CLS(md, st, "# Title", "hhhhhhh");
    CLS(md, st, "- *a* `b`", "..eee.ddd");
    CLS(md, st, "* item", "......");
    CLS(md, st, "```c", "dddd");
    CHECK(st != 0);
    CLS(md, st, "# not a heading", "ddddddddddddddd");
    CLS(md, st, "```", "ddd");
    CHECK(st == 0);
    CLS(md, st, "[a](b)", "lll...");

    CLS(js, st, "let s = 'a';", "kkk...o.sss.");
    CLS(js, st, "$el.x", ".....");
    CLS(js, st, "a = `x", "..o.ss");
    CHECK(st != 0);
    CLS(js, st, "y`;", "ss.");
    CHECK(st == 0);

    syn_free(c); syn_free(py); syn_free(sh); syn_free(md); syn_free(js);
}

static void test_syntax_parse(void)
{
    char err[128];
    size_t i;
    Syntax *s;

    CHECK(syn_parse("name X\nregion bogus a b\n", 24, err, sizeof err) == NULL);
    CHECK(strstr(err, "riga 2") != NULL);
    CHECK(syn_parse("region comment a b\n", 19, err, sizeof err) == NULL);
    CHECK(syn_parse("name X\nfoo\n", 11, err, sizeof err) == NULL);
    CHECK(syn_parse("name X\nregion comment a $ ml\n", 29, err, sizeof err) == NULL);
    CHECK(syn_parse("name X\nregion comment a b zzz\n", 30, err, sizeof err) == NULL);
    s = syn_parse("# note\nname Due Parole\nwords keyword a b\n", 41, err, sizeof err);
    CHECK(s && strcmp(syn_name(s), "Due Parole") == 0);
    syn_free(s);

    /* every built-in definition must parse */
    for (i = 0; i < lang_ndefs; i++) {
        SBuf t = SBUF_INIT;
        const char *const *d;

        for (d = lang_defs[i].def; *d; d++) {
            sbuf_adds(&t, *d);
            sbuf_add(&t, "\n", 1);
        }
        s = syn_parse(t.p, t.len, err, sizeof err);
        CHECK(s != NULL);
        if (s) {
            CHECK(strcmp(syn_name(s), lang_defs[i].name) == 0);
            syn_free(s);
        } else {
            fprintf(stderr, "%s: %s\n", lang_defs[i].name, err);
        }
        sbuf_free(&t);
    }
}

static const char *detect_name(const char *path, const char *text)
{
    static char name[32];
    Buffer b;
    Syntax *s;
    size_t y = 0, x = 0;

    buf_init(&b);
    buf_insert_text(&b, &y, &x, text, strlen(text));
    if (path)
        b.path = xstrdup(path);
    s = syn_detect(&b);
    snprintf(name, sizeof name, "%s", s ? syn_name(s) : "-");
    syn_free(s);
    buf_free(&b);
    return name;
}

static void test_syntax_detect(void)
{
    CHECK(strcmp(detect_name("a/b.c", ""), "C") == 0);
    CHECK(strcmp(detect_name("x.h", ""), "C") == 0);
    CHECK(strcmp(detect_name("x.PY", ""), "Python") == 0);
    CHECK(strcmp(detect_name("x.tsx", ""), "TypeScript") == 0);
    CHECK(strcmp(detect_name("README.MD", ""), "Markdown") == 0);
    CHECK(strcmp(detect_name("/home/u/.bashrc", ""), "Shell") == 0);
    CHECK(strcmp(detect_name("run", "#!/usr/bin/env python3\nx"), "Python") == 0);
    CHECK(strcmp(detect_name("run", "#!/bin/bash\n"), "Shell") == 0);
    CHECK(strcmp(detect_name("run", "#!/usr/bin/python3.11\n"), "Python") == 0);
    CHECK(strcmp(detect_name(NULL, "#!/bin/sh\n"), "Shell") == 0);
    CHECK(strcmp(detect_name("a.txt", "x\n# vim: set ft=markdown :\n"), "Markdown") == 0);
    CHECK(strcmp(detect_name("a.txt", "// ked: ft=c++\n"), "C") == 0);
    CHECK(strcmp(detect_name("a.c", "# ked: filetype=python\n"), "Python") == 0);
    CHECK(strcmp(detect_name("a.txt", "plain\n"), "-") == 0);
    CHECK(strcmp(detect_name(NULL, ""), "-") == 0);
    CHECK(strcmp(detect_name("Makefile", ""), "Makefile") == 0);
}

static void test_syntax_langs(void)
{
    Syntax *php = lang("php"), *html = lang("html"), *sql = lang("sql"), *json = lang("json");
    Syntax *ts = lang("ts"), *go = lang("go"), *diff = lang("diff"), *yaml = lang("yml");
    Syntax *css = lang("css"), *rb = lang("rb");
    uint16_t st = 0;

    CHECK(php && html && sql && json && ts && go && diff && yaml && css && rb);

    /* PHP: tag tokens, variables, HTML between ?> and <? */
    CLS(php, st, "<?php echo $a->b(\"x\"); // c", "ppppp.kkkk.vvoof.sss...cccc");
    CHECK(st == 0);
    CLS(php, st, "$y = 1; #[Attr] // z", "vv.o.n..ppppppp.cccc");
    CLS(php, st, "?> it's <b>", "...........");
    CHECK(st != 0);
    CLS(php, st, "x <?= $x; ?>", "....o.vv....");     /* back in PHP at <?, in HTML again at ?> */
    CHECK(st != 0);

    /* HTML: tags, comments, script bodies are left alone */
    st = 0;
    CLS(html, st, "<a href=\"x\">t < u</a>", "kkkkkkkkkkkk.....kkkk");
    CLS(html, st, "<script>", "dddddddd");
    CHECK(st != 0);
    CLS(html, st, "if (a<b) {", "dddddddddd");
    CLS(html, st, "</script> <b>", "ddddddddd.kkk");
    CHECK(st == 0);

    /* SQL ignores case */
    CLS(sql, st, "SELECT a FROM t", "kkkkkk...kkkk..");
    CLS(sql, st, "select a from t", "kkkkkk...kkkk..");
    CLS(sql, st, "x = 'it''s' -- c", "..o.sssssss.cccc");

    st = 0;
    CLS(json, st, "{\"a\": [1, true]}", ".ssso..no.CCCC..");
    CLS(ts, st, "interface A { x: number }", "kkkkkkkkk......o.TTTTTT..");
    CLS(ts, st, "@Component() class B {}", "pppppppppp...kkkkk.....");
    CLS(go, st, "x := `r`", "..oo.sss");
    CLS(yaml, st, "key: \"v\" # c", "...o.sss.ccc");
    CLS(css, st, "a { margin: 10px; }", "T...kkkkkko.nnnn...");
    CLS(rb, st, "def foo?(x)", "kkk.ffff...");

    st = 0;
    CLS(diff, st, "--- a/x", "hhhhhhh");
    CLS(diff, st, "+new", "cccc");
    CLS(diff, st, "-old", "ssss");
    CLS(diff, st, " ctx", "....");
    CLS(diff, st, "@@ -1 +1 @@", "ppppppppppp");

    syn_free(php); syn_free(html); syn_free(sql); syn_free(json); syn_free(ts); syn_free(go);
    syn_free(diff); syn_free(yaml); syn_free(css); syn_free(rb);
}

static void test_syntax_detect_langs(void)
{
    static const struct { const char *path, *first, *want; } t[] = {
        { "a.php", "", "PHP" }, { "a.phtml", "", "PHP" }, { "run", "#!/usr/bin/php\n", "PHP" },
        { "a.ts", "", "TypeScript" }, { "a.js", "", "JavaScript" }, { "a.jsx", "", "JavaScript" },
        { "a.html", "", "HTML" }, { "a.svg", "", "XML" }, { "a.css", "", "CSS" },
        { "a.json", "", "JSON" }, { ".eslintrc", "", "JSON" }, { "a.yml", "", "YAML" },
        { "a.toml", "", "TOML" }, { "a.sql", "", "SQL" }, { "a.go", "", "Go" },
        { "a.rs", "", "Rust" }, { "A.java", "", "Java" }, { "a.cs", "", "C#" },
        { "a.rb", "", "Ruby" }, { "Rakefile", "", "Ruby" }, { "run", "#!/usr/bin/env ruby\n", "Ruby" },
        { "Makefile", "", "Makefile" }, { "rules.mk", "", "Makefile" },
        { "Dockerfile", "", "Dockerfile" }, { "a.patch", "", "Diff" },
        { "a.txt", "// ked: ft=typescript\n", "TypeScript" },
    };
    size_t i;

    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        if (strcmp(detect_name(t[i].path, t[i].first), t[i].want) != 0)
            fprintf(stderr, "detect %s: got %s, want %s\n", t[i].path,
                    detect_name(t[i].path, t[i].first), t[i].want);
        CHECK(strcmp(detect_name(t[i].path, t[i].first), t[i].want) == 0);
    }
}

/* ---- mouse ------------------------------------------------------------- */

static void put_file(const char *dir, const char *name, const char *text);

static void mclick(Editor *e, int kind, int x, int y, int mods, int button)
{
    MouseEv ev;

    ev.kind = (MouseKind)kind;
    ev.button = button;
    ev.x = x;
    ev.y = y;
    ev.mods = mods;
    mouse_event(e, &ev);
}

static void mouse_screen(Editor *e, int tabs)
{
    e->rows = 24;
    e->cols = 80;
    e->text_x = 0;
    e->text_y = tabs ? 1 : 0;
    e->sidebar_w = 0;
    layout_compute(&e->layout, 0, e->text_y, 80, 23 - e->text_y);
}

static void test_mouse(void)
{
    Editor e;
    MouseHit h;
    char dir[] = "/tmp/ked_mouse_XXXXXX";
    char p1[300], p2[300], err[128];
    SBuf text = SBUF_INIT;
    size_t i;

    /* clicks: cells map to lines and bytes, tabs and wide characters included */
    ed_load(&e, "one\ntwo\n\tx \xc3\xa9 y\nlast");
    mouse_screen(&e, 0);
    mouse_locate(&e, 8, 2, &h);
    CHECK(h.kind == MH_TEXT && h.y == 2 && h.x == 1);
    mouse_locate(&e, 3, 2, &h);
    CHECK(h.kind == MH_TEXT && h.y == 2 && h.x == 0);       /* inside the tab */
    mouse_locate(&e, 10, 2, &h);
    CHECK(h.kind == MH_TEXT && h.x == 3);
    mouse_locate(&e, 70, 1, &h);
    CHECK(h.kind == MH_TEXT && h.y == 1 && h.x == 3);       /* past the end of the line */
    mouse_locate(&e, 5, 15, &h);
    CHECK(h.kind == MH_TEXT && h.y == 3);                   /* below the text: the last line */
    mouse_locate(&e, 5, 23, &h);
    CHECK(h.kind == MH_NONE);                               /* the message row */
    mouse_locate(&e, 5, 22, &h);
    CHECK(h.kind == MH_STATUS);

    mclick(&e, MOUSE_PRESS, 8, 2, 0, 0);
    CHECK(e.cy == 2 && e.cx == 1 && !e.sel_on && e.want_col == 8);
    mclick(&e, MOUSE_RELEASE, 8, 2, 0, 0);

    /* drag selects from the press to the pointer, also past the pane's edge */
    mclick(&e, MOUSE_PRESS, 1, 0, 0, 0);
    mclick(&e, MOUSE_DRAG, 2, 1, 0, 0);
    CHECK(e.sel_on && e.ay == 0 && e.ax == 1 && e.cy == 1 && e.cx == 2);
    mclick(&e, MOUSE_RELEASE, 2, 1, 0, 0);
    mclick(&e, MOUSE_DRAG, 0, 0, 0, 0);                     /* no drag running: ignored */
    CHECK(e.cy == 1 && e.cx == 2);

    /* double click: word; triple: line */
    e.mouse.t = 0;
    mclick(&e, MOUSE_PRESS, 1, 0, 0, 0);
    mclick(&e, MOUSE_RELEASE, 1, 0, 0, 0);
    mclick(&e, MOUSE_PRESS, 1, 0, 0, 0);
    CHECK(e.sel_on && e.ay == 0 && e.ax == 0 && e.cy == 0 && e.cx == 3);
    mclick(&e, MOUSE_PRESS, 1, 0, 0, 0);
    CHECK(e.sel_on && e.ay == 0 && e.ax == 0 && e.cy == 1 && e.cx == 0);
    mclick(&e, MOUSE_PRESS, 2, 1, 0, 0);                    /* another cell: a single click */
    CHECK(!e.sel_on && e.cy == 1 && e.cx == 2);

    /* Alt+click adds a cursor; a plain click drops it again */
    mclick(&e, MOUSE_PRESS, 1, 3, MOD_ALT, 0);
    CHECK(e.nxc == 1 && e.cy == 3 && e.cx == 1);
    mclick(&e, MOUSE_PRESS, 1, 3, MOD_ALT, 0);              /* the same place: nothing new */
    CHECK(e.nxc == 1);
    e.mouse.t = 0;
    mclick(&e, MOUSE_PRESS, 0, 0, 0, 0);
    CHECK(e.nxc == 0 && e.cy == 0 && e.cx == 0);

    /* turned off: nothing happens */
    e.mouse_on = 0;
    mclick(&e, MOUSE_PRESS, 2, 2, 0, 0);
    CHECK(e.cy == 0);
    e.mouse_on = 1;

    /* the picker and the prompt row */
    picker_open(&e.picker, "Prova");
    picker_add(&e.picker, "uno");
    picker_add(&e.picker, "due");
    picker_filter(&e.picker);
    e.picker.on = 1;
    mouse_locate(&e, 3, e.rows - 1 - render_picker_height(&e) + 2, &h);
    CHECK(h.kind == MH_PICKER && h.index == 2);
    mouse_locate(&e, 3, 2, &h);
    CHECK(h.kind == MH_NONE);
    e.picker.on = 0;
    editor_free(&e);

    /* the wheel scrolls a long text and keeps the cursor in the window */
    for (i = 0; i < 100; i++)
        sbuf_addf(&text, "line %zu\n", i);
    ed_load(&e, text.p);
    sbuf_free(&text);
    mouse_screen(&e, 0);
    mclick(&e, MOUSE_WHEEL_DOWN, 5, 5, 0, 0);
    CHECK(e.rowoff == 3 && e.cy == 3);
    mclick(&e, MOUSE_WHEEL_DOWN, 5, 5, 0, 0);
    CHECK(e.rowoff == 6 && e.cy == 6);
    mclick(&e, MOUSE_WHEEL_UP, 5, 5, 0, 0);
    CHECK(e.rowoff == 3 && e.cy == 6);
    mclick(&e, MOUSE_WHEEL_UP, 5, 5, 0, 0);
    mclick(&e, MOUSE_WHEEL_UP, 5, 5, 0, 0);
    CHECK(e.rowoff == 0);
    for (i = 0; i < 100; i++)
        mclick(&e, MOUSE_WHEEL_DOWN, 5, 5, 0, 0);
    CHECK(e.rowoff == 79 && e.cy >= 79);                    /* the last line ends the window */
    editor_free(&e);

    /* the tab bar: a click selects, a middle click closes */
    CHECK(mkdtemp(dir) != NULL);
    put_file(dir, "a.c", "int a;\n");
    put_file(dir, "b.txt", "bee\n");
    snprintf(p1, sizeof p1, "%s/a.c", dir);
    snprintf(p2, sizeof p2, "%s/b.txt", dir);
    editor_init(&e);
    CHECK(editor_open_doc(&e, p1, err, sizeof err) != NULL);
    CHECK(editor_open_doc(&e, p2, err, sizeof err) != NULL);
    mouse_screen(&e, 1);
    CHECK(e.ndocs == 2 && e.doc == e.docs[1]);
    mouse_locate(&e, 2, 0, &h);
    CHECK(h.kind == MH_TAB && h.index == 0);
    mouse_locate(&e, 7, 0, &h);
    CHECK(h.kind == MH_TAB && h.index == 1);
    mouse_locate(&e, 60, 0, &h);
    CHECK(h.kind == MH_NONE);
    mclick(&e, MOUSE_PRESS, 2, 0, 0, 0);
    CHECK(e.doc == e.docs[0]);
    mclick(&e, MOUSE_PRESS, 7, 0, 0, 1);
    CHECK(e.ndocs == 1 && strstr(e.doc->buf.path, "a.c") != NULL);
    editor_free(&e);
}

static void test_syntax_incremental(void)
{
    Syntax *c = lang("c");
    Buffer b;
    size_t y = 0, x = 0, cy, cx;
    const char *text = "a\nb\nc\nd\ne";
    const uint8_t *k;

    buf_init(&b);
    buf_insert_text(&b, &y, &x, text, strlen(text));
    buf_undo_close(&b);
    syn_update(c, &b, 4);
    CHECK(b.stable == 5 && b.lines[4].hl == 0);

    buf_undo_begin(&b, 0, 0);
    buf_insert(&b, 1, 0, "/*", 2);
    buf_undo_close(&b);
    CHECK(b.stable == 1);
    syn_update(c, &b, 4);
    CHECK(b.lines[1].hl != 0 && b.lines[4].hl != 0);
    k = syn_classes(c, &b, 3);
    CHECK(k && k[0] == SC_COMMENT);

    CHECK(buf_undo(&b, &cy, &cx) == 1);
    CHECK(b.stable <= 1);
    syn_update(c, &b, 4);
    CHECK(b.lines[4].hl == 0);
    k = syn_classes(c, &b, 3);
    CHECK(k && k[0] == SC_NORMAL);

    /* line operations shift the lines but keep the states consistent */
    buf_undo_begin(&b, 0, 0);
    buf_insert(&b, 0, 0, "/*", 2);
    buf_split(&b, 2, 0);
    buf_undo_close(&b);
    CHECK(b.stable <= 2);
    syn_update(c, &b, b.nlines - 1);
    CHECK(b.lines[b.nlines - 1].hl != 0);
    buf_join(&b, 2);
    CHECK(b.stable <= 2);
    syn_update(c, &b, b.nlines - 1);
    CHECK(b.lines[b.nlines - 1].hl != 0);

    buf_free(&b);
    syn_free(c);
}

static int match_at(Syntax *s, const char *text, size_t y, size_t x, size_t *my, size_t *mx)
{
    Buffer b;
    size_t ty = 0, tx = 0, py, px;
    int r;

    buf_init(&b);
    buf_insert_text(&b, &ty, &tx, text, strlen(text));
    r = syn_match_bracket(s, &b, y, x, &py, &px, my, mx);
    buf_free(&b);
    return r;
}

static void test_syntax_brackets(void)
{
    Syntax *c = lang("c");
    const char *t = "f(a,\n  (b)) \")\" ;";
    size_t my, mx;

    CHECK(match_at(c, t, 0, 1, &my, &mx) == 1 && my == 1 && mx == 5);
    CHECK(match_at(c, t, 0, 2, &my, &mx) == 1 && my == 1 && mx == 5);   /* just after */
    CHECK(match_at(c, t, 1, 5, &my, &mx) == 1 && my == 0 && mx == 1);
    CHECK(match_at(c, t, 1, 2, &my, &mx) == 1 && my == 1 && mx == 4);
    CHECK(match_at(c, t, 1, 8, &my, &mx) == 0);                       /* inside a string */
    CHECK(match_at(c, t, 0, 0, &my, &mx) == 0);                       /* not a bracket */
    CHECK(match_at(c, "(a", 0, 0, &my, &mx) == 0);                    /* unbalanced */
    CHECK(match_at(c, "{ \"}\" }", 0, 0, &my, &mx) == 1 && mx == 6);  /* skips the string */
    CHECK(match_at(c, "[ ( ]", 0, 0, &my, &mx) == 1 && mx == 4);
    CHECK(match_at(NULL, "{ \"}\" }", 0, 0, &my, &mx) == 1 && mx == 3);   /* plain text */
    syn_free(c);
}

static void test_theme(void)
{
    SBuf o = SBUF_INIT;
    Rgb red = { 255, 0, 0 }, black = { 0, 0, 0 }, grey = { 128, 128, 128 };

    CHECK(theme_to_256(red) == 196);
    CHECK(theme_to_256(black) == 16);
    CHECK(theme_to_256(grey) == 244);
    CHECK(theme_to_16(red) == 9);
    CHECK(theme_to_16(black) == 0);

    theme_fg(&o, COLOR_TRUE, red);
    CHECK(o.len == 15 && memcmp(o.p, "\x1b[38;2;255;0;0m", 15) == 0);
    o.len = 0;
    theme_bg(&o, COLOR_256, red);
    CHECK(o.len == 11 && memcmp(o.p, "\x1b[48;5;196m", 11) == 0);
    o.len = 0;
    theme_fg(&o, COLOR_16, red);
    CHECK(o.len == 5 && memcmp(o.p, "\x1b[91m", 5) == 0);
    o.len = 0;
    theme_bg(&o, COLOR_16, black);
    CHECK(o.len == 5 && memcmp(o.p, "\x1b[40m", 5) == 0);
    o.len = 0;
    theme_fg(&o, COLOR_NONE, red);
    CHECK(o.len == 0);
    sbuf_free(&o);

    CHECK(theme_count() >= 2);
    CHECK(theme_get(0) != theme_get(1) && theme_get(theme_count()) == theme_get(0));
}

/* ---- phase 4: documents, layout, fuzzy, files, session -------------------- */

static void put_file(const char *dir, const char *name, const char *text)
{
    char path[512];
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *make_dir(char *buf, size_t n)
{
    snprintf(buf, n, "/tmp/ked-test-XXXXXX");
    return mkdtemp(buf);
}

static void rm_rf(const char *dir)
{
    char cmd[600];

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "rm -rf failed\n");
}

static void test_fuzzy_picker(void)
{
    Picker p;

    CHECK(fuzzy_score("", "anything") == 0);
    CHECK(fuzzy_score("xyz", "src/main.c") < 0);
    CHECK(fuzzy_score("mc", "src/main.c") >= 0);
    CHECK(fuzzy_score("MAIN", "src/main.c") >= 0);             /* case-insensitive */
    CHECK(fuzzy_score("main", "src/main.c") > fuzzy_score("main", "src/domain/xin.c"));
    CHECK(fuzzy_score("ed", "src/editor.c") > fuzzy_score("ed", "src/render.c"));
    CHECK(fuzzy_score("main", "main.c") > fuzzy_score("main", "a/b/c/d/main.c"));

    memset(&p, 0, sizeof p);
    picker_open(&p, "Test");
    picker_add(&p, "src/render.c");
    picker_add(&p, "src/editor.c");
    picker_add(&p, "README.md");
    picker_filter(&p);
    CHECK(p.nshown == 3 && picker_chosen(&p) == 0);            /* no query: original order */
    picker_move(&p, 2);
    CHECK(picker_chosen(&p) == 2);
    picker_move(&p, 5);
    CHECK(picker_chosen(&p) == 2);                             /* clamped */
    picker_type(&p, 'e');
    picker_type(&p, 'd');
    picker_type(&p, 'i');
    CHECK(p.nshown == 1 && picker_chosen(&p) == 1);            /* only editor.c has e..d */
    picker_backspace(&p);
    picker_backspace(&p);
    picker_backspace(&p);
    CHECK(p.nshown == 3 && p.qlen == 0);
    picker_type(&p, 'z');
    CHECK(p.nshown == 0 && picker_chosen(&p) == -1);
    picker_free(&p);
}

static void test_layout(void)
{
    Doc *d = doc_new();
    Layout l;
    Win *a, *b, *c, *w[8];

    layout_init(&l, d);
    layout_compute(&l, 0, 1, 80, 22);
    a = l.active;
    CHECK(layout_count(&l) == 1 && a->x == 0 && a->y == 1 && a->w == 80 && a->h == 22);

    b = layout_split(&l, 1);                                   /* side by side */
    CHECK(b && l.active == b && layout_count(&l) == 2);
    layout_compute(&l, 0, 1, 80, 22);
    CHECK(a->x == 0 && a->w + 1 + b->w == 80 && b->x == a->w + 1 && a->h == 22 && b->h == 22);
    CHECK(layout_at(&l, 0, 1) == a && layout_at(&l, a->w, 1) == NULL && layout_at(&l, 79, 5) == b);

    c = layout_split(&l, 0);                                   /* stack b */
    CHECK(c && layout_count(&l) == 3);
    layout_compute(&l, 0, 1, 80, 22);
    CHECK(b->w == c->w && b->x == c->x && c->y == b->y + b->h && b->h + c->h == 22);
    CHECK(layout_wins(&l, w, 8) == 3 && w[0] == a && w[1] == b && w[2] == c);
    CHECK(layout_uses(&l, d) == 3);

    layout_cycle(&l, 1);
    CHECK(l.active == a);
    layout_cycle(&l, -1);
    CHECK(l.active == c);

    CHECK(layout_close(&l, b) == 0 && layout_count(&l) == 2);
    layout_compute(&l, 0, 1, 80, 22);
    CHECK(c->h == 22);
    CHECK(a->w + 1 + c->w == 80);
    CHECK(layout_close(&l, c) == 0 && layout_count(&l) == 1 && l.active == a);
    layout_compute(&l, 0, 1, 80, 22);
    CHECK(a->w == 80 && layout_close(&l, a) == -1);

    /* too small to split */
    layout_compute(&l, 0, 1, 20, 5);
    CHECK(layout_split(&l, 1) == NULL && layout_split(&l, 0) == NULL);
    layout_free(&l);
    doc_free(d);
}

static void test_docs(void)
{
    char dir[64], p1[600], p2[600], err[256];
    Doc *d;
    Editor e;
    struct timespec ts = { 0, 50000000 };

    CHECK(make_dir(dir, sizeof dir) != NULL);
    put_file(dir, "a.c", "int a;\nint b;\n");
    put_file(dir, "b.txt", "hello\n");
    snprintf(p1, sizeof p1, "%s/a.c", dir);
    snprintf(p2, sizeof p2, "%s/b.txt", dir);

    d = doc_new();
    CHECK(doc_load(d, p1, err, sizeof err) == 0 && d->buf.nlines == 2);
    CHECK(d->syn && strcmp(syn_name(d->syn), "C") == 0 && !d->readonly && d->disk_known);
    CHECK(doc_disk_changed(d) == DISK_SAME);
    nanosleep(&ts, NULL);
    put_file(dir, "a.c", "int a;\n");
    CHECK(doc_disk_changed(d) == DISK_CHANGED);
    CHECK(doc_reload(d, err, sizeof err) == 0 && d->buf.nlines == 1 && !d->buf.modified);
    CHECK(doc_disk_changed(d) == DISK_SAME);
    CHECK(doc_save(d, NULL, err, sizeof err) >= 0 && doc_disk_changed(d) == DISK_SAME);
    CHECK(doc_save(d, p2, err, sizeof err) >= 0 && strcmp(doc_base_name(d), "b.txt") == 0);
    CHECK(strcmp(syn_name(d->syn), "Testo") == 0 || d->syn == NULL);    /* language follows the name */
    unlink(p2);
    CHECK(doc_disk_changed(d) == DISK_MISSING);
    doc_free(d);

    chmod(p1, 0444);
    d = doc_new();
    CHECK(doc_load(d, p1, err, sizeof err) == 0);
    CHECK(d->readonly == (geteuid() != 0));
    doc_free(d);
    chmod(p1, 0644);

    put_file(dir, "b.txt", "hello\n");
    put_file(dir, "a.c", "int a;\nint b;\n");
    editor_init(&e);
    CHECK(e.ndocs == 1);
    CHECK(editor_open_doc(&e, p1, err, sizeof err) != NULL);
    CHECK(e.ndocs == 1 && e.doc->buf.path != NULL);            /* the blank document was replaced */
    CHECK(editor_open_doc(&e, p2, err, sizeof err) != NULL);
    CHECK(e.ndocs == 2 && strcmp(doc_base_name(e.doc), "b.txt") == 0);
    CHECK(editor_open_doc(&e, p1, err, sizeof err) == e.docs[0] && e.ndocs == 2);   /* no duplicate */
    editor_goto(&e, 2, 4);
    CHECK(e.cy == 1 && e.cx == 3);
    editor_activate(&e, e.docs[1]);
    editor_activate(&e, e.docs[0]);
    CHECK(e.cy == 1 && e.cx == 3);                             /* each document keeps its cursor */
    CHECK(editor_open_doc(&e, dir, err, sizeof err) == NULL);
    editor_free(&e);
    rm_rf(dir);
}

typedef struct {
    char names[64][64];
    size_t n;
} Names;

static void collect_name(const char *rel, void *ctx)
{
    Names *nm = ctx;

    if (nm->n < 64)
        snprintf(nm->names[nm->n++], 64, "%s", rel);
}

static void test_files_tree(void)
{
    char dir[64], sub[600];
    Names nm;
    Tree t;
    size_t i;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    snprintf(sub, sizeof sub, "%s/src", dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/.git", dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/build", dir);
    mkdir(sub, 0755);
    put_file(dir, "z.txt", "");
    put_file(dir, "a.txt", "");
    put_file(dir, "src/main.c", "");
    put_file(dir, ".git/config", "");
    put_file(dir, "build/out.o", "");
    put_file(dir, ".hidden", "");

    nm.n = 0;
    CHECK(files_scan(dir, collect_name, &nm) == 3 && nm.n == 3);
    CHECK(strcmp(nm.names[0], "a.txt") == 0 && strcmp(nm.names[1], "src/main.c") == 0 &&
          strcmp(nm.names[2], "z.txt") == 0);

    tree_init(&t, dir);
    /* root, then folders first (.git, build, src), then files; hidden shown in the browser */
    CHECK(t.n >= 5 && t.e[0].is_dir && t.e[0].open && t.e[0].depth == 0);
    for (i = 1; i < t.n && t.e[i].is_dir; i++)
        ;
    CHECK(i >= 2);                                              /* at least one folder before files */
    for (; i < t.n; i++)
        CHECK(!t.e[i].is_dir);
    for (i = 1; i < t.n && strcmp(t.e[i].name, "src") != 0; i++)
        ;
    CHECK(i < t.n && !t.e[i].open);
    {
        size_t before = t.n;

        tree_toggle(&t, i);
        CHECK(t.e[i].open && t.n == before + 1 && strcmp(t.e[i + 1].name, "main.c") == 0 &&
              t.e[i + 1].depth == 2);
        t.sel = i + 1;
        tree_left(&t);                                          /* a file: goes to its folder */
        CHECK(t.sel == i);
        tree_left(&t);                                          /* an open folder: closes it */
        CHECK(!t.e[i].open && t.n == before);
    }
    tree_free(&t);
    rm_rf(dir);
}

static void test_session(void)
{
    char dir[64], file[600];
    Session s, r;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    snprintf(file, sizeof file, "%s/sub/deeper/sess", dir);
    memset(&s, 0, sizeof s);
    session_add(&s, "/tmp/a b.c", 3, 4, 1);
    session_add(&s, "/tmp/x.txt", 0, 0, 0);
    s.active = 1;
    CHECK(session_save(file, &s) == 0);
    CHECK(session_load(file, &r) == 0 && r.n == 2 && r.active == 1);
    CHECK(strcmp(r.e[0].path, "/tmp/a b.c") == 0 && r.e[0].y == 3 && r.e[0].x == 4 &&
          r.e[0].rowoff == 1 && strcmp(r.e[1].path, "/tmp/x.txt") == 0);
    session_free(&r);
    session_free(&s);

    memset(&s, 0, sizeof s);
    CHECK(session_save(file, &s) == 0);                         /* no entries: file removed */
    CHECK(session_load(file, &r) < 0);
    put_file(dir, "bad", "not a session\n");
    snprintf(file, sizeof file, "%s/bad", dir);
    CHECK(session_load(file, &r) < 0);

    setenv("XDG_STATE_HOME", dir, 1);
    {
        char *f = session_file();

        CHECK(f && strstr(f, "/ked/sessions/%") != NULL && strncmp(f, dir, strlen(dir)) == 0);
        free(f);
    }
    unsetenv("XDG_STATE_HOME");
    rm_rf(dir);
}

static void test_session_layout(void)
{
    char dir[64], p1[600], p2[600], cwd[600], err[256];
    Editor e;
    Win *w[8];

    CHECK(make_dir(dir, sizeof dir) != NULL);
    put_file(dir, "a.c", "int a;\nint b;\nint c;\n");
    put_file(dir, "b.txt", "one\ntwo\n");
    snprintf(p1, sizeof p1, "%s/a.c", dir);
    snprintf(p2, sizeof p2, "%s/b.txt", dir);
    setenv("XDG_STATE_HOME", dir, 1);
    CHECK(getcwd(cwd, sizeof cwd) != NULL);

    editor_init(&e);
    e.rows = 30;
    e.cols = 100;
    CHECK(editor_open_doc(&e, p1, err, sizeof err) != NULL);
    CHECK(editor_open_doc(&e, p2, err, sizeof err) != NULL);
    editor_activate(&e, e.docs[0]);
    editor_goto(&e, 3, 1);
    editor_save_session(&e);                                    /* single pane first */
    editor_free(&e);

    editor_init(&e);
    e.rows = 30;
    e.cols = 100;
    CHECK(editor_restore_session(&e) == 2 && layout_count(&e.layout) == 1 && e.cy == 2);
    editor_free(&e);

    /* now with panes: a | (a over b), focus on the lower right one */
    editor_init(&e);
    e.rows = 30;
    e.cols = 100;
    CHECK(editor_open_doc(&e, p1, err, sizeof err) != NULL);
    CHECK(editor_open_doc(&e, p2, err, sizeof err) != NULL);
    editor_activate(&e, e.docs[0]);
    editor_split(&e, 1);
    editor_split(&e, 0);
    editor_activate(&e, e.docs[1]);
    editor_goto(&e, 2, 2);
    CHECK(layout_count(&e.layout) == 3);
    editor_save_session(&e);
    editor_free(&e);

    editor_init(&e);
    e.rows = 30;
    e.cols = 100;
    CHECK(editor_restore_session(&e) == 2);
    CHECK(layout_count(&e.layout) == 3 && layout_wins(&e.layout, w, 8) == 3);
    CHECK(w[0]->doc == e.docs[0] && w[1]->doc == e.docs[0] && w[2]->doc == e.docs[1]);
    CHECK(e.layout.active == w[2] && e.cy == 1 && e.cx == 1);
    CHECK(e.layout.root->vertical && !e.layout.root->b->vertical);
    editor_free(&e);

    unsetenv("XDG_STATE_HOME");
    rm_rf(dir);
}

/* ---- phase 5: search ------------------------------------------------------ */

static int find1(Matcher *m, const char *s, size_t from, size_t *a, size_t *b)
{
    return sm_find(m, s, strlen(s), from, a, b, NULL);
}

static void test_fold(void)
{
    CHECK(cp_fold('A') == 'a' && cp_fold('z') == 'z' && cp_fold('1') == '1');
    CHECK(cp_fold(0xC9) == 0xE9);                       /* É -> é */
    CHECK(cp_fold(0xD7) == 0xD7);                       /* multiplication sign untouched */
    CHECK(cp_fold(0x416) == 0x436);                     /* Ж -> ж */
    CHECK(cp_fold(0x401) == 0x451);                     /* Ё -> ё */
    CHECK(cp_fold(0x3A3) == 0x3C3);                     /* Σ -> σ */
    CHECK(cp_fold(0x100) == 0x101);
}

static void test_search_lines(void)
{
    Matcher m;
    size_t a, b;
    regmatch_t g[10];
    size_t rng[8][2];

    memset(&m, 0, sizeof m);
    CHECK(sm_compile(&m, "", 0, 0, 0, 0) < 0 && !m.valid);
    CHECK(sm_compile(&m, "foo", 3, 0, 0, 0) == 0);
    CHECK(find1(&m, "a Foo b FOO", 0, &a, &b) && a == 2 && b == 5);
    CHECK(find1(&m, "a Foo b FOO", 3, &a, &b) && a == 8 && b == 11);
    CHECK(!find1(&m, "a Foo b FOO", 9, &a, &b));
    CHECK(sm_compile(&m, "foo", 3, 0, 1, 0) == 0);
    CHECK(find1(&m, "a Foo b foo", 0, &a, &b) && a == 8);   /* match case */
    CHECK(sm_compile(&m, "caf\xC3\x89", 5, 0, 0, 0) == 0);  /* "cafÉ" finds "café" */
    CHECK(find1(&m, "un caf\xC3\xA9 ok", 0, &a, &b) && a == 3 && b == 8);
    CHECK(sm_compile(&m, "\xD0\x96\xD0\xA3", 4, 0, 0, 0) == 0);       /* ЖУ */
    CHECK(find1(&m, "x \xD0\xB6\xD1\x83 y", 0, &a, &b) && a == 2 && b == 6);

    CHECK(sm_compile(&m, "cat", 3, 0, 0, 1) == 0);                    /* whole word */
    CHECK(find1(&m, "concat cat cats", 0, &a, &b) && a == 7 && b == 10);
    CHECK(!find1(&m, "concat cats", 0, &a, &b));
    CHECK(find1(&m, "(cat)", 0, &a, &b) && a == 1);

    CHECK(sm_compile(&m, "a*", 2, 1, 0, 0) == 0);                     /* no empty matches */
    CHECK(!find1(&m, "xyz", 0, &a, &b));
    CHECK(find1(&m, "xaay", 0, &a, &b) && a == 1 && b == 3);

    CHECK(sm_compile(&m, "\\d+", 3, 1, 0, 0) == 0);
    CHECK(find1(&m, "ab 123 c4", 0, &a, &b) && a == 3 && b == 6);
    CHECK(find1(&m, "ab 123 c4", 6, &a, &b) && a == 8 && b == 9);
    CHECK(sm_compile(&m, "\\w+@\\w+", 7, 1, 0, 0) == 0);
    CHECK(find1(&m, "mail a_b@cd now", 0, &a, &b) && a == 5 && b == 11);
    CHECK(sm_compile(&m, "[\\d]+", 5, 1, 0, 0) == 0);                 /* brackets are literal */
    CHECK(find1(&m, "x\\d9", 0, &a, &b) && a == 1 && b == 3);
    CHECK(sm_compile(&m, "^ab", 3, 1, 0, 0) == 0);
    CHECK(find1(&m, "ab ab", 0, &a, &b) && a == 0);
    CHECK(!find1(&m, "ab ab", 1, &a, &b));                            /* ^ only at line start */
    CHECK(sm_compile(&m, "(a+)-(b+)", 9, 1, 0, 0) == 0);
    CHECK(sm_find(&m, "xx aa-bbb", 9, 0, &a, &b, g) && g[1].rm_so == 3 && g[1].rm_eo == 5 &&
          g[2].rm_so == 6 && g[2].rm_eo == 9);
    CHECK(sm_compile(&m, "(", 1, 1, 0, 0) < 0 && !m.valid && m.err[0] != '\0');
    CHECK(sm_compile(&m, "[a", 2, 1, 0, 0) < 0);

    CHECK(sm_compile(&m, "ab", 2, 0, 0, 0) == 0);
    CHECK(sm_find_last(&m, "ab ab ab", 8, 8, &a, &b) && a == 6);
    CHECK(sm_find_last(&m, "ab ab ab", 8, 6, &a, &b) && a == 3);
    CHECK(!sm_find_last(&m, "ab ab ab", 8, 0, &a, &b));
    CHECK(sm_line_matches(&m, "ab ab ab", 8, rng, 8) == 3 && rng[1][0] == 3 && rng[2][1] == 8);
    CHECK(sm_line_matches(&m, "ab ab ab", 8, rng, 2) == 2);
    CHECK(sm_line_matches(&m, "", 0, rng, 8) == 0);
    sm_free(&m);
}

static Buffer *buf_of(const char *text)
{
    Buffer *b = xmalloc(sizeof *b);
    size_t y = 0, x = 0;

    buf_init(b);
    buf_insert_text(b, &y, &x, text, strlen(text));
    buf_undo_close(b);
    return b;
}

static void test_search_buffer(void)
{
    Buffer *b = buf_of("one two\nthree two\nfour\ntwo");
    Matcher m;
    Hit h;
    int wr;
    size_t before, total;
    Span lim;

    memset(&m, 0, sizeof m);
    sm_compile(&m, "two", 3, 0, 0, 0);
    CHECK(search_next(&m, b, 0, 0, 1, 1, NULL, &h, &wr) && h.y == 0 && h.x == 4 && !wr);
    CHECK(search_next(&m, b, 0, 5, 1, 1, NULL, &h, &wr) && h.y == 1 && h.x == 6);
    CHECK(search_next(&m, b, 1, 7, 1, 1, NULL, &h, &wr) && h.y == 3 && h.x == 0 && !wr);
    CHECK(search_next(&m, b, 3, 1, 1, 1, NULL, &h, &wr) && h.y == 0 && h.x == 4 && wr);
    CHECK(!search_next(&m, b, 3, 1, 1, 0, NULL, &h, &wr));
    CHECK(search_next(&m, b, 3, 0, 0, 1, NULL, &h, &wr) && h.y == 1 && h.x == 6 && !wr);
    CHECK(search_next(&m, b, 0, 4, 0, 1, NULL, &h, &wr) && h.y == 3 && wr);
    CHECK(!search_next(&m, b, 0, 4, 0, 0, NULL, &h, &wr));
    CHECK(search_next(&m, b, 1, 6, 1, 1, NULL, &h, &wr) && h.y == 1 && h.x == 6);   /* at the cursor */
    search_stats(&m, b, 1, 6, 1000, &before, &total);
    CHECK(total == 3 && before == 1);
    search_stats(&m, b, 0, 0, 2, &before, &total);
    CHECK(total == 2);

    lim.y0 = 1; lim.x0 = 0; lim.y1 = 1; lim.x1 = 9;                 /* only "three two" */
    CHECK(search_next(&m, b, 1, 0, 1, 1, &lim, &h, &wr) && h.y == 1);
    CHECK(search_next(&m, b, 1, 7, 1, 1, &lim, &h, &wr) && h.y == 1 && h.x == 6 && wr);
    lim.x1 = 8;                                                      /* cuts the match in half */
    CHECK(!search_next(&m, b, 1, 0, 1, 1, &lim, &h, &wr));
    sm_free(&m);
    buf_free(b);
    free(b);
}

static void test_search_replace(void)
{
    Buffer *b = buf_of("a1 b22\nc333 a4\nnone");
    Matcher m;
    Span lim;
    SBuf o = SBUF_INIT;
    regmatch_t g[10];
    size_t a, e;

    memset(&m, 0, sizeof m);
    sm_compile(&m, "(\\w)(\\d+)", 9, 1, 0, 0);
    CHECK(sm_find(&m, "xx b22", 6, 0, &a, &e, g));
    search_expand(&m, "xx b22", g, "<\\2\\1\\0\\\\\\t>", &o);
    CHECK(o.len == 10 && memcmp(o.p, "<22bb22\\\t>", 10) == 0);
    sbuf_free(&o);

    buf_undo_begin(b, 0, 0);
    CHECK(search_replace_all(b, &m, "\\2\\1", NULL) == 4);
    buf_undo_close(b);
    CHECK(buf_is(b, "1a 22b\n333c 4a\nnone"));
    CHECK(buf_undo(b, &a, &e) == 1 && buf_is(b, "a1 b22\nc333 a4\nnone"));   /* one undo step */
    CHECK(buf_redo(b, &a, &e) == 1 && buf_is(b, "1a 22b\n333c 4a\nnone"));
    sm_free(&m);

    sm_compile(&m, "a", 1, 0, 1, 0);                                  /* limited to a span */
    lim.y0 = 0; lim.x0 = 0; lim.y1 = 1; lim.x1 = 7;
    buf_undo_begin(b, 0, 0);
    CHECK(search_replace_all(b, &m, "AAA", &lim) == 2);
    buf_undo_close(b);
    CHECK(buf_is(b, "1AAA 22b\n333c 4AAA\nnone") && lim.x1 == 9);
    sm_free(&m);

    sm_compile(&m, "o", 1, 0, 0, 0);                                  /* replacement may be empty */
    buf_undo_begin(b, 0, 0);
    CHECK(search_replace_all(b, &m, "", NULL) == 1);
    buf_undo_close(b);
    CHECK(b->lines[2].len == 3 && memcmp(b->lines[2].s, "nne", 3) == 0);
    sm_free(&m);
    sm_compile(&m, "e", 1, 0, 0, 0);                                  /* longer text, no loop */
    buf_undo_begin(b, 0, 0);
    CHECK(search_replace_all(b, &m, "ee", NULL) == 1);
    buf_undo_close(b);
    CHECK(b->lines[2].len == 4 && memcmp(b->lines[2].s, "nnee", 4) == 0);
    sm_free(&m);
    buf_free(b);
    free(b);
}

static void test_search_files(void)
{
    char dir[64];
    Matcher m;
    FileHits h;
    size_t i;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    {
        char sub[600];

        snprintf(sub, sizeof sub, "%s/src", dir);
        mkdir(sub, 0755);
        snprintf(sub, sizeof sub, "%s/.git", dir);
        mkdir(sub, 0755);
    }
    put_file(dir, "a.txt", "nothing\n  Needle here\nmore needle\n");
    put_file(dir, "src/b.c", "int needle;\r\nint x;\r\n");
    put_file(dir, ".git/c", "needle\n");
    put_file(dir, "bin.dat", "needle\n");
    {
        char path[700];
        FILE *f;

        snprintf(path, sizeof path, "%s/bin.dat", dir);
        f = fopen(path, "wb");
        fwrite("needle\0\0", 1, 8, f);
        fclose(f);
    }
    memset(&m, 0, sizeof m);
    sm_compile(&m, "needle", 6, 0, 0, 0);
    memset(&h, 0, sizeof h);
    search_dir(&m, dir, NULL, NULL, &h, 100);
    CHECK(h.n == 3 && !h.truncated);
    for (i = 0; i < h.n; i++)
        CHECK(strstr(h.v[i].path, "/.git/") == NULL && strstr(h.v[i].path, "bin.dat") == NULL);
    CHECK(h.n == 3 && h.v[0].line == 1 && h.v[0].col == 2 && strcmp(h.v[0].text, "Needle here") == 0);
    CHECK(h.n == 3 && h.v[1].line == 2 && strcmp(h.v[1].text, "more needle") == 0);
    CHECK(h.n == 3 && strcmp(h.v[2].text, "int needle;") == 0);      /* CRLF stripped */
    filehits_free(&h);

    memset(&h, 0, sizeof h);
    search_dir(&m, dir, NULL, NULL, &h, 2);
    CHECK(h.n == 2 && h.truncated);
    filehits_free(&h);

    sm_free(&m);
    {
        Buffer *b = buf_of("x\nneedle y\nz");

        sm_compile(&m, "needle", 6, 0, 0, 0);
        memset(&h, 0, sizeof h);
        search_buffer(&m, "mem", b, &h, 10);
        CHECK(h.n == 1 && h.v[0].line == 1 && strcmp(h.v[0].path, "mem") == 0);
        filehits_free(&h);
        buf_free(b);
        free(b);
    }
    sm_free(&m);
    rm_rf(dir);
}

static void test_goto_parse(void)
{
    size_t l, c;

    CHECK(search_parse_goto("12", &l, &c) == 0 && l == 12 && c == 0);
    CHECK(search_parse_goto(" 12:5 ", &l, &c) == 0 && l == 12 && c == 5);
    CHECK(search_parse_goto(":7", &l, &c) == 0 && l == 0 && c == 7);
    CHECK(search_parse_goto("", &l, &c) < 0);
    CHECK(search_parse_goto("abc", &l, &c) < 0);
    CHECK(search_parse_goto("12:", &l, &c) < 0);
    CHECK(search_parse_goto("12x", &l, &c) < 0);
}

static void test_comments(void)
{
    Editor e;
    const char *o, *c;

    {
        Syntax *sc = lang("c"), *sm = lang("md");

        CHECK(syn_comment(sc, &o, &c) && strcmp(o, "//") == 0 && !c);
        CHECK(syn_comment(sm, &o, &c) && strcmp(o, "<!--") == 0 && strcmp(c, "-->") == 0);
        syn_free(sc);
        syn_free(sm);
    }
    CHECK(!syn_comment(NULL, &o, &c));

    /* single line, then back */
    ed_load(&e, "int x;\n    y = 1;");
    doc_set_syntax(e.doc, lang("c"));
    e.cx = 3;
    edit_toggle_comment(&e);
    CHECK(buf_is(e.buf, "// int x;\n    y = 1;") && e.cx == 6);
    edit_toggle_comment(&e);
    CHECK(buf_is(e.buf, "int x;\n    y = 1;") && e.cx == 3);
    edit_undo(&e);
    CHECK(buf_is(e.buf, "// int x;\n    y = 1;"));
    editor_free(&e);

    /* a selection: common indent, blank lines skipped, mixed state comments all */
    ed_load(&e, "  a\n\n    b\n// c");
    doc_set_syntax(e.doc, lang("c"));
    ed_sel(&e, 0, 0, 3, 0);
    edit_toggle_comment(&e);
    CHECK(buf_is(e.buf, "  // a\n\n  //   b\n// c"));
    ed_sel(&e, 0, 0, 3, 3);
    edit_toggle_comment(&e);
    CHECK(buf_is(e.buf, "  a\n\n    b\nc"));
    editor_free(&e);

    ed_load(&e, "  // a\n\n  //   b");
    doc_set_syntax(e.doc, lang("c"));
    ed_sel(&e, 0, 0, 2, 5);
    edit_toggle_comment(&e);
    CHECK(buf_is(e.buf, "  a\n\n    b"));
    editor_free(&e);

    /* block markers per line, and a line comment in Python */
    ed_load(&e, "text");
    doc_set_syntax(e.doc, lang("md"));
    edit_toggle_comment(&e);
    CHECK(buf_is(e.buf, "<!-- text -->"));
    edit_toggle_comment(&e);
    CHECK(buf_is(e.buf, "text"));
    editor_free(&e);

    ed_load(&e, "");
    doc_set_syntax(e.doc, lang("py"));
    edit_toggle_comment(&e);
    CHECK(buf_is(e.buf, "# "));
    editor_free(&e);

    /* no language: nothing happens */
    ed_load(&e, "abc");
    edit_toggle_comment(&e);
    CHECK(buf_is(e.buf, "abc") && !e.buf->modified);
    editor_free(&e);
}

static void test_commands(void)
{
    size_t i, j;
    Editor e;

    CHECK(ncommands > 20);
    for (i = 0; i < ncommands; i++) {
        CHECK(commands[i].name && commands[i].name[0] && commands[i].id && commands[i].id[0] && commands[i].key && commands[i].fn);
        for (j = i + 1; j < ncommands; j++) {
            CHECK(strcmp(commands[i].name, commands[j].name) != 0);
            CHECK(strcmp(commands[i].id, commands[j].id) != 0);
            CHECK(commands[i].key[0] == '\0' || strcmp(commands[i].key, commands[j].key) != 0);
        }
    }
    /* the palette lists them all, in order, so the chosen index is the table index */
    ed_load(&e, "x");
    editor_cmd_palette(&e);
    CHECK(e.picker.on && e.picker_kind == PK_COMMANDS && e.picker.nitems == ncommands);
    CHECK(strstr(e.picker.items[0], commands[0].name) != NULL);
    picker_close(&e.picker);
    editor_free(&e);
}

static void test_complete(void)
{
    Editor e;
    Complete c;
    size_t start;

    memset(&c, 0, sizeof c);
    ed_load(&e, "alpha_beta alphabet\nint al\nAlbatross");
    doc_set_syntax(e.doc, lang("c"));
    CHECK(complete_prefix(e.buf, 1, 6, &start) == 2 && start == 4);
    CHECK(complete_prefix(e.buf, 1, 4, &start) == 0 && start == 4);

    /* words of the buffer and keywords; the word being typed is not offered */
    CHECK(complete_open(&c, e.docs, e.ndocs, e.doc, 1, 6) && c.on);
    CHECK(c.n >= 2 && c.y == 1 && c.x == 4);
    {
        size_t i, nbeta = 0, nself = 0;

        for (i = 0; i < c.n; i++) {
            if (strcmp(c.v[i].text, "alpha_beta") == 0 || strcmp(c.v[i].text, "alphabet") == 0)
                nbeta++;
            if (strcmp(c.v[i].text, "al") == 0)
                nself++;
        }
        CHECK(nbeta == 2 && nself == 0);
    }
    /* first letter must agree: "Albatross" matches, "int" does not */
    {
        size_t i, alb = 0, in = 0;

        for (i = 0; i < c.n; i++) {
            if (strcmp(c.v[i].text, "Albatross") == 0)
                alb = 1;
            if (strcmp(c.v[i].text, "int") == 0)
                in = 1;
        }
        CHECK(alb == 1 && in == 0);
    }
    /* keywords come from the language, marked as such */
    complete_close(&c);
    {
        size_t y = 0, x = 0;
        Editor k;

        ed_load(&k, "");
        doc_set_syntax(k.doc, lang("c"));
        buf_insert_text(&k.doc->buf, &y, &x, "retu", 4);
        CHECK(complete_open(&c, k.docs, k.ndocs, k.doc, 0, 4));
        CHECK(c.n == 1 && strcmp(c.v[0].text, "return") == 0 && c.v[0].kind == CAND_KEYWORD);
        complete_close(&c);
        editor_free(&k);
    }
    complete_close(&c);
    CHECK(!c.on && c.n == 0);

    /* too short a prefix opens nothing */
    CHECK(!complete_open(&c, e.docs, e.ndocs, e.doc, 1, 5));

    /* selection cycles */
    CHECK(complete_open(&c, e.docs, e.ndocs, e.doc, 1, 6));
    complete_move(&c, -1);
    CHECK(c.sel == c.n - 1);
    complete_move(&c, 1);
    CHECK(c.sel == 0);
    complete_free(&c);

    /* accepting replaces the prefix, as one undo step */
    e.cy = 1;
    e.cx = 6;
    edit_replace_word(&e, 4, "alphabet", 8);
    CHECK(line_is(e.buf, 1, "int alphabet") && e.cx == 12);
    edit_undo(&e);
    CHECK(line_is(e.buf, 1, "int al"));
    editor_free(&e);

    /* other documents contribute their words; keywords are suggested by kind */
    {
        Editor f;
        Doc *d2 = doc_new();
        size_t y = 0, x = 0;
        char *txt = "zebra_crossing";

        ed_load(&f, "ze");
        buf_insert_text(&d2->buf, &y, &x, txt, strlen(txt));
        f.docs = xrealloc(f.docs, 2 * sizeof *f.docs);
        f.docs[1] = d2;
        f.ndocs = 2;
        memset(&c, 0, sizeof c);
        CHECK(complete_open(&c, f.docs, f.ndocs, f.doc, 0, 2));
        CHECK(c.n == 1 && strcmp(c.v[0].text, "zebra_crossing") == 0 && c.v[0].kind == CAND_WORD);
        complete_free(&c);
        f.ndocs = 1;
        doc_free(d2);
        editor_free(&f);
    }
}

static void test_snippets(void)
{
    SBuf t = SBUF_INIT;
    SnipStops st;
    SnipDef def;
    Editor e;
    size_t n;

    /* parsing: order 1..9 then 0, defaults, escaped dollar, implicit end */
    snip_parse("a ${2:bb} $1 $$ ${3:c} $0 z", &t, &st);
    CHECK(t.len == 12 && memcmp(t.p, "a bb  $ c  z", 12) == 0);
    CHECK(st.n == 4);
    CHECK(st.stop[0].off == 5 && st.stop[0].len == 0);     /* $1 */
    CHECK(st.stop[1].off == 2 && st.stop[1].len == 2);     /* ${2:bb} */
    CHECK(st.stop[2].off == 8 && st.stop[2].len == 1);     /* ${3:c} */
    CHECK(st.stop[3].off == 10 && st.stop[3].len == 0);    /* $0 */
    sbuf_free(&t);
    snip_parse("x $1 $1", &t, &st);
    CHECK(st.n == 2 && st.stop[0].off == 2 && st.stop[1].off == t.len);
    sbuf_free(&t);
    CHECK(snip_lang("C", &n) != NULL && n > 5);
    CHECK(snip_lang("Markdown", &n) == NULL && n == 0);
    CHECK(snip_lang("PHP", &n) != NULL && n > 5);
    CHECK(snip_lang("TypeScript", &n) == snip_lang("JavaScript", &n) && n > 5);

    /* expansion keeps the indentation and selects the first stop */
    ed_load(&e, "  if (x) {\n    fo\n  }");
    def.name = "for";
    def.body = "for (${1:i}) {\n\t$0\n}";
    e.cy = 1;
    e.cx = 6;
    edit_expand_snippet(&e, 4, &def);
    CHECK(buf_is(e.buf, "  if (x) {\n    for (i) {\n    \t\n    }\n  }"));
    CHECK(e.snip.on && e.snip.n == 2 && e.sel_on);
    CHECK(e.ay == 1 && e.ax == 9 && e.cy == 1 && e.cx == 10);
    edit_insert_cp(&e, 'k');
    edit_undo(&e);
    CHECK(buf_is(e.buf, "  if (x) {\n    for (i) {\n    \t\n    }\n  }"));
    edit_undo(&e);
    CHECK(buf_is(e.buf, "  if (x) {\n    fo\n  }"));
    editor_free(&e);

    /* adjust: typing in a stop grows it and shifts the following ones; leaving ends it */
    {
        SnipSession s;

        memset(&s, 0, sizeof s);
        s.on = 1;
        s.n = 3;
        s.stop[0].y = 2; s.stop[0].x = 4; s.stop[0].len = 3;
        s.stop[1].y = 2; s.stop[1].x = 10; s.stop[1].len = 1;
        s.stop[2].y = 4; s.stop[2].x = 0; s.stop[2].len = 0;
        CHECK(snip_adjust(&s, 6, 6, 20, 22, 2, 9));
        CHECK(s.stop[0].len == 5 && s.stop[1].x == 12 && s.stop[2].x == 0);
        CHECK(snip_adjust(&s, 6, 6, 22, 21, 2, 8));
        CHECK(s.stop[0].len == 4 && s.stop[1].x == 11);
        CHECK(!snip_adjust(&s, 6, 6, 21, 21, 2, 3));        /* cursor left the stop */
        CHECK(!s.on);
        s.on = 1;
        CHECK(!snip_adjust(&s, 6, 7, 21, 21, 2, 6));        /* a line was added */
    }
}

static void fn_insert(Editor *e, void *a, size_t idx)
{
    (void)idx;
    edit_insert_cp(e, *(uint32_t *)a);
}

static void fn_newline(Editor *e, void *a, size_t idx)
{
    (void)a;
    (void)idx;
    edit_newline(e);
    buf_undo_close(e->buf);
}

static void fn_backspace(Editor *e, void *a, size_t idx)
{
    (void)a;
    (void)idx;
    edit_backspace(e);
}

static void test_cursors(void)
{
    Editor e;
    uint32_t cp = 'Z';
    size_t n;
    char *t;

    /* Ctrl+D: first press selects the word, the next ones add occurrences, wrapping */
    ed_load(&e, "foo bar foo\nxx foo\nlast");
    cur_add_next(&e);
    CHECK(e.sel_on && e.cx == 3 && !cur_multi(&e));
    cur_add_next(&e);
    CHECK(cur_count(&e) == 2 && e.cy == 0 && e.cx == 11);
    cur_add_next(&e);
    CHECK(cur_count(&e) == 3 && e.cy == 1 && e.cx == 6);
    cur_add_next(&e);                                  /* all taken */
    CHECK(cur_count(&e) == 3);
    t = cur_selected_text(&e, &n);
    CHECK(t && n == 11 && memcmp(t, "foo\nfoo\nfoo", 11) == 0);
    free(t);

    /* typing replaces every selection; one undo step restores all */
    cur_each(&e, fn_insert, &cp);
    buf_undo_close(e.buf);
    CHECK(buf_is(e.buf, "Z bar Z\nxx Z\nlast") && cur_count(&e) == 3);
    cur_each(&e, fn_insert, &cp);
    CHECK(buf_is(e.buf, "ZZ bar ZZ\nxx ZZ\nlast"));
    buf_undo_close(e.buf);
    edit_undo(&e);
    CHECK(buf_is(e.buf, "Z bar Z\nxx Z\nlast"));      /* each key run is one step for all cursors */
    edit_undo(&e);
    CHECK(buf_is(e.buf, "foo bar foo\nxx foo\nlast") && !cur_multi(&e));
    editor_free(&e);

    /* Alt+D takes them all at once */
    ed_load(&e, "ab ab ab\nab");
    cur_add_all(&e);
    CHECK(cur_count(&e) == 4);
    cur_each(&e, fn_newline, NULL);
    CHECK(buf_is(e.buf, "\n \n \n\n\n") && e.buf->nlines == 6);
    editor_free(&e);

    /* several cursors on one line, and lines splitting: positions stay right */
    ed_load(&e, "abcdef\nxyz");
    e.cx = 2;
    {
        Cursor c;

        memset(&c, 0, sizeof c);
        c.cy = 0;
        c.cx = 4;
        e.xc = xmalloc(sizeof c);
        e.xc[0] = c;
        e.nxc = e.xc_cap = 1;
        c.cy = 1;
        c.cx = 1;
        e.xc = xrealloc(e.xc, 2 * sizeof c);
        e.xc[1] = c;
        e.nxc = e.xc_cap = 2;
    }
    cur_each(&e, fn_newline, NULL);
    CHECK(buf_is(e.buf, "ab\ncd\nef\nx\nyz"));
    CHECK(cur_count(&e) == 3 && e.cy == 0 + 1 && e.cx == 0);
    cur_each(&e, fn_backspace, NULL);
    CHECK(buf_is(e.buf, "abcdef\nxyz") && cur_count(&e) == 3);
    editor_free(&e);

    /* cursors that meet merge */
    ed_load(&e, "abc");
    e.cx = 2;
    e.xc = xmalloc(sizeof *e.xc);
    memset(e.xc, 0, sizeof *e.xc);
    e.xc[0].cx = 1;
    e.nxc = e.xc_cap = 1;
    cur_each(&e, fn_backspace, NULL);
    CHECK(buf_is(e.buf, "c") && !cur_multi(&e) && e.cx == 0);
    editor_free(&e);

    /* vertical: same column, skipping lines that are too short does not move it */
    ed_load(&e, "abcdef\nab\nabcdef");
    e.cx = 4;
    e.want_col = 4;
    cur_add_vertical(&e, 1);
    cur_add_vertical(&e, 1);
    CHECK(cur_count(&e) == 3 && e.cy == 2 && e.cx == 4);
    cur_add_vertical(&e, 1);                           /* no line below: nothing */
    CHECK(cur_count(&e) == 3);
    editor_free(&e);
}

static void test_folding(void)
{
    Editor e;
    size_t last = 0;
    const char *c_text = "int f(void)\n{\n    if (x) {\n        a();\n        b();\n    }\n"
                         "    return 0;\n}\n\nint g;";
    const char *py_text = "def f():\n    a = 1\n\n    b = 2\n\nx = 3\n";

    /* by bracket: the closing line stays visible; Allman braces fold from the brace line */
    ed_load(&e, c_text);
    doc_set_syntax(e.doc, lang("c"));
    CHECK(fold_range(e.doc->syn, e.buf, 1, &last) && last == 6);
    CHECK(fold_range(e.doc->syn, e.buf, 2, &last) && last == 4);
    CHECK(!fold_range(e.doc->syn, e.buf, 0, &last));          /* "{" is on the next line */
    CHECK(!fold_range(e.doc->syn, e.buf, 3, &last));
    CHECK(!fold_range(e.doc->syn, e.buf, 9, &last));
    editor_free(&e);

    /* by indentation; blank lines inside count, trailing ones do not */
    ed_load(&e, py_text);
    doc_set_syntax(e.doc, lang("py"));
    CHECK(fold_range(e.doc->syn, e.buf, 0, &last) && last == 3);
    CHECK(!fold_range(e.doc->syn, e.buf, 5, &last));
    editor_free(&e);

    /* folding hides lines from every walk; the cursor goes to the head */
    ed_load(&e, c_text);
    doc_set_syntax(e.doc, lang("c"));
    e.cy = 4;
    fold_toggle(&e);                                   /* innermost block containing line 4 */
    CHECK(e.cy == 2 && e.buf->lines[2].fold == 2 && e.buf->folded);
    CHECK(fold_next(e.buf, 2) == 5 && fold_prev(e.buf, 5) == 2);
    CHECK(fold_head(e.buf, 3) == 2 && fold_head(e.buf, 4) == 2 && fold_head(e.buf, 5) == 5);
    CHECK(fold_count(e.buf, 0, 6) == 4 && fold_count(e.buf, 0, 10) == 8);
    CHECK(fold_nth(e.buf, 0, 3) == 5 && fold_nth(e.buf, 0, 20) == e.buf->nlines);
    CHECK(fold_advance(e.buf, 1, 2) == 5 && fold_back(e.buf, 5, 2) == 1);
    CHECK(fold_back(e.buf, 5, 100) == 0 && fold_advance(e.buf, 8, 5) == 9);

    /* an outer fold hides the inner one, which stays folded when the outer opens */
    e.cy = 1;
    fold_toggle(&e);
    CHECK(e.buf->lines[1].fold == 5 && fold_next(e.buf, 1) == 7);
    e.buf->lines[1].fold = 0;
    CHECK(e.buf->lines[2].fold == 2);

    /* something that moves the cursor into a fold opens it */
    e.cy = 3;
    fold_reveal(&e);
    CHECK(e.buf->lines[2].fold == 0);
    fold_all(&e);
    CHECK(e.buf->lines[1].fold == 5 && e.buf->lines[8].fold == 0);
    unfold_all(&e);
    CHECK(!e.buf->folded && e.buf->lines[1].fold == 0);
    e.cy = 3;
    fold_toggle(&e);                                   /* cursor on line 3: the "if" block */
    CHECK(e.buf->lines[2].fold == 2);
    fold_toggle(&e);
    CHECK(e.buf->lines[2].fold == 0);
    editor_free(&e);

    /* a folded line keeps its fold across line insertion above it */
    ed_load(&e, "a {\n  b\n}\nc");
    doc_set_syntax(e.doc, lang("c"));
    fold_toggle(&e);
    CHECK(e.buf->lines[0].fold == 1);
    buf_split(e.buf, 0, 0);
    CHECK(e.buf->lines[0].fold == 1 && e.buf->lines[1].fold == 0);
    editor_free(&e);
}

static void msleep(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int wait_job(Job *j, SBuf *out)
{
    int i;

    for (i = 0; i < 500; i++) {
        char buf[256];
        long n = shell_read(j, buf, sizeof buf);

        if (n > 0)
            sbuf_add(out, buf, (size_t)n);
        else if (n == 0 && shell_reap(j))
            return 1;
        else
            msleep(10);
    }
    return 0;
}

static void test_shell(void)
{
    SBuf out = SBUF_INIT, err = SBUF_INIT;
    const char *cat[] = { "/bin/sh", "-c", "cat", NULL };
    const char *two[] = { "/bin/sh", "-c", "echo out; echo bad >&2; exit 3", NULL };
    const char *slow[] = { "/bin/sh", "-c", "sleep 5", NULL };
    const char *big[] = { "/bin/sh", "-c", "head -c 100000 /dev/zero", NULL };
    const char *bad[] = { "/nonexistent/prog", NULL };
    int st = 0;
    Job j;
    char e[128];
    long t0;

    /* stdin to stdout, with something bigger than a pipe buffer */
    {
        char *in = xmalloc(300000);

        memset(in, 'x', 300000);
        CHECK(shell_capture(cat, NULL, in, 300000, &out, &err, 5000, 1 << 20, &st) == 0);
        CHECK(out.len == 300000 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        free(in);
    }
    sbuf_free(&out);
    sbuf_free(&err);

    /* stdout and stderr apart, exit status */
    CHECK(shell_capture(two, NULL, NULL, 0, &out, &err, 5000, 1 << 20, &st) == 0);
    CHECK(out.len == 4 && memcmp(out.p, "out\n", 4) == 0);
    CHECK(err.len == 4 && memcmp(err.p, "bad\n", 4) == 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3);
    sbuf_free(&out);
    sbuf_free(&err);

    /* a command that is too slow is killed */
    t0 = (long)time(NULL);
    CHECK(shell_capture(slow, NULL, NULL, 0, &out, NULL, 300, 1 << 20, &st) == -2);
    CHECK((long)time(NULL) - t0 < 4);
    sbuf_free(&out);

    /* output over the cap is dropped, not an error */
    CHECK(shell_capture(big, NULL, NULL, 0, &out, NULL, 5000, 1000, &st) == 0 && out.len == 1000);
    sbuf_free(&out);

    /* a program that does not exist: the shell-less exec fails with status 127 */
    CHECK(shell_capture(bad, NULL, NULL, 0, &out, NULL, 2000, 100, &st) == 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 127);
    sbuf_free(&out);

    /* a background job: output on one pipe, working directory, exit code */
    CHECK(shell_start(&j, "pwd; echo err >&2; exit 4", "/tmp", e, sizeof e) == 0);
    CHECK(wait_job(&j, &out) && shell_exit_code(&j) == 4);
    CHECK(out.len == 9 && memcmp(out.p, "/tmp\nerr\n", 9) == 0);
    shell_close(&j);
    sbuf_free(&out);

    /* stop ends the whole group, including children of the shell */
    CHECK(shell_start(&j, "sleep 30 & sleep 30", NULL, e, sizeof e) == 0);
    msleep(50);
    CHECK(!shell_reap(&j));
    shell_stop(&j);
    CHECK(wait_job(&j, &out) && shell_exit_code(&j) == 128 + 15);
    shell_close(&j);
    sbuf_free(&out);

    /* closing a running job kills it */
    CHECK(shell_start(&j, "sleep 30", NULL, e, sizeof e) == 0);
    shell_close(&j);
    CHECK(j.done && j.fd < 0);
}

static int parse_err(const char *line, const char *path, size_t ln, size_t col)
{
    size_t po, pl, l, c;

    if (!err_parse(line, strlen(line), &po, &pl, &l, &c))
        return 0;
    return strlen(path) == pl && memcmp(line + po, path, pl) == 0 && l == ln && c == col;
}

static void test_errors_parse(void)
{
    size_t po, pl, l, c;

    CHECK(parse_err("src/main.c:12:5: error: expected ';'", "src/main.c", 12, 5));
    CHECK(parse_err("a.c:7: warning: unused", "a.c", 7, 0));
    CHECK(parse_err("./lib/x.h:3:1: note: here", "./lib/x.h", 3, 1));
    CHECK(parse_err("In file included from foo/bar.h:10:", "foo/bar.h", 10, 0));
    CHECK(parse_err("/abs/path/f.py:99:", "/abs/path/f.py", 99, 0));
    CHECK(parse_err("file.txt:5:some grep hit", "file.txt", 5, 0));
    CHECK(parse_err("src/a.ts(14,3): error TS2304: x", "src/a.ts", 14, 3));
    CHECK(parse_err("src/a.ts(14): error", "src/a.ts", 14, 0));
    CHECK(parse_err("  File \"/tmp/t.py\", line 8, in <module>", "/tmp/t.py", 8, 0));
    CHECK(parse_err("make: *** [all] Error 1 and then a.c:3:2: boom", "a.c", 3, 2));
    /* not locations */
    CHECK(!err_parse("at 12:30 we left", 16, &po, &pl, &l, &c));
    CHECK(!err_parse("error:3", 7, &po, &pl, &l, &c));
    CHECK(!err_parse("plain text", 10, &po, &pl, &l, &c));
    CHECK(!err_parse("a.c:x:1: nope", 13, &po, &pl, &l, &c));
    CHECK(!err_parse("", 0, &po, &pl, &l, &c));
    CHECK(!err_parse("a.c:12abc", 9, &po, &pl, &l, &c));
}

static void wait_run(Editor *e)
{
    int i;

    for (i = 0; i < 300 && e->run.on; i++) {
        run_pump(e);
        if (e->run.on)
            msleep(10);
    }
}

static void test_run(void)
{
    Editor e;
    char dir[] = "/tmp/ked_run_XXXXXX";
    char path[300];
    FILE *f;
    size_t i;

    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/x.c", dir);
    f = fopen(path, "w");
    fputs("one\ntwo\nthree\nfour\nfive\n", f);
    fclose(f);

    ed_load(&e, "text");
    e.root = xstrdup(dir);
    run_command(&e, "printf 'x.c:4:2: error: y\\n\\033[31mred\\033[0m\\r\\nq.c:1: no file\\n'; exit 3");
    CHECK(e.run.on && e.run.out && layout_count(&e.layout) == 2);
    wait_run(&e);
    CHECK(!e.run.on);
    {
        Buffer *b = &e.run.out->buf;

        CHECK(b->nlines >= 5 && line_is(b, 1, "x.c:4:2: error: y") && line_is(b, 2, "red"));
        CHECK(!b->modified);
        CHECK(b->nlines > 5 && strstr(b->lines[b->nlines - 2].s, "uscito con 3") != NULL);
        CHECK(line_is(b, 0, "$ printf 'x.c:4:2: error: y\\n\\033[31mred\\033[0m\\r\\nq.c:1: no file\\n'; exit 3"));
    }
    /* the focus stayed on the text pane and the output pane is a scratch buffer */
    CHECK(e.doc != e.run.out && e.run.out->scratch && e.run.out->readonly);

    /* F8 skips lines whose file does not exist and opens the next real one */
    run_next_error(&e);
    CHECK(e.doc->buf.path && strstr(e.doc->buf.path, "x.c") != NULL);
    CHECK(e.cy == 3 && e.cx == 1);
    run_next_error(&e);                                /* q.c does not exist: nothing more */
    CHECK(e.cy == 3);
    run_prev_error(&e);
    CHECK(e.cy == 3);

    /* appended text is cleaned: no escapes, no CR, controls shown as ? */
    {
        const char *junk = "\x1b[1mbold\x1b[0m\x01z\r\n";

        run_append(&e, junk, strlen(junk));
    }
    {
        Buffer *b = &e.run.out->buf;
        size_t n = b->nlines;

        CHECK(n >= 2 && line_is(b, n - 2, "bold?z"));
    }
    /* running again clears the output */
    run_command(&e, "echo second");
    wait_run(&e);
    CHECK(line_is(&e.run.out->buf, 1, "second"));

    /* running the same command again (F5) hands the stored text to itself */
    run_again(&e);
    wait_run(&e);
    CHECK(line_is(&e.run.out->buf, 0, "$ echo second") && line_is(&e.run.out->buf, 1, "second"));
    CHECK(strcmp(e.run.cmd, "echo second") == 0);

    /* stopping */
    run_command(&e, "sleep 30");
    CHECK(e.run.on);
    run_stop(&e);
    wait_run(&e);
    CHECK(!e.run.on);

    /* closing the output buffer while a command runs kills it */
    run_command(&e, "sleep 30");
    for (i = 0; i < e.ndocs; i++)
        if (e.docs[i] == e.run.out) {
            Doc *d = e.run.out;

            run_doc_removed(&e, d);
            CHECK(!e.run.on && e.run.out == NULL);
            e.docs[i] = doc_new();       /* keep the list valid; the test owns d now */
            doc_free(d);
            break;
        }
    editor_free(&e);
    snprintf(path, sizeof path, "rm -rf %s", dir);
    CHECK(system(path) == 0);
}

static void test_filter(void)
{
    Editor e;

    /* the selection goes through the command and is replaced, in one undo step */
    ed_load(&e, "pear\napple\nfig\nkiwi");
    ed_sel(&e, 0, 0, 2, 3);
    edit_filter_cmd(&e, "sort");
    CHECK(buf_is(e.buf, "apple\nfig\npear\nkiwi") && e.sel_on);
    edit_undo(&e);
    CHECK(buf_is(e.buf, "pear\napple\nfig\nkiwi"));
    editor_free(&e);

    /* no selection: the whole text; a final newline added by the command is not kept */
    ed_load(&e, "b\na\nc");
    edit_filter_cmd(&e, "sort");
    CHECK(buf_is(e.buf, "a\nb\nc") && !e.sel_on);
    edit_filter_cmd(&e, "tr a-z A-Z");
    CHECK(buf_is(e.buf, "A\nB\nC"));
    editor_free(&e);

    /* failure and timeout leave the text alone */
    ed_load(&e, "keep");
    edit_filter_cmd(&e, "exit 3");
    CHECK(buf_is(e.buf, "keep") && strstr(e.msg, "errore") != NULL);
    edit_filter_cmd(&e, "echo boom >&2; exit 1");
    CHECK(buf_is(e.buf, "keep") && strstr(e.msg, "boom") != NULL);
    CHECK(!e.buf->modified);
    editor_free(&e);
}

static Key mk(uint32_t code, int mods)
{
    Key k;

    k.code = code;
    k.mods = mods;
    return k;
}

static int parses(const char *spec, uint32_t code, int mods)
{
    Key k;
    char err[96];

    return key_parse(spec, &k, err, sizeof err) == 0 && k.code == code && k.mods == mods;
}

static void test_keymap(void)
{
    Keymap m;
    Key k;
    char err[96], out[48];
    size_t i;

    CHECK(parses("ctrl+s", CTRL('s'), 0));
    CHECK(parses("Ctrl+S", CTRL('s'), 0));
    CHECK(parses("alt+/", '/', MOD_ALT));
    CHECK(parses("alt+shift+up", KEY_UP, MOD_ALT | MOD_SHIFT));
    CHECK(parses("ctrl+tab", KEY_TAB, MOD_CTRL));
    CHECK(parses("ctrl+shift+tab", KEY_TAB, MOD_CTRL | MOD_SHIFT));
    CHECK(parses("shift+f3", KEY_F3, MOD_SHIFT));
    CHECK(parses("f12", KEY_F12, 0));
    CHECK(parses("alt+,", ',', MOD_ALT));
    CHECK(parses("alt++", '+', MOD_ALT));
    CHECK(parses("alt+D", 'd', MOD_ALT));
    CHECK(parses("alt+shift+D", 'd', MOD_ALT | MOD_SHIFT));
    CHECK(parses("ctrl+pgdn", KEY_PGDN, MOD_CTRL));
    CHECK(parses("alt+\xC3\xA8", 0xE8, MOD_ALT));
    CHECK(parses("f1", KEY_F1, 0));
    /* errors: unknown key or modifier, empty, F13, keys the terminal cannot tell apart */
    CHECK(key_parse("ctrl+", &k, err, sizeof err) < 0);
    CHECK(key_parse("hyper+x", &k, err, sizeof err) < 0 && strstr(err, "hyper") != NULL);
    CHECK(key_parse("alt+banana", &k, err, sizeof err) < 0);
    CHECK(key_parse("f13", &k, err, sizeof err) < 0);
    CHECK(key_parse("ctrl+i", &k, err, sizeof err) < 0);
    CHECK(key_parse("ctrl+m", &k, err, sizeof err) < 0);
    CHECK(key_parse("", &k, err, sizeof err) < 0);

    /* formatting is the inverse */
    key_format(mk(CTRL('s'), 0), 0, out, sizeof out);
    CHECK(strcmp(out, "ctrl+s") == 0);
    key_format(mk(KEY_UP, MOD_ALT | MOD_SHIFT), 1, out, sizeof out);
    CHECK(strcmp(out, "Alt+Shift+Up") == 0);
    key_format(mk(KEY_F5, MOD_SHIFT), 1, out, sizeof out);
    CHECK(strcmp(out, "Shift+F5") == 0);
    key_format(mk('/', MOD_ALT), 1, out, sizeof out);
    CHECK(strcmp(out, "Alt+/") == 0);

    /* normalisation: the Ctrl flag on a control code, Alt+capital */
    CHECK(key_norm(mk(CTRL('s'), MOD_CTRL)).mods == 0);
    CHECK(key_norm(mk('D', MOD_ALT)).code == 'd' && key_norm(mk('D', MOD_ALT)).mods == (MOD_ALT | MOD_SHIFT));
    CHECK(key_norm(mk(KEY_TAB, MOD_CTRL)).mods == MOD_CTRL);

    /* every default shortcut parses, and lands on its command */
    keymap_init(&m);
    for (i = 0; i < ncommands; i++) {
        if (!commands[i].key[0])
            continue;
        CHECK(key_parse(commands[i].key, &k, err, sizeof err) == 0);
        CHECK(keymap_find(&m, k) == (int)i);
    }
    CHECK(keymap_find(&m, mk(CTRL('s'), 0)) == command_by_id("save"));
    CHECK(keymap_find(&m, mk(CTRL('s'), MOD_CTRL)) == command_by_id("save"));
    CHECK(keymap_find(&m, mk(KEY_TAB, MOD_CTRL)) == command_by_id("next_win"));
    CHECK(keymap_find(&m, mk(KEY_RIGHT, MOD_ALT)) == command_by_id("next_win"));
    CHECK(keymap_find(&m, mk('n', MOD_ALT)) == command_by_id("find_next"));
    CHECK(keymap_find(&m, mk(KEY_UP, MOD_ALT | MOD_SHIFT)) == command_by_id("cursor_above"));
    CHECK(keymap_find(&m, mk('y', MOD_ALT)) == command_by_id("duplicate_down"));
    CHECK(keymap_find(&m, mk('x', 0)) == -1 && keymap_find(&m, mk(KEY_UP, 0)) == -1);
    CHECK(command_by_id("no_such") == -1);

    /* rebinding and unbinding */
    CHECK(key_parse("alt+j", &k, err, sizeof err) == 0);
    keymap_bind(&m, k, command_by_id("duplicate_up"));
    CHECK(keymap_find(&m, k) == command_by_id("duplicate_up"));
    keymap_bind(&m, mk(CTRL('s'), 0), -1);
    CHECK(keymap_find(&m, mk(CTRL('s'), 0)) == -1);
    keymap_bind(&m, mk(CTRL('s'), 0), command_by_id("find"));
    CHECK(keymap_find(&m, mk(CTRL('s'), 0)) == command_by_id("find"));
    keymap_label(&m, command_by_id("duplicate_up"), out, sizeof out);
    CHECK(strcmp(out, "Alt+J") == 0);
    keymap_label(&m, command_by_id("fold_all"), out, sizeof out);
    CHECK(out[0] == '\0');
    keymap_free(&m);
}

static void test_config(void)
{
    Config c;
    LangCfg l;
    const char *text =
        "# a comment\n"
        "tab_width = 4        # trailing comment\n"
        "expand_tab = ON\n"
        "theme = chiaro\n"
        "clipboard = off\n"
        "git = yes\n"
        "final_newline = always\n"
        "\n"
        "[Python]\n"
        "tab_width=2\n"
        "trim_trailing_space = true\n"
        "\n"
        "[ keys ]\n"
        "ctrl+s = save\n"
        "alt+y = duplicate_down\n"
        "ctrl+shift+f = none\n";

    config_init(&c);
    CHECK(config_parse(&c, text, strlen(text)) == 0 && c.first_error[0] == '\0');
    CHECK(strcmp(c.theme, "chiaro") == 0 && c.clipboard == 0 && c.git == 1);
    CHECK(c.nkeys == 3 && strcmp(c.keys[0].key, "ctrl+s") == 0 && strcmp(c.keys[0].cmd, "save") == 0);
    CHECK(strcmp(c.keys[2].cmd, "none") == 0 && c.keys[2].line == 16);
    l = config_lang(&c, "C");
    CHECK(l.tab_width == 4 && l.expand_tab == 1 && l.final_newline == 1 && l.trim_trailing_space == 0);
    CHECK(l.autosuggest == 1 && l.suggest_min == 2);
    l = config_lang(&c, "python");
    CHECK(l.tab_width == 2 && l.expand_tab == 1 && l.trim_trailing_space == 1);
    l = config_lang(&c, NULL);
    CHECK(l.tab_width == 4);
    config_free(&c);

    /* defaults without a file */
    config_init(&c);
    l = config_lang(&c, "C");
    CHECK(l.tab_width == 8 && l.expand_tab == 0 && l.final_newline == 0 && l.autosuggest == 1);
    CHECK(c.clipboard == -1 && c.git == -1 && c.theme[0] == '\0');
    config_free(&c);

    /* mistakes are skipped, counted, and the first one is described with its line */
    {
        const char *bad =
            "tab_width = 40\n"
            "colour = red\n"
            "expand_tab = maybe\n"
            "no equals here\n"
            "[python\n"
            "tab_width = 3\n"
            "[go]\n"
            "theme = x\n"
            "final_newline = sometimes\n";

        config_init(&c);
        CHECK(config_parse(&c, bad, strlen(bad)) == 7);
        CHECK(strstr(c.first_error, "riga 1:") != NULL && strstr(c.first_error, "tab_width") != NULL);
        l = config_lang(&c, "C");
        CHECK(l.tab_width == 3 || l.tab_width == 8);        /* the bad value was not taken */
        CHECK(l.tab_width != 40 && l.expand_tab == 0);
        config_free(&c);
    }

    /* the directory comes from XDG_CONFIG_HOME, else HOME */
    {
        char *d;

        setenv("XDG_CONFIG_HOME", "/tmp/xdgtest", 1);
        d = config_dir();
        CHECK(d && strcmp(d, "/tmp/xdgtest/ked") == 0);
        free(d);
        unsetenv("XDG_CONFIG_HOME");
        setenv("HOME", "/tmp/hometest", 1);
        d = config_dir();
        CHECK(d && strcmp(d, "/tmp/hometest/.config/ked") == 0);
        free(d);
    }

    /* loading from a folder; a missing file is not an error */
    {
        char dir[] = "/tmp/ked_cfg_XXXXXX";
        char path[300];
        FILE *f;

        CHECK(mkdtemp(dir) != NULL);
        config_init(&c);
        CHECK(config_load(&c, dir) == 0 && c.nerrors == 0);
        snprintf(path, sizeof path, "%s/config", dir);
        f = fopen(path, "w");
        fputs("tab_width = 6\n", f);
        fclose(f);
        CHECK(config_load(&c, dir) == 1 && config_lang(&c, "C").tab_width == 6);
        config_free(&c);
        remove(path);
        rmdir(dir);
    }
}

static void test_config_apply(void)
{
    Editor e;
    char dir[] = "/tmp/ked_xdg_XXXXXX";
    char path[300];
    FILE *f;
    Key k;
    char err[64];

    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/ked", dir);
    CHECK(mkdir(path, 0700) == 0);
    snprintf(path, sizeof path, "%s/ked/config", dir);
    f = fopen(path, "w");
    fputs("tab_width = 4\nexpand_tab = on\ntheme = chiaro\nclipboard = off\nautosuggest = off\n"
          "trim_trailing_space = yes\nfinal_newline = always\n"
          "[c]\ntab_width = 2\nexpand_tab = off\n"
          "[keys]\nalt+j = duplicate_up\nctrl+s = none\nalt+q = save\n", f);
    fclose(f);
    setenv("XDG_CONFIG_HOME", dir, 1);

    ed_load(&e, "a\n");
    doc_set_syntax(e.doc, lang("c"));
    editor_load_config(&e);
    CHECK(e.cfg.nerrors == 0 && e.msg[0] == '\0');
    CHECK(e.doc->tabw == 2 && e.tabw == 2 && !e.doc->expand_tab);
    CHECK(strcmp(e.theme->name, "chiaro") == 0 && e.clipboard_osc52 == 0);
    CHECK(!e.doc->autosuggest && e.doc->trim_space && e.doc->final_nl);
    CHECK(key_parse("alt+j", &k, err, sizeof err) == 0 && keymap_find(&e.km, k) == command_by_id("duplicate_up"));
    CHECK(keymap_find(&e.km, key_norm((Key){ CTRL('s'), 0 })) == -1);
    CHECK(key_parse("alt+q", &k, err, sizeof err) == 0 && keymap_find(&e.km, k) == command_by_id("save"));
    CHECK(keymap_find(&e.km, key_norm((Key){ CTRL('q'), 0 })) == command_by_id("quit"));
    editor_free(&e);

    /* plain text takes the top-level values: Tab inserts spaces to the next stop */
    ed_load(&e, "ab");
    editor_load_config(&e);
    CHECK(e.doc->tabw == 4 && e.doc->expand_tab);
    e.cx = 0;
    edit_tab(&e);
    CHECK(buf_is(e.buf, "    ab") && e.cx == 4);
    e.cx = 5;
    edit_tab(&e);
    CHECK(buf_is(e.buf, "    a   b") && e.cx == 8);
    ed_sel(&e, 0, 0, 0, 3);
    edit_tab(&e);
    CHECK(buf_is(e.buf, "        a   b"));
    edit_dedent(&e);
    CHECK(buf_is(e.buf, "    a   b"));
    editor_free(&e);

    /* trailing blanks go, as one undo step; the cursor stays inside the line */
    ed_load(&e, "x  \n\ty \t\nz");
    e.cx = 3;
    edit_trim_trailing(&e, e.doc);
    CHECK(buf_is(e.buf, "x\n\ty\nz") && e.cx == 1);
    edit_undo(&e);
    CHECK(buf_is(e.buf, "x  \n\ty \t\nz"));
    editor_free(&e);

    /* problems: unknown theme, key, command, bad key syntax */
    snprintf(path, sizeof path, "%s/ked/config", dir);
    f = fopen(path, "w");
    fputs("theme = nonesiste\n[keys]\nalt+j = no_such_command\nhyper+x = save\nctrl+i = save\n", f);
    fclose(f);
    ed_load(&e, "x");
    editor_load_config(&e);
    CHECK(e.cfg.nerrors == 4 && strncmp(e.msg, "config:", 7) == 0);
    CHECK(strstr(e.msg, "nonesiste") != NULL && strstr(e.msg, "altri") != NULL);
    editor_free(&e);

    unsetenv("XDG_CONFIG_HOME");
    remove(path);
    snprintf(path, sizeof path, "%s/ked", dir);
    rmdir(path);
    rmdir(dir);
}

static void collect_msg(const char *msg, void *ctx)
{
    SBuf *b = ctx;

    sbuf_add(b, msg, strlen(msg));
    sbuf_add(b, "\n", 1);
}

static void test_user_themes_syntax(void)
{
    char err[200];
    const char *th = "name Notte\nbackground #101010\nforeground #eeeeee\ncomment #808080\n"
                     "git_add #00ff00\n# comment\n";
    size_t n0 = theme_count();
    int i;
    char dir[] = "/tmp/ked_user_XXXXXX";
    char sub[300], cmd[400];
    SBuf msgs = SBUF_INIT;
    Buffer b;
    Syntax *s;
    uint16_t st = 0;

    /* a theme: given values replace, the rest comes from the dark theme */
    CHECK(theme_add_user(th, strlen(th), err, sizeof err) == 0);
    CHECK(theme_count() == n0 + 1);
    i = theme_by_name("notte");
    CHECK(i == (int)n0 && strcmp(theme_get((size_t)i)->name, "Notte") == 0);
    CHECK(theme_get((size_t)i)->has_bg && theme_get((size_t)i)->bg.r == 0x10);
    CHECK(theme_get((size_t)i)->cls[SC_COMMENT].r == 0x80 && theme_get((size_t)i)->fg.g == 0xee);
    CHECK(theme_get((size_t)i)->cls[SC_NORMAL].r == 0xee && theme_get((size_t)i)->git_add.g == 255);
    CHECK(theme_get((size_t)i)->cls[SC_STRING].r == theme_get(0)->cls[SC_STRING].r);
    CHECK(theme_by_name("scuro") == 0 && theme_by_name("chiaro") == 1 && theme_by_name("zzz") == -1);
    CHECK(theme_get(n0)->has_bg && theme_get(n0 + 1) == theme_get(0));      /* wraps around */
    CHECK(theme_add_user("name X\nbackground none\n", 24, err, sizeof err) == 0);
    CHECK(!theme_get(n0 + 1)->has_bg);
    /* errors */
    CHECK(theme_add_user("name X\ncomment red\n", 20, err, sizeof err) < 0 && strstr(err, "riga 2") != NULL);
    CHECK(theme_add_user("name X\nbanana #000000\n", 23, err, sizeof err) < 0 && strstr(err, "banana") != NULL);
    CHECK(theme_add_user("comment #000000\n", 16, err, sizeof err) < 0 && strstr(err, "name") != NULL);
    CHECK(theme_count() == n0 + 2);
    theme_clear_user();
    CHECK(theme_count() == n0);

    /* a language: found by extension before the built-in ones, replaces one of the same name */
    buf_init(&b);
    b.path = xstrdup("x.foo");
    CHECK(syn_detect(&b) == NULL);
    {
        const char *lang_foo = "name Foo\next foo\nwords keyword wibble wobble\nregion comment ;; $\n";

        CHECK(syn_add_user(lang_foo, strlen(lang_foo), err, sizeof err) == 0);
    }
    s = syn_detect(&b);
    CHECK(s && strcmp(syn_name(s), "Foo") == 0);
    CHECK(strcmp(cls_str(s, &st, "wibble ;; c"), "kkkkkk.cccc") == 0);
    syn_free(s);
    free(b.path);
    b.path = xstrdup("x.c");
    {
        const char *mine = "name C\next c\nwords keyword wibble\n";

        CHECK(syn_add_user(mine, strlen(mine), err, sizeof err) == 0);
    }
    s = syn_detect(&b);
    st = 0;
    CHECK(s && strcmp(cls_str(s, &st, "if wibble"), "...kkkkkk") == 0);
    syn_free(s);
    syn_clear_user();
    s = syn_detect(&b);
    CHECK(s && strcmp(syn_name(s), "C") == 0);
    st = 0;
    CHECK(strncmp(cls_str(s, &st, "if wibble"), "kk.", 3) == 0);
    syn_free(s);
    buf_free(&b);
    CHECK(syn_add_user("ext foo\n", 8, err, sizeof err) < 0);
    CHECK(syn_add_user("name Bad\nfrobnicate\n", 20, err, sizeof err) < 0 && strstr(err, "riga 2") != NULL);

    /* folders: alphabetical, bad files reported and skipped, hidden files ignored */
    CHECK(mkdtemp(dir) != NULL);
    put_file(dir, "b.theme", "name Beta\nforeground #112233\n");
    put_file(dir, "a.theme", "name Alfa\n");
    put_file(dir, "bad.theme", "name Bad\nmatch zzz\n");
    put_file(dir, ".hidden", "name Hidden\n");
    CHECK(theme_load_dir(dir, collect_msg, &msgs) == 2);
    CHECK(theme_by_name("alfa") == (int)n0 && theme_by_name("beta") == (int)n0 + 1);
    CHECK(theme_by_name("hidden") == -1 && strstr(msgs.p, "bad.theme") != NULL);
    theme_clear_user();
    sbuf_free(&msgs);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0);

    /* both folders through the editor's configuration */
    {
        Editor e;
        char xdg[] = "/tmp/ked_xdg2_XXXXXX";
        Buffer b2;
        Syntax *s2;

        CHECK(mkdtemp(xdg) != NULL);
        snprintf(sub, sizeof sub, "%s/ked", xdg);
        CHECK(mkdir(sub, 0700) == 0);
        snprintf(sub, sizeof sub, "%s/ked/themes", xdg);
        CHECK(mkdir(sub, 0700) == 0);
        put_file(sub, "notte", "name Notte\nbackground #101010\n");
        snprintf(sub, sizeof sub, "%s/ked/syntax", xdg);
        CHECK(mkdir(sub, 0700) == 0);
        put_file(sub, "foo", "name Foo\next foo\nwords keyword wibble\n");
        snprintf(sub, sizeof sub, "%s/ked", xdg);
        put_file(sub, "config", "theme = notte\n[foo]\ntab_width = 3\n");
        setenv("XDG_CONFIG_HOME", xdg, 1);
        ed_load(&e, "x");
        editor_load_config(&e);
        CHECK(e.cfg.nerrors == 0 && strcmp(e.theme->name, "Notte") == 0);
        buf_init(&b2);
        b2.path = xstrdup("t.foo");
        s2 = syn_detect(&b2);
        CHECK(s2 && strcmp(syn_name(s2), "Foo") == 0);
        syn_free(s2);
        buf_free(&b2);
        editor_free(&e);
        CHECK(theme_count() == 2);
        unsetenv("XDG_CONFIG_HOME");
        snprintf(cmd, sizeof cmd, "rm -rf %s", xdg);
        CHECK(system(cmd) == 0);
    }
}

static void test_git(void)
{
    uint8_t m[12];
    const char *diff1 =
        "diff --git a/f b/f\nindex 1..2 100644\n--- a/f\n+++ b/f\n"
        "@@ -2,0 +3,2 @@ ctx\n+new1\n+new2\n"          /* two lines added after old line 2 */
        "@@ -5 +7 @@\n-old\n+changed\n"                /* one line changed */
        "@@ -9,2 +10,3 @@\n-a\n-b\n+x\n+y\n+z\n";      /* two become three */
    const char *diff2 = "@@ -4,2 +3,0 @@\n-gone\n-gone2\n@@ -1,1 +0,0 @@\n-top\n";
    char dir[] = "/tmp/ked_git_XXXXXX";
    char path[300], cmd[600];
    uint8_t *marks = NULL;
    FILE *f;
    int r;

    memset(m, 0, sizeof m);
    git_parse_diff(diff1, strlen(diff1), m, 12);
    CHECK(m[0] == GIT_NONE && m[1] == GIT_NONE && m[2] == GIT_ADD && m[3] == GIT_ADD);
    CHECK(m[4] == GIT_NONE && m[6] == GIT_MOD);
    CHECK(m[9] == GIT_MOD && m[10] == GIT_MOD && m[11] == GIT_ADD);
    /* a pure deletion marks the line above it (the first line for a deletion at the top) */
    memset(m, 0, sizeof m);
    git_parse_diff(diff2, strlen(diff2), m, 12);
    CHECK(m[2] == GIT_DEL && m[0] == GIT_DEL);
    /* junk and out-of-range hunks are harmless */
    memset(m, 0, sizeof m);
    git_parse_diff("@@ nonsense @@\n@@ -1 +100,5 @@\n+x\n@@ -1 +0 @@\n", 40, m, 12);
    git_parse_diff("", 0, m, 12);

    if (system("git --version >/dev/null 2>&1") != 0)
        return;                                    /* no git here: nothing more to try */
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/a.txt", dir);
    f = fopen(path, "w");
    fputs("one\ntwo\nthree\nfour\nfive\n", f);
    fclose(f);

    /* outside a repository, or not tracked: no marks, not an error */
    CHECK(git_file_marks(path, 5, &marks) == 0 && marks == NULL);
    snprintf(cmd, sizeof cmd,
             "cd %s && git init -q . && git -c user.name=t -c user.email=t@t add a.txt && "
             "git -c user.name=t -c user.email=t@t commit -q -m first", dir);
    CHECK(system(cmd) == 0);
    r = git_file_marks(path, 5, &marks);
    CHECK(r == 1 && marks && memcmp(marks, "\0\0\0\0\0", 5) == 0);
    free(marks);

    /* a change, an addition and a deletion against HEAD */
    f = fopen(path, "w");
    fputs("one\nTWO\nthree\nfive\nsix\n", f);
    fclose(f);
    r = git_file_marks(path, 5, &marks);
    CHECK(r == 1 && marks[0] == GIT_NONE && marks[1] == GIT_MOD && marks[2] == GIT_DEL);
    CHECK(marks[3] == GIT_NONE && marks[4] == GIT_ADD);
    free(marks);
    /* an untracked file next to it */
    snprintf(path, sizeof path, "%s/b.txt", dir);
    f = fopen(path, "w");
    fputs("x\n", f);
    fclose(f);
    marks = NULL;
    CHECK(git_file_marks(path, 1, &marks) == 0 && marks == NULL);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
}

static void test_tags(void)
{
    char dir[] = "/tmp/ked_tags_XXXXXX";
    char sub[300], path[300], cmd[400];
    TagHits h;
    char *found;
    Buffer b;
    const char *src = "#include <x.h>\nstatic int helper(int a)\n{\n    return a;\n}\n\nint main(void)\n{\n}\n";
    const char *sorted =
        "!_TAG_FILE_FORMAT\t2\t/extended format/\n"
        "!_TAG_FILE_SORTED\t1\t/0=unsorted, 1=sorted/\n"
        "helper\tsrc/a.c\t/^static int helper(int a)$/;\"\tf\n"
        "main\tsrc/a.c\t/^int main(void)$/;\"\tf\tline:7\n"
        "main\tsrc/b.c\t12;\"\tkind:function\n"
        "mainloop\tsrc/c.c\t3;\"\tf\n"
        "zeta\tsrc/a.c\t/^int \\/zeta$/;\"\tv\n";
    const char *unsorted =
        "zeta\tz.c\t9\n"
        "alpha\ta.c\t/^alpha$/\n"
        "alpha\tb.c\t4;\"\tf\n";

    CHECK(mkdtemp(dir) != NULL);
    snprintf(sub, sizeof sub, "%s/src", dir);
    CHECK(mkdir(sub, 0700) == 0);
    snprintf(path, sizeof path, "%s/tags", dir);
    write_file(path, sorted, strlen(sorted));

    /* the tags file is found from a folder below it */
    found = tags_find_file(sub);
    CHECK(found && strcmp(found, path) == 0);
    free(found);

    memset(&h, 0, sizeof h);
    CHECK(tags_lookup(path, "main", &h) == 2);            /* "mainloop" is another name */
    CHECK(strcmp(h.v[0].file, "src/a.c") == 0 && strcmp(h.v[0].kind, "f") == 0 && h.v[0].line == 7);
    CHECK(strcmp(h.v[1].file, "src/b.c") == 0 && strcmp(h.v[1].addr, "12") == 0);
    CHECK(strcmp(h.v[1].kind, "function") == 0);
    taghits_free(&h);
    CHECK(tags_lookup(path, "helper", &h) == 1 && strcmp(h.v[0].addr, "/^static int helper(int a)$/") == 0);
    taghits_free(&h);
    CHECK(tags_lookup(path, "mainloop", &h) == 1 && tags_lookup(path, "mai", &h) == 0);
    taghits_free(&h);
    CHECK(tags_lookup(path, "zeta", &h) == 1 && tags_lookup(path, "nothing", &h) == 0);
    taghits_free(&h);
    CHECK(tags_lookup(path, "aaa", &h) == 0 && tags_lookup("/nonexistent/tags", "x", &h) == -1);

    /* resolving an address against the text of the file */
    buf_init(&b);
    {
        size_t y = 0, x = 0;

        buf_insert_text(&b, &y, &x, src, strlen(src));
    }
    memset(&h, 0, sizeof h);
    tags_lookup(path, "helper", &h);
    CHECK(tag_resolve(&h.v[0], "helper", &b) == 2);
    taghits_free(&h);
    tags_lookup(path, "main", &h);
    CHECK(tag_resolve(&h.v[0], "main", &b) == 7);          /* from line:7 */
    CHECK(tag_resolve(&h.v[1], "main", &b) == 12);         /* a plain number */
    taghits_free(&h);
    {
        TagHit t;
        memset(&t, 0, sizeof t);
        t.addr = xstrdup("/^int main(void)$/");
        CHECK(tag_resolve(&t, "main", &b) == 7);
        free(t.addr);
        t.addr = xstrdup("/^return/");
        CHECK(tag_resolve(&t, "x", &b) == 0);               /* the line starts with spaces */
        free(t.addr);
        t.addr = xstrdup("/return a/");
        CHECK(tag_resolve(&t, "x", &b) == 4);
        free(t.addr);
        t.addr = xstrdup("/^gone$/");
        CHECK(tag_resolve(&t, "x", &b) == 0);
        free(t.addr);
    }
    buf_free(&b);

    /* an unsorted file is read line by line, CRLF and extended fields tolerated */
    write_file(path, unsorted, strlen(unsorted));
    memset(&h, 0, sizeof h);
    CHECK(tags_lookup(path, "alpha", &h) == 2 && strcmp(h.v[0].file, "a.c") == 0);
    CHECK(strcmp(h.v[1].file, "b.c") == 0 && strcmp(h.v[1].kind, "f") == 0);
    taghits_free(&h);
    CHECK(tags_lookup(path, "zeta", &h) == 1);
    taghits_free(&h);

    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0);
}

int main(void)
{
    test_utf8();
    test_edit();
    test_files();
    test_undo();
    test_undo_saved();
    test_ranges();
    test_clip();
    test_edit_cmds();
    test_syntax_lines();
    test_syntax_parse();
    test_mouse();
    test_syntax_detect();
    test_syntax_langs();
    test_syntax_detect_langs();
    test_syntax_incremental();
    test_syntax_brackets();
    test_theme();
    test_fuzzy_picker();
    test_layout();
    test_docs();
    test_files_tree();
    test_session();
    test_session_layout();
    test_fold();
    test_search_lines();
    test_search_buffer();
    test_search_replace();
    test_search_files();
    test_goto_parse();
    test_comments();
    test_commands();
    test_keymap();
    test_config();
    test_config_apply();
    test_user_themes_syntax();
    test_git();
    test_tags();
    test_complete();
    test_snippets();
    test_cursors();
    test_folding();
    test_shell();
    test_errors_parse();
    test_run();
    test_filter();
    if (failures) {
        fprintf(stderr, "%d test falliti\n", failures);
        return 1;
    }
    puts("tutti i test passati");
    return 0;
}
