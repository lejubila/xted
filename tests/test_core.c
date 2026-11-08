#include "buffer.h"
#include "clip.h"
#include "edit.h"
#include "langs.h"
#include "editor.h"
#include "syntax.h"
#include "theme.h"
#include "util.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    buf_insert_text(&e->buf, &y, &x, text, strlen(text));
    buf_undo_close(&e->buf);
    undo_free(&e->buf.undo);
    e->buf.modified = 0;
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
    CHECK(buf_is(&e.buf, "\tfoo\n\t") && e.cy == 1 && e.cx == 1);
    e.cy = 0;
    e.cx = 1;
    edit_newline(&e);                    /* cursor inside the indentation */
    CHECK(buf_is(&e.buf, "\t\n\tfoo\n\t") && e.cy == 1 && e.cx == 1);
    editor_free(&e);

    /* typing over a selection replaces it, as one undo step */
    ed_load(&e, "hello");
    ed_sel(&e, 0, 0, 0, 3);
    CHECK(edit_sel_active(&e));
    edit_insert_cp(&e, 'X');
    edit_insert_cp(&e, 'Y');
    CHECK(buf_is(&e.buf, "XYlo") && !e.sel_on && e.cx == 2);
    edit_undo(&e);
    CHECK(buf_is(&e.buf, "hello") && e.cy == 0 && e.cx == 0);
    edit_redo(&e);
    CHECK(buf_is(&e.buf, "XYlo"));
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
    CHECK(buf_is(&e.buf, "a\nb\nb\nc") && e.cy == 3 && e.cx == 0);
    edit_undo(&e);
    CHECK(buf_is(&e.buf, "a\nb\nc"));
    e.cy = 0;
    edit_cut(&e);
    CHECK(buf_is(&e.buf, "b\nc") && e.cy == 0 && e.clip.count == 2);
    edit_undo(&e);
    CHECK(buf_is(&e.buf, "a\nb\nc"));
    ed_sel(&e, 0, 0, 1, 1);
    edit_cut(&e);
    CHECK(buf_is(&e.buf, "\nc") && !e.clip.e[0].whole_line && e.clip.e[0].n == 3);
    e.cy = 1;
    e.cx = 1;
    edit_paste(&e);
    CHECK(buf_is(&e.buf, "\nca\nb") && e.cy == 2 && e.cx == 1);
    editor_free(&e);

    /* Alt+V walks back through the kill ring */
    ed_load(&e, "x");
    clip_push(&e.clip, strdup("old"), 3, 0);
    clip_push(&e.clip, strdup("new"), 3, 0);
    e.cx = 1;
    edit_paste(&e);
    CHECK(buf_is(&e.buf, "xnew"));
    edit_paste_older(&e);
    CHECK(buf_is(&e.buf, "xold"));
    edit_paste_older(&e);
    CHECK(buf_is(&e.buf, "xnew"));
    edit_undo(&e);
    CHECK(buf_is(&e.buf, "xold"));
    editor_free(&e);

    /* bracketed paste: no autoindent, \r\n normalised, controls dropped */
    ed_load(&e, "\tq");
    e.cx = 2;
    edit_paste_text(&e, "a\r\nb\rc\x01\td", 9);
    CHECK(buf_is(&e.buf, "\tqa\nb\nc\td"));
    edit_undo(&e);
    CHECK(buf_is(&e.buf, "\tq"));
    editor_free(&e);

    /* move, duplicate, delete, join */
    ed_load(&e, "1\n2\n3");
    e.cy = 1;
    edit_move_lines(&e, 0);
    CHECK(buf_is(&e.buf, "2\n1\n3") && e.cy == 0);
    edit_move_lines(&e, 0);             /* already first: nothing happens */
    CHECK(buf_is(&e.buf, "2\n1\n3") && e.cy == 0);
    edit_move_lines(&e, 1);
    CHECK(buf_is(&e.buf, "1\n2\n3") && e.cy == 1);
    e.cy = 2;
    edit_move_lines(&e, 1);             /* already last */
    CHECK(buf_is(&e.buf, "1\n2\n3"));
    ed_sel(&e, 0, 0, 1, 1);
    edit_move_lines(&e, 1);
    CHECK(buf_is(&e.buf, "3\n1\n2") && e.ay == 1 && e.cy == 2);
    edit_undo(&e);
    CHECK(buf_is(&e.buf, "1\n2\n3"));
    e.sel_on = 0;
    e.cy = 0;
    edit_duplicate_lines(&e, 1);
    CHECK(buf_is(&e.buf, "1\n1\n2\n3") && e.cy == 1);
    edit_duplicate_lines(&e, 0);
    CHECK(buf_is(&e.buf, "1\n1\n1\n2\n3") && e.cy == 1);
    e.cy = 4;
    edit_delete_lines(&e);
    CHECK(buf_is(&e.buf, "1\n1\n1\n2") && e.cy == 3);
    editor_free(&e);

    ed_load(&e, "foo\n   bar\nbaz");
    edit_join_lines(&e);
    CHECK(buf_is(&e.buf, "foo bar\nbaz") && e.cy == 0 && e.cx == 3);
    e.cy = 1;
    edit_join_lines(&e);                /* last line: nothing to join */
    CHECK(buf_is(&e.buf, "foo bar\nbaz"));
    edit_undo(&e);
    CHECK(buf_is(&e.buf, "foo\n   bar\nbaz"));
    editor_free(&e);

    /* indent and dedent a selection */
    ed_load(&e, "a\n  b\n        c\nd");
    ed_sel(&e, 0, 0, 2, 3);
    edit_tab(&e);
    CHECK(buf_is(&e.buf, "\ta\n\t  b\n\t        c\nd") && e.cx == 4);
    edit_dedent(&e);
    CHECK(buf_is(&e.buf, "a\n  b\n        c\nd"));
    edit_dedent(&e);                    /* spaces: up to tabw of them */
    CHECK(buf_is(&e.buf, "a\nb\nc\nd"));
    edit_undo(&e);
    edit_undo(&e);
    edit_undo(&e);
    CHECK(buf_is(&e.buf, "a\n  b\n        c\nd"));
    e.sel_on = 0;
    e.cy = 3;
    e.cx = 0;
    edit_tab(&e);                       /* no selection: a tab character */
    CHECK(buf_is(&e.buf, "a\n  b\n        c\n\td"));
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
    CHECK(strcmp(detect_name("x.tsx", ""), "JavaScript") == 0);
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
    CHECK(strcmp(detect_name("Makefile", ""), "-") == 0);
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
    test_syntax_detect();
    test_syntax_incremental();
    test_syntax_brackets();
    test_theme();
    if (failures) {
        fprintf(stderr, "%d test falliti\n", failures);
        return 1;
    }
    puts("tutti i test passati");
    return 0;
}
