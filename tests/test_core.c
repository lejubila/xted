#include "buffer.h"
#include "clip.h"
#include "commands.h"
#include "ipc.h"
#include "config.h"
#include "cursors.h"
#include "cfgsave.h"
#include "complete.h"
#include "doc.h"
#include "editorconfig.h"
#include "indent.h"
#include "i18n.h"
#include "keymap.h"
#include "json.h"
#include "md.h"
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
#include "symbols.h"
#include "editor.h"
#include "mouse.h"
#include "syntax.h"
#include "tags.h"
#include "theme.h"
#include "util.h"
#include "utf8.h"

#ifndef _WIN32
#include <poll.h>
#include <sys/wait.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "sys.h"
#include <time.h>
#include <unistd.h>

#include "test_win.h"

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
    char dir[] = "/tmp/xted_test_XXXXXX";
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
#ifndef _WIN32
    CHECK(stat(path, &st) == 0 && (st.st_mode & 07777) == 0640);     /* no such modes on Windows */
#endif
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

static char *make_dir(char *buf, size_t n);
static void rm_rf(const char *dir);

/* The folder the tests started in (on Windows they move to %TEMP%): the files of the project
 * that they read, as lang/it, are there. */
static char start_dir[4096];

static const char *start_file(const char *rel)
{
    static char p[4200];

    snprintf(p, sizeof p, "%s/%s", start_dir[0] ? start_dir : ".", rel);
    return p;
}

/* UTF-16 and UTF-8 with a BOM: UTF-8 in the buffer, back in the file's encoding when saved. */
static void test_encodings(void)
{
    char dir[64], path[128], err[128], *out;
    Buffer b;
    size_t n;
    /* desktop.ini as Windows writes it: UTF-16 LE, BOM, CRLF */
    static const char ini[] = "\xFF\xFE" "[\0.\0S\0\r\0\n\0\xE0\0=\0\x3D\xD8\x42\xDE\r\0\n\0";
    static const char be[] = "\xFE\xFF" "\0a\0\n";
    static const char bom8[] = "\xEF\xBB\xBF" "ciao\n";
    static const char bad[] = "\xFF\xFE" "a\0\x00\xD8" "b\0";             /* half a surrogate pair */

    CHECK(make_dir(dir, sizeof dir) != NULL);
    snprintf(path, sizeof path, "%s/e", dir);

    /* opened as text, saved back identical */
    roundtrip(path, ini, sizeof ini - 1, 1, 1);
    write_file(path, ini, sizeof ini - 1);
    buf_init(&b);
    CHECK(buf_load(&b, path, err, sizeof err) == 0 && b.enc == ENC_UTF16LE && !b.has_nul);
    CHECK(b.nlines == 2 && b.crlf && line_is(&b, 0, "[.S") && line_is(&b, 1, "\xc3\xa0=\xf0\x9f\x99\x82"));
    /* an edit keeps the encoding */
    buf_insert(&b, 0, 3, "x", 1);
    CHECK(buf_save(&b, NULL, err, sizeof err) == (long)(sizeof ini - 1) + 2);
    out = read_file(path, &n);
    CHECK(n == sizeof ini + 1 && memcmp(out, "\xFF\xFE[\0.\0S\0x\0\r\0", 12) == 0);
    free(out);
    buf_free(&b);

    roundtrip(path, be, sizeof be - 1, 0, 1);
    write_file(path, be, sizeof be - 1);
    buf_init(&b);
    CHECK(buf_load(&b, path, err, sizeof err) == 0 && b.enc == ENC_UTF16BE && line_is(&b, 0, "a"));
    buf_free(&b);

    roundtrip(path, bom8, sizeof bom8 - 1, 0, 1);
    write_file(path, bom8, sizeof bom8 - 1);
    buf_init(&b);
    CHECK(buf_load(&b, path, err, sizeof err) == 0 && b.enc == ENC_UTF8_BOM && line_is(&b, 0, "ciao"));
    buf_free(&b);

    roundtrip(path, "\xFF\xFE", 2, 0, 1);                     /* the BOM alone stays */

    /* not valid UTF-16: as it is (bytes), and back as it was */
    roundtrip(path, bad, sizeof bad - 1, 0, 0);
    write_file(path, bad, sizeof bad - 1);
    buf_init(&b);
    CHECK(buf_load(&b, path, err, sizeof err) == 0 && b.enc == ENC_UTF8 && b.has_nul);
    buf_free(&b);
    CHECK(strcmp(buf_enc_name(ENC_UTF16LE), "UTF-16 LE") == 0 && strcmp(buf_enc_name(ENC_UTF8), "UTF-8") == 0);
    rm_rf(dir);
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
    char dir[] = "/tmp/xted_test_XXXXXX";
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
    buf_insert_text(&b, &y, &x, "hello\nworld\nagain", strlen("hello\nworld\nagain"));
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

static Syntax *lang(const char *name);

/* Types text as the keyboard does: with the automatic pairs. */
static void type_text(Editor *e, const char *t)
{
    for (; *t; t++)
        if (!edit_type(e, (unsigned char)*t))
            edit_insert_cp(e, (unsigned char)*t);
}

static void put_file(const char *dir, const char *name, const char *text);
static char *make_dir(char *buf, size_t n);

static void test_indent(void)
{
    Editor e;
    Buffer b;
    int tabs = -1, width = -1;
    char dir[64], path[160], err[128];
    Doc *d;
    size_t y = 0, x = 0;
    EcProps ec;

    /* detection */
    buf_init(&b);
    buf_insert_text(&b, &y, &x, "int f()\n{\n    if (a) {\n        b();\n    }\n}\n", strlen("int f()\n{\n    if (a) {\n        b();\n    }\n}\n"));
    CHECK(indent_detect(&b, &tabs, &width) && tabs == 0 && width == 4);
    buf_free(&b);
    buf_init(&b);
    y = x = 0;
    buf_insert_text(&b, &y, &x, "a\n  b\n    c\n  d\n", strlen("a\n  b\n    c\n  d\n"));
    CHECK(indent_detect(&b, &tabs, &width) && tabs == 0 && width == 2);
    buf_free(&b);
    buf_init(&b);
    y = x = 0;
    buf_insert_text(&b, &y, &x, "a\n\tb\n\t\tc\n\td\n", strlen("a\n\tb\n\t\tc\n\td\n"));
    CHECK(indent_detect(&b, &tabs, &width) && tabs == 1);
    buf_free(&b);
    buf_init(&b);
    y = x = 0;
    buf_insert_text(&b, &y, &x, "a\nb\nc\n", strlen("a\nb\nc\n"));
    CHECK(!indent_detect(&b, &tabs, &width));
    buf_free(&b);

    /* globs */
    CHECK(editorconfig_match("*", "a/b.c") && editorconfig_match("*.py", "x/y/z.py"));
    CHECK(!editorconfig_match("*.py", "z.pyc") && editorconfig_match("*.{js,ts}", "a.ts"));
    CHECK(editorconfig_match("Makefile", "sub/Makefile") && !editorconfig_match("/Makefile", "sub/Makefile"));
    CHECK(editorconfig_match("src/**.c", "src/a/b.c") && !editorconfig_match("src/*.c", "src/a/b.c"));
    CHECK(editorconfig_match("[ab].txt", "a.txt") && !editorconfig_match("[!a].txt", "a.txt"));
    CHECK(editorconfig_match("f{1..3}.x", "f2.x") && !editorconfig_match("f{1..3}.x", "f4.x"));

    /* .editorconfig files: the nearest wins, root = true stops the climb */
    CHECK(make_dir(dir, sizeof dir) != NULL);
    snprintf(path, sizeof path, "%s/sub", dir);
    CHECK(mkdir(path, 0700) == 0);
    put_file(dir, ".editorconfig",
             "root = true\n[*]\nindent_style = space\nindent_size = 4\n[*.mk]\nindent_style = tab\n"
             "trim_trailing_whitespace = true\n");
    put_file(dir, "sub/.editorconfig", "[*.py]\nindent_size = 2\n");
    snprintf(path, sizeof path, "%s/sub/a.py", dir);
    CHECK(editorconfig_lookup(path, &ec) == 2);
    CHECK(ec.expand == 1 && ec.size == 2 && ec.trim == -1);
    snprintf(path, sizeof path, "%s/sub/b.mk", dir);
    CHECK(editorconfig_lookup(path, &ec) == 2 && ec.expand == 0 && ec.size == 4 && ec.trim == 1);

    /* the whole chain: configuration > .editorconfig > the file's own style > defaults */
    put_file(dir, "sub/a.py", "x = 1\nif x:\n\ty = 2\n\tz = 3\n");
    editor_init(&e);
    snprintf(path, sizeof path, "%s/sub/a.py", dir);
    d = editor_open_doc(&e, path, err, sizeof err);
    CHECK(d && d->expand_tab == 1 && d->indent_w == 2);     /* .editorconfig over the tabs of the text */
    editor_free(&e);
    put_file(dir, "n.txt", "a\n\tb\n\tc\n");
    snprintf(path, sizeof path, "%s/n.txt", dir);
    editor_init(&e);
    config_parse(&e.cfg, "indent_width = 3\n", strlen("indent_width = 3\n"));
    d = editor_open_doc(&e, path, err, sizeof err);
    CHECK(d && d->expand_tab == 1 && d->indent_w == 3);     /* .editorconfig (space, 4) vs the config's 3 */
    editor_free(&e);
    {
        char dir2[64];

        CHECK(make_dir(dir2, sizeof dir2) != NULL);
        put_file(dir2, "t.txt", "a\n\tb\n\tc\n");
        snprintf(path, sizeof path, "%s/t.txt", dir2);
        editor_init(&e);
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d && d->expand_tab == 0);                    /* the text uses tabs */
        editor_free(&e);
        put_file(dir2, "s.txt", "a\n  b\n    c\n  d\n");
        snprintf(path, sizeof path, "%s/s.txt", dir2);
        editor_init(&e);
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d && d->expand_tab == 1 && d->indent_w == 2);
        editor_free(&e);
        editor_init(&e);
        config_parse(&e.cfg, "expand_tab = false\n", strlen("expand_tab = false\n"));
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d && d->expand_tab == 0);                    /* the user's setting wins */
        editor_free(&e);
    }

    /* Tab, Shift+Tab and Enter between braces follow the indentation width */
    ed_load(&e, "x");
    e.doc->expand_tab = 1;
    e.doc->indent_w = 3;
    e.tabw = e.doc->tabw = 8;
    edit_tab(&e);
    CHECK(buf_is(e.buf, "   x") && e.cx == 3);
    e.cx = 4;
    edit_tab(&e);                                           /* up to the next stop: 6 */
    CHECK(buf_is(e.buf, "   x  ") && e.cx == 6);
    edit_select_all(&e);
    edit_tab(&e);
    CHECK(buf_is(e.buf, "      x  "));
    edit_dedent(&e);
    CHECK(buf_is(e.buf, "   x  "));
    editor_free(&e);

    /* conversion keeps the width in columns */
    ed_load(&e, "a\n    b\n        c\n\t\td\n  \n");
    e.tabw = e.doc->tabw = 4;
    CHECK(edit_convert_indent(&e, 1, 4) == 2);
    CHECK(buf_is(e.buf, "a\n\tb\n\t\tc\n\t\td\n  \n"));
    CHECK(edit_convert_indent(&e, 0, 0) == 3);
    CHECK(buf_is(e.buf, "a\n    b\n        c\n        d\n  \n"));
    edit_undo(&e);
    CHECK(buf_is(e.buf, "a\n\tb\n\t\tc\n\t\td\n  \n"));
    editor_free(&e);

    /* a snippet's tabs become the document's indentation */
    ed_load(&e, "if");
    e.doc->expand_tab = 1;
    e.doc->indent_w = 2;
    e.cx = 2;
    {
        static const SnipDef sd = { "t", "a {\n\t$0\n}" };

        edit_expand_snippet(&e, 0, &sd);
    }
    CHECK(buf_is(e.buf, "a {\n  \n}") && e.cy == 1 && e.cx == 2);
    editor_free(&e);
}

/* Every built-in snippet expands to plain text with no marker left over and has a stop for
 * each of its placeholders (a "$" before a placeholder is written "$$"). */
static void test_snippet_defs(void)
{
    static const char *const langs[] = { "C", "Python", "PHP", "JavaScript", "Shell" };
    size_t li, i, n;

    for (li = 0; li < sizeof langs / sizeof langs[0]; li++) {
        const SnipDef *sd = snip_lang(langs[li], &n);

        CHECK(sd != NULL && n > 0);
        for (i = 0; i < n; i++) {
            SBuf text = SBUF_INIT;
            SnipStops st;
            const char *p;
            size_t want = 0;
            int ok;

            snip_parse(sd[i].body, &text, &st);
            sbuf_add(&text, "", 1);
            for (p = sd[i].body; *p; p++) {
                if (p[0] == '$' && p[1] == '$')
                    p++;
                else if (p[0] == '$' && p[1] == '{')
                    want++;
            }
            ok = strstr(text.p, "${") == NULL && st.n >= want;
            CHECK(ok);
            if (!ok)
                printf("snippet %s/%s\n", langs[li], sd[i].name);
            sbuf_free(&text);
        }
    }
}

static void test_autopair(void)
{
    Editor e;

    ed_load(&e, "");
    doc_set_syntax(e.doc, lang("c"));
    type_text(&e, "f(");
    CHECK(buf_is(e.buf, "f()") && e.cx == 2);
    type_text(&e, "a)");                        /* the closing one is stepped over */
    CHECK(buf_is(e.buf, "f(a)") && e.cx == 4);
    type_text(&e, " {");
    CHECK(buf_is(e.buf, "f(a) {}") && e.cx == 6);
    edit_newline(&e);                           /* {|} opens an indented block */
    CHECK(buf_is(e.buf, "f(a) {\n\t\n}") && e.cy == 1 && e.cx == 1);
    editor_free(&e);

    ed_load(&e, "");
    doc_set_syntax(e.doc, lang("c"));
    type_text(&e, "s = \"ab");
    CHECK(buf_is(e.buf, "s = \"ab\"") && e.cx == 7);
    type_text(&e, "(");                         /* inside a string: a character */
    CHECK(buf_is(e.buf, "s = \"ab(\""));
    type_text(&e, "\"");                        /* the closing quote is stepped over */
    CHECK(buf_is(e.buf, "s = \"ab(\"") && e.cx == 9);
    type_text(&e, ";");
    CHECK(buf_is(e.buf, "s = \"ab(\";"));
    editor_free(&e);

    /* an apostrophe after a letter is not a quote; nothing is paired before a word */
    ed_load(&e, "");
    doc_set_syntax(e.doc, lang("c"));
    type_text(&e, "don'");
    CHECK(buf_is(e.buf, "don'"));
    editor_free(&e);
    ed_load(&e, "foo");
    doc_set_syntax(e.doc, lang("c"));
    type_text(&e, "(");
    CHECK(buf_is(e.buf, "(foo"));
    editor_free(&e);

    /* Backspace inside an empty pair removes both, and it is one undo step */
    ed_load(&e, "");
    doc_set_syntax(e.doc, lang("c"));
    type_text(&e, "[");
    CHECK(buf_is(e.buf, "[]"));
    CHECK(edit_backspace_pair(&e) && buf_is(e.buf, "") && e.cx == 0);
    type_text(&e, "(");
    edit_undo(&e);
    CHECK(buf_is(e.buf, ""));
    editor_free(&e);

    /* a selection is wrapped and stays selected */
    ed_load(&e, "abc");
    doc_set_syntax(e.doc, lang("c"));
    ed_sel(&e, 0, 1, 0, 2);
    CHECK(edit_type(&e, '('));
    CHECK(buf_is(e.buf, "a(b)c") && e.sel_on && e.ax == 2 && e.cx == 3);
    editor_free(&e);

    /* off by setting; no quotes in text without a language */
    ed_load(&e, "");
    doc_set_syntax(e.doc, lang("c"));
    e.doc->autopair = 0;
    type_text(&e, "(");
    CHECK(buf_is(e.buf, "("));
    editor_free(&e);
    ed_load(&e, "");
    type_text(&e, "\"");
    CHECK(buf_is(e.buf, "\""));
    editor_free(&e);
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
static const char *cls_str(Syntax *s, uint16_t *state, const char *text)
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
    CHECK(strstr(err, "line 2") != NULL);
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
    CHECK(strcmp(detect_name("a.txt", "// xted: ft=c++\n"), "C") == 0);
    CHECK(strcmp(detect_name("a.c", "# xted: filetype=python\n"), "Python") == 0);
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
    CLS(php, st, "?> it's <b>", "pp......kkk");
    CHECK(st != 0);
    CLS(php, st, "x <?= $x; ?>", "..ppp.vv..pp");     /* back in PHP at <?, in HTML again at ?> */
    CHECK(st != 0);

    /* HTML: tags, comments, script bodies are left alone */
    st = 0;
    CLS(html, st, "<a href=\"x\">t < u</a>", "kkkkkkkkkkkk.....kkkk");
    CLS(html, st, "<script>", "kkkkkkkk");
    CHECK(st != 0);
    CLS(html, st, "if (a<b) {", "kk...o....");
    CLS(html, st, "</script> <b>", "kkkkkkkkk.kkk");
    CHECK(st == 0);

    /* SQL ignores case */
    CLS(sql, st, "SELECT a FROM t", "kkkkkk...kkkk..");
    CLS(sql, st, "select a from t", "kkkkkk...kkkk..");
    CLS(sql, st, "x = 'it''s' -- c", "..o.sssssss.cccc");

    st = 0;
    CLS(json, st, "{\"a\": [1, true]}", "ovvvo.ono.CCCCoo");
    CLS(ts, st, "interface A { x: number }", "kkkkkkkkk......o.TTTTTT..");
    CLS(ts, st, "@Component() class B {}", "pppppppppp...kkkkk.....");
    CLS(go, st, "x := `r`", "..oo.sss");
    CLS(yaml, st, "key: \"v\" # c", "vvvo.sss.ccc");
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

static void test_syntax_embed(void)
{
    Syntax *html = lang("html"), *php = lang("php"), *tpl = lang("phtml");
    uint16_t st = 0;
    char err[128];
    const char *open, *close;
    Buffer b;
    Syntax *s;

    CHECK(html && php && tpl && strcmp(syn_name(tpl), "PHP/HTML") == 0);

    /* JavaScript between the script tags, over several lines */
    CLS(html, st, "<p>a</p>", "kkk.kkkk");
    CHECK(st == 0);
    CLS(html, st, "<script type=\"x\">var a = 1; /* c", "kkkkkkkkkkkkkkkkkkkk...o.n..cccc");
    CHECK(st != 0);
    CLS(html, st, "still */ return a</script>", "cccccccc.kkkkkk..kkkkkkkkk");
    CHECK(st == 0);
    CLS(html, st, "<script src=\"a.js\"></script> <b>", "kkkkkkkkkkkkkkkkkkkkkkkkkkkk.kkk");
    CHECK(st == 0);
    CLS(html, st, "<scripts>", "kkkkkkkkk");                  /* another tag: not a script */
    CHECK(st == 0);
    CLS(html, st, "<script", "kkkkkkk");                       /* tag not closed on the line */
    CHECK(st != 0 && st < 0x8000);                             /* an ordinary multi-line tag */
    CLS(html, st, "src=\"a.js\">", "kkkkkkkkkkk");
    CHECK(st == 0);

    /* CSS between style tags */
    CLS(html, st, "<style>", "kkkkkkk");
    CHECK(st != 0);
    CLS(html, st, "a { color: red; }", "T...kkkkko.......");
    CLS(html, st, "</style>", "kkkkkkkk");
    CHECK(st == 0);

    /* PHP tags inside HTML, on one line or across lines */
    CLS(html, st, "<?php echo $x; ?> <i>", "ppppp.kkkk.vv..pp.kkk");
    CHECK(st == 0);
    CLS(html, st, "<b><?= $x", "kkkppp.vv");
    CLS(html, st, "?></b>", "ppkkkk");
    CHECK(st == 0);

    /* PHP with HTML after ?>, and back at the next tag */
    st = 0;
    CLS(php, st, "$a = 1; ?>", "vv.o.n..pp");
    CHECK(st != 0);
    CLS(php, st, "<div><?php if ($a): ?>", "kkkkkppppp.kk..vv.o.pp");
    CHECK(st != 0);
    CLS(php, st, "<?php", "ppppp");
    CHECK(st == 0);
    CLS(php, st, "x", ".");

    /* a template starts as HTML */
    st = 0;
    CLS(tpl, st, "<ul><?php foreach ($a as $b): ?>", "kkkkppppp.kkkkkkk..vv.kk.vv.o.pp");
    CHECK(st == 0);

    /* one level only: no script inside the HTML that follows ?>, no PHP inside a script body */
    CLS(php, st, "?><script>", "ppkkkkkkkk");
    CHECK(st != 0);
    CLS(php, st, "var x = 1;", "..........");                  /* the HTML of the PHP file: no JS */
    st = 0;
    CLS(html, st, "<script>", "kkkkkkkk");
    CLS(html, st, "a = \"<?php\"; </script>", "..o.sssssss..kkkkkkkkk");
    CHECK(st == 0);

    /* the comment markers follow the language the line starts in */
    buf_init(&b);
    {
        size_t y = 0, x = 0;
        static const char text[] = "<p>\n<script>\nvar a;\n</script>\n<style>\na {}\n</style>\n";

        buf_insert_text(&b, &y, &x, text, sizeof text - 1);
    }
    s = html;
    CHECK(syn_comment_at(s, &b, 0, &open, &close) && strcmp(open, "<!--") == 0);
    CHECK(syn_comment_at(s, &b, 2, &open, &close) && strcmp(open, "//") == 0 && !close);
    CHECK(syn_comment_at(s, &b, 3, &open, &close) && strcmp(open, "//") == 0);
    CHECK(syn_comment_at(s, &b, 4, &open, &close) && strcmp(open, "<!--") == 0);
    CHECK(syn_comment_at(s, &b, 5, &open, &close) && strcmp(open, "/*") == 0 && close);
    CHECK(!syn_comment_at(NULL, &b, 0, &open, &close));
    buf_free(&b);

    /* the directive: errors and a language that does not exist */
    CHECK(syn_parse("name X\nembed Y keyword <a\n", 28, err, sizeof err) == NULL &&
          strstr(err, "line 2") != NULL);
    CHECK(syn_parse("name X\nembed Y nonsense <a </a>\n", 34, err, sizeof err) == NULL);
    CHECK(syn_parse("name X\nembed Y keyword <a </a> what\n", 38, err, sizeof err) == NULL);
    {
        static const char def[] = "name X\nembed NoSuchLanguage keyword <a </a>\nregion string \" \"\n";
        Syntax *x = syn_parse(def, sizeof def - 1, err, sizeof err);

        CHECK(x != NULL);
        st = 0;
        CLS(x, st, "<a \"s\"", "...sss");                        /* ignored: the other rules apply */
        CHECK(st == 0);
        syn_free(x);
    }
    syn_free(html);
    syn_free(php);
    syn_free(tpl);
}

static void test_syntax_detect_langs(void)
{
    static const struct { const char *path, *first, *want; } t[] = {
        { "a.php", "", "PHP" }, { "a.php", "<?php\n", "PHP" }, { "a.phtml", "", "PHP/HTML" },
        { "a.php", "<html>\n", "PHP/HTML" }, { "a.php", "\n  <?php\n", "PHP" },
        { "a.php", "#!/usr/bin/env php\n<?php\n", "PHP" }, { "run", "#!/usr/bin/php\n", "PHP" },
        { "a.ts", "", "TypeScript" }, { "a.js", "", "JavaScript" }, { "a.jsx", "", "JavaScript" },
        { "a.html", "", "HTML" }, { "a.svg", "", "XML" }, { "a.css", "", "CSS" },
        { "a.json", "", "JSON" }, { ".eslintrc", "", "JSON" }, { "a.yml", "", "YAML" },
        { "a.toml", "", "TOML" }, { "a.sql", "", "SQL" }, { "a.go", "", "Go" },
        { "a.rs", "", "Rust" }, { "A.java", "", "Java" }, { "a.cs", "", "C#" },
        { "a.rb", "", "Ruby" }, { "Rakefile", "", "Ruby" }, { "run", "#!/usr/bin/env ruby\n", "Ruby" },
        { "Makefile", "", "Makefile" }, { "rules.mk", "", "Makefile" },
        { "Dockerfile", "", "Dockerfile" }, { "a.patch", "", "Diff" },
        { "a.txt", "// xted: ft=typescript\n", "TypeScript" },
        { ".env", "", "Dotenv" }, { "app/.env.local", "", "Dotenv" }, { ".env.example", "", "Dotenv" },
        { ".env.production", "", "Dotenv" }, { "prod.env", "", "Dotenv" }, { ".flaskenv", "", "Dotenv" },
        { ".environment", "", "-" },
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
    char dir[] = "/tmp/xted_mouse_XXXXXX";
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

static void test_mouse_borders(void)
{
    Editor e;
    MouseHit h;
    Win *w[8], *a, *b, *c;

    ed_load(&e, "one\ntwo\n");
    mouse_screen(&e, 0);
    CHECK(editor_split(&e, 1) == 0);                        /* a | b */
    mouse_screen(&e, 0);
    CHECK(layout_wins(&e.layout, w, 8) == 2);
    a = w[0];
    b = w[1];
    CHECK(a->w == 39 && b->w == 40);
    mouse_locate(&e, 39, 5, &h);
    CHECK(h.kind == MH_BORDER && h.node && h.node->vertical);

    mclick(&e, MOUSE_PRESS, 39, 5, 0, 0);
    CHECK(e.mouse.drag == DRAG_BORDER && e.layout.active == b);     /* a separator does not focus */
    mclick(&e, MOUSE_DRAG, 30, 6, 0, 0);
    CHECK(a->w == 30 && a->w + 1 + b->w == 80);
    mclick(&e, MOUSE_DRAG, 1, 6, 0, 0);
    CHECK(a->w == WIN_MIN_W);
    mclick(&e, MOUSE_DRAG, 70, 6, 0, 0);
    CHECK(b->w == WIN_MIN_W);
    mclick(&e, MOUSE_DRAG, 25, 6, 0, 0);
    mclick(&e, MOUSE_RELEASE, 25, 6, 0, 0);
    CHECK(e.mouse.drag == DRAG_NONE && a->w == 25);
    mclick(&e, MOUSE_DRAG, 40, 6, 0, 0);                    /* no drag running: nothing moves */
    CHECK(a->w == 25);

    e.mouse.t = 0;
    mclick(&e, MOUSE_PRESS, 25, 3, 0, 0);                   /* double click: equal shares */
    mclick(&e, MOUSE_RELEASE, 25, 3, 0, 0);
    mclick(&e, MOUSE_PRESS, 25, 3, 0, 0);
    mclick(&e, MOUSE_RELEASE, 25, 3, 0, 0);
    CHECK(a->w == 39 && b->w == 40);

    /* stacked panes: the status line of the upper pane is the border */
    CHECK(editor_split(&e, 0) == 0);                        /* a | (b over c) */
    mouse_screen(&e, 0);
    CHECK(layout_wins(&e.layout, w, 8) == 3);
    b = w[1];
    c = w[2];
    mouse_locate(&e, b->x + 3, b->y + b->h - 1, &h);
    CHECK(h.kind == MH_STATUS && h.node && !h.node->vertical && h.win == b);
    mouse_locate(&e, b->x + 3, b->y + 2, &h);
    CHECK(h.kind == MH_TEXT && h.node == NULL);
    mclick(&e, MOUSE_PRESS, b->x + 3, b->y + b->h - 1, 0, 0);
    CHECK(e.layout.active == b && e.mouse.drag == DRAG_BORDER);
    mclick(&e, MOUSE_DRAG, b->x + 3, b->y + 5, 0, 0);       /* the status line goes to row y + 5 */
    CHECK(b->h == 6 && b->h + c->h == 23);
    mclick(&e, MOUSE_RELEASE, b->x + 3, b->y + 5, 0, 0);
    mouse_locate(&e, a->w, 4, &h);
    CHECK(h.kind == MH_BORDER && h.node->vertical);         /* the separator beats the status line */
    mouse_locate(&e, a->w, a->h - 1, &h);
    CHECK(h.kind == MH_BORDER);

    /* the browser's width follows the pointer between 10 columns and half the screen */
    e.sidebar_on = 1;
    editor_relayout(&e);
    CHECK(e.sidebar_w == 28);
    mouse_locate(&e, 28, 5, &h);
    CHECK(h.kind == MH_EDGE);
    mclick(&e, MOUSE_PRESS, 28, 5, 0, 0);
    mclick(&e, MOUSE_DRAG, 20, 5, 0, 0);
    CHECK(e.sidebar_cols == 20 && e.sidebar_w == 20 && e.text_x == 21);
    mclick(&e, MOUSE_DRAG, 2, 5, 0, 0);
    CHECK(e.sidebar_cols == 10);
    mclick(&e, MOUSE_DRAG, 75, 5, 0, 0);
    CHECK(e.sidebar_cols == 40);
    mclick(&e, MOUSE_RELEASE, 75, 5, 0, 0);
    editor_free(&e);
}

static void kkey(Editor *e, uint32_t code, int mods)
{
    Key k;

    k.code = code;
    k.mods = mods;
    editor_key(e, k);
}

static void test_resize_mode(void)
{
    Editor e;
    Win *w[8], *a, *b;

    ed_load(&e, "one\ntwo\n");
    mouse_screen(&e, 0);
    kkey(&e, 'e', MOD_ALT);                                 /* one pane: nothing to resize */
    CHECK(!e.resize_mode);

    CHECK(editor_split(&e, 1) == 0);
    mouse_screen(&e, 0);
    layout_wins(&e.layout, w, 8);
    a = w[0];
    b = w[1];
    CHECK(e.layout.active == b && a->w == 39);
    kkey(&e, 'e', MOD_ALT);
    CHECK(e.resize_mode && strstr(e.msg, "Resize") != NULL);
    kkey(&e, KEY_LEFT, 0);                                  /* b grows: its border moves left */
    CHECK(a->w == 37 && b->w == 42);
    kkey(&e, KEY_LEFT, MOD_SHIFT);
    CHECK(a->w == 27);
    kkey(&e, KEY_RIGHT, 0);
    CHECK(a->w == 29);
    kkey(&e, KEY_UP, 0);                                    /* no stacked split */
    CHECK(a->w == 29 && e.resize_mode);
    kkey(&e, '=', 0);
    CHECK(a->w == 39 && e.resize_mode);
    CHECK(e.cy == 0 && e.cx == 0);                          /* the arrows did not move the cursor */
    kkey(&e, KEY_ESC, 0);
    CHECK(!e.resize_mode && e.msg[0] == '\0');

    kkey(&e, 'e', MOD_ALT);                                 /* another key ends it and is handled */
    kkey(&e, KEY_DOWN, 0);
    CHECK(e.resize_mode);
    kkey(&e, 'x', 0);
    CHECK(!e.resize_mode && e.buf->lines[0].len == 4);
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
    snprintf(buf, n, "/tmp/xted-test-XXXXXX");
#ifdef _WIN32
    /* as the editor writes it: C:/tmp/... (with the drive) */
    if (!mkdtemp(buf) || !sys_realpath(buf, buf))
        return NULL;
    return buf;
#else
    return mkdtemp(buf);
#endif
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

static void test_layout_resize(void)
{
    Doc *d = doc_new();
    Layout l;
    Win *a, *b, *c;
    Node *n;
    int w0;

    layout_init(&l, d);
    layout_compute(&l, 0, 1, 80, 22);
    a = l.active;
    CHECK(layout_border_at(&l, 5, 5) == NULL);
    b = layout_split(&l, 1);                                   /* a | b */
    layout_compute(&l, 0, 1, 80, 22);
    CHECK(a->w == 39 && b->w == 40);
    n = layout_border_at(&l, a->w, 7);
    CHECK(n && n == l.root && n->vertical);
    CHECK(layout_border_at(&l, a->w - 1, 7) == NULL && layout_border_at(&l, a->w, 0) == NULL);

    /* the size of the first half is exact, wherever the pointer is */
    for (w0 = 12; w0 <= 66; w0++) {
        layout_set_size(n, w0);
        layout_refresh(&l);
        if (a->w != w0 || a->w + 1 + b->w != 80)
            fprintf(stderr, "set_size %d: a=%d b=%d\n", w0, a->w, b->w);
        CHECK(a->w == w0 && a->w + 1 + b->w == 80);
    }
    layout_set_size(n, 2);                                     /* the minimum width applies */
    layout_refresh(&l);
    CHECK(a->w == WIN_MIN_W);
    layout_set_size(n, 79);
    layout_refresh(&l);
    CHECK(b->w == WIN_MIN_W);

    /* the arrow pushes the border on its side; otherwise the other one */
    layout_set_size(n, 40);
    layout_refresh(&l);
    l.active = a;
    CHECK(layout_resize(&l, a, LAYOUT_RIGHT, 5) == 0 && a->w == 45);   /* a grows */
    CHECK(layout_resize(&l, a, LAYOUT_LEFT, 7) == 0 && a->w == 38);    /* a has no left border */
    CHECK(layout_resize(&l, b, LAYOUT_LEFT, 3) == 0 && a->w == 35);    /* b grows */
    CHECK(layout_resize(&l, b, LAYOUT_RIGHT, 4) == 0 && a->w == 39);   /* b has no right border */
    CHECK(layout_resize(&l, a, LAYOUT_UP, 1) == -1 && layout_resize(&l, a, LAYOUT_DOWN, 1) == -1);

    /* stacked: the border is the status line of the upper pane */
    c = layout_split(&l, 0);                                   /* (a over c) | b */
    layout_compute(&l, 0, 1, 80, 22);
    CHECK(c == l.active && a->h + c->h == 22);
    n = layout_border_at(&l, 3, a->h);
    CHECK(n == NULL || n->vertical == 0);
    n = layout_border_at(&l, 3, a->y + a->h - 1);
    CHECK(n && !n->vertical && n->a->h == a->h);
    CHECK(layout_border_at(&l, 3, a->y + a->h - 2) == NULL);
    layout_set_size(n, 6);
    layout_refresh(&l);
    CHECK(a->h == 6 && c->h == 16 && c->y == a->y + 6);
    layout_set_size(n, 1);
    layout_refresh(&l);
    CHECK(a->h == WIN_MIN_H);
    CHECK(layout_resize(&l, c, LAYOUT_UP, 3) == 0 && a->h == WIN_MIN_H);   /* already at the minimum */
    layout_set_size(n, 11);
    layout_refresh(&l);
    CHECK(layout_resize(&l, c, LAYOUT_UP, 3) == 0 && a->h == 8);
    CHECK(layout_resize(&l, a, LAYOUT_DOWN, 2) == 0 && a->h == 10);

    layout_equalize(&l);
    CHECK(a->h == 11 && layout_border_at(&l, 3, a->y + 10) != NULL);
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
    CHECK(strcmp(syn_name(d->syn), "Text") == 0 || d->syn == NULL);    /* language follows the name */
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
    /* read again (a file made, a folder made, one removed): the open folders stay open,
     * the selected row stays selected; with show, the way to it is opened and it is selected */
    {
        char p[700];
        size_t k;

        tree_toggle(&t, i);                                     /* src open */
        for (k = 0; k < t.n && strcmp(t.e[k].name, "z.txt") != 0; k++)
            ;
        t.sel = k;
        put_file(dir, "m.txt", "");
        put_file(dir, "src/a.c", "");
        tree_refresh(&t, NULL);
        for (k = 1; k < t.n && strcmp(t.e[k].name, "src") != 0; k++)
            ;
        CHECK(k < t.n && t.e[k].open && strcmp(t.e[k + 1].name, "a.c") == 0 &&
              strcmp(t.e[k + 2].name, "main.c") == 0);
        CHECK(t.sel < t.n && strcmp(t.e[t.sel].name, "z.txt") == 0);
        for (k = 1; k < t.n && strcmp(t.e[k].name, "m.txt") != 0; k++)
            ;
        CHECK(k < t.n && strcmp(t.e[k - 1].name, "a.txt") == 0);
        snprintf(p, sizeof p, "%s/build/new.o", dir);           /* in a closed folder */
        put_file(dir, "build/new.o", "");
        tree_refresh(&t, p);
        CHECK(t.sel < t.n && strcmp(t.e[t.sel].name, "new.o") == 0 && t.e[t.sel].depth == 2 &&
              strcmp(t.e[t.sel - 1].name, "build") == 0 && t.e[t.sel - 1].open);
        snprintf(p, sizeof p, "%s/m.txt", dir);
        remove(p);
        tree_refresh(&t, NULL);
        for (k = 1; k < t.n && strcmp(t.e[k].name, "m.txt") != 0; k++)
            ;
        CHECK(k == t.n);
    }
    tree_free(&t);
    rm_rf(dir);
}

#ifdef _WIN32
static DWORD WINAPI ipc_sender(LPVOID arg)
{
    return ipc_send((const IpcFile *)arg, 2) ? 0 : 1;
}
#endif

/* The single instance: the request, the queue, and a real channel between two of us. */
static void test_ipc(void)
{
    IpcFile f[2], *got;
    SBuf b = SBUF_INIT;
    size_t n, k;
    intptr_t h;
    char addr[64];
    int sent_ok = 0;

    f[0].path = "/a b/x.c";
    f[0].line = 3;
    f[0].col = 4;
    f[1].path = "C:/y.md";
    f[1].line = f[1].col = 0;
    ipc_request(&b, f, 2);
    CHECK(ipc_complete(b.p, b.len) && !ipc_complete(b.p, b.len - 1));
    CHECK(ipc_queue(b.p, b.len) == 2);
    got = ipc_take(&n);
    CHECK(n == 2 && got && strcmp(got[0].path, "/a b/x.c") == 0 && got[0].line == 3 && got[0].col == 4 &&
          strcmp(got[1].path, "C:/y.md") == 0 && got[1].line == 0);
    for (k = 0; k < n; k++)
        free(got[k].path);
    free(got);
    CHECK(ipc_take(&n) == NULL && n == 0);
    CHECK(ipc_queue("garbage\n\n", 9) == 0);                  /* not a request */
    sbuf_free(&b);

    /* the channel, at an address of the tests' own */
#ifdef _WIN32
    snprintf(addr, sizeof addr, "xted-test-%lu", (unsigned long)GetCurrentProcessId());
#else
    snprintf(addr, sizeof addr, "/tmp/xted-test-%ld.sock", (long)getpid());
#endif
    setenv("XTED_IPC", addr, 1);
    CHECK(ipc_send(f, 2) == 0);                                 /* no one listens yet */
    h = ipc_listen();
    CHECK(h != -1);
#ifdef _WIN32
    {
        HANDLE t = CreateThread(NULL, 0, ipc_sender, f, 0, NULL);
        DWORD code = 1;

        CHECK(t != NULL && WaitForSingleObject((HANDLE)h, 3000) == WAIT_OBJECT_0);
        ipc_serve();
        WaitForSingleObject(t, 5000);
        GetExitCodeThread(t, &code);
        CloseHandle(t);
        sent_ok = code == 0;
    }
#else
    {
        pid_t pid = fork();
        struct pollfd p;
        int st = 1;

        if (pid == 0)
            _exit(ipc_send(f, 2) ? 0 : 1);
        p.fd = (int)h;
        p.events = POLLIN;
        CHECK(pid > 0 && poll(&p, 1, 3000) == 1);
        ipc_serve();
        waitpid(pid, &st, 0);
        sent_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }
#endif
    CHECK(sent_ok);
    got = ipc_take(&n);
    CHECK(n == 2 && got && strcmp(got[1].path, "C:/y.md") == 0);
    for (k = 0; k < n; k++)
        free(got[k].path);
    free(got);
    ipc_close();
    CHECK(ipc_send(f, 2) == 0);                                 /* closed: no one takes them */
    unsetenv("XTED_IPC");
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

#ifdef _WIN32
        /* C:/tmp/...: the drive is in the name, without its ':' */
        CHECK(f && strstr(f, "/xted/sessions/") != NULL && !strchr(strstr(f, "/sessions/"), ':'));
#else
        CHECK(f && strstr(f, "/xted/sessions/%") != NULL && strncmp(f, dir, strlen(dir)) == 0);
#endif
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

#ifndef XTED_NO_REGEX
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
#else
    CHECK(sm_compile(&m, "a*", 2, 1, 0, 0) < 0 && m.err[0] != '\0');  /* no regular expressions */
    (void)g;
#endif

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
#ifndef XTED_NO_REGEX
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
#else
    (void)g;
    (void)o;
    (void)a;
    (void)e;
    buf_free(b);                                /* the text the regex would have given */
    free(b);
    b = buf_of("1a 22b\n333c 4a\nnone");
#endif

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
    CHECK(e.picker.on && e.picker_kind == PK_COMMANDS && e.picker.nitems > ncommands);
    CHECK(e.picker.items[0][0] == PICKER_HEAD);
    CHECK(e.picker.sel == 1 && strstr(e.picker.items[1], commands[0].name) != NULL);
    CHECK(e.pal[1] == 0);
    /* actions before settings; a setting shows its value */
    for (i = 1; i < ncommands; i++)
        CHECK(commands[i].group >= commands[i - 1].group);
    for (i = 0; i < ncommands; i++)
        if (strcmp(commands[i].id, "toggle_autopair") == 0) {
            char v[24];

            CHECK(commands[i].group == G_SETTINGS);
            CHECK(command_state(&e, (int)i, v, sizeof v) >= 0 && v[0]);
        } else if (strcmp(commands[i].id, "save") == 0) {
            char v[24];

            CHECK(command_state(&e, (int)i, v, sizeof v) == -1);
        }
    /* with a query the headings go away, and the arrows never stop on one */
    picker_type(&e.picker, 's');
    CHECK(e.picker.nshown > 0 && !picker_is_head(&e.picker, 0));
    picker_filter(&e.picker);
    e.picker.qlen = 0;
    e.picker.query[0] = '\0';
    picker_filter(&e.picker);
    picker_move(&e.picker, -5);
    CHECK(e.picker.sel == 1);
    for (i = 0; i < 40; i++) {
        picker_move(&e.picker, 1);
        CHECK(!picker_is_head(&e.picker, e.picker.sel));
    }
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
        /* the C library adds fuzzy matches (rename_t_u...), the keyword still comes first */
        CHECK(c.n >= 1 && strcmp(c.v[0].text, "return") == 0 && c.v[0].kind == CAND_KEYWORD);
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

#ifdef _WIN32
/* The commands on Windows: cmd.exe in place of /bin/sh, the same rules otherwise. */
static void test_shell_win(void)
{
    SBuf out = SBUF_INIT, err = SBUF_INIT;
    const char *echo_all[] = { "/bin/sh", "-c", "findstr \"^\"", NULL };
    const char *two[] = { "/bin/sh", "-c", "echo out& (echo bad)1>&2& exit /b 3", NULL };
    const char *slow[] = { "/bin/sh", "-c", "ping -n 6 127.0.0.1 >nul", NULL };
    const char *big[] = { "/bin/sh", "-c", "for /l %i in (1,1,2000) do @echo xxxxxxxxxx", NULL };
    const char *utf[] = { "/bin/sh", "-c", "echo \xc3\xa0\xc3\xa8", NULL };
    const char *sort[] = { "/bin/sh", "-c", "sort", NULL };
    const char *where[] = { "/bin/sh", "-c", "cd", NULL };
    const char *bad[] = { "C:/nonexistent/prog.exe", NULL };
    const char *argv_q[] = { "cmd.exe", "/d", "/c", "exit 7", NULL };
    int st = 0, guard;
    Job j;
    char e[128], buf[256];
    long t0, n;

    /* stdin to stdout, more than a pipe holds; CR LF come as LF */
    {
        char *in = xmalloc(300000);
        size_t i;

        for (i = 0; i < 300000; i++)
            in[i] = i % 100 == 99 ? '\n' : 'x';
        CHECK(shell_capture(echo_all, NULL, in, 300000, &out, &err, 10000, 1 << 20, &st) == 0);
        CHECK(out.len == 300000 && st == 0 && memchr(out.p, '\r', out.len) == NULL);
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

    /* UTF-8 to the command (the manifest's code page), its OEM output back as UTF-8 */
    CHECK(shell_capture(utf, NULL, NULL, 0, &out, NULL, 5000, 1 << 20, &st) == 0);
    CHECK(out.len == 5 && memcmp(out.p, "\xc3\xa0\xc3\xa8\n", 5) == 0);
    sbuf_free(&out);

    /* sort as a filter (with chcp 65001 it wrote nothing) */
    CHECK(shell_capture(sort, NULL, "pera\nmela\nbanana\n", 17, &out, NULL, 5000, 1 << 20, &st) == 0);
    CHECK(st == 0 && out.len == 17 && memcmp(out.p, "banana\nmela\npera\n", 17) == 0);
    sbuf_free(&out);

    /* the folder given */
    CHECK(shell_capture(where, "C:/Windows", NULL, 0, &out, NULL, 5000, 1 << 20, &st) == 0);
    CHECK(out.len >= 11 && _strnicmp(out.p, "C:\\Windows\n", 11) == 0);
    sbuf_free(&out);

    /* a command that is too slow is killed */
    t0 = (long)time(NULL);
    CHECK(shell_capture(slow, NULL, NULL, 0, &out, NULL, 300, 1 << 20, &st) == -2);
    CHECK((long)time(NULL) - t0 < 4);
    sbuf_free(&out);

    /* output over the cap is dropped, not an error */
    CHECK(shell_capture(big, NULL, NULL, 0, &out, NULL, 10000, 1000, &st) == 0 && out.len == 1000);
    sbuf_free(&out);

    /* a program that does not exist: status 127; arguments quoted and split again */
    CHECK(shell_capture(bad, NULL, NULL, 0, &out, NULL, 2000, 100, &st) == 0 && st == 127);
    CHECK(shell_capture(argv_q, NULL, NULL, 0, &out, NULL, 5000, 100, &st) == 0 && st == 7);
    sbuf_free(&out);

    /* in the background: the output, then the exit code */
    CHECK(shell_start(&j, "echo ciao& exit /b 4", NULL, e, sizeof e) == 0);
    for (guard = 0; guard < 500; guard++) {
        n = shell_read(&j, buf, sizeof buf);
        if (n > 0)
            sbuf_add(&out, buf, (size_t)n);
        else if (n == 0)
            break;
        else
            Sleep(10);
    }
    CHECK(out.len == 5 && memcmp(out.p, "ciao\n", 5) == 0);
    for (guard = 0; guard < 500 && !shell_reap(&j); guard++)
        Sleep(10);
    CHECK(shell_exit_code(&j) == 4);
    shell_close(&j);
    sbuf_free(&out);

    /* the same with letters outside ASCII: OEM as UTF-8 in the pieces read too */
    CHECK(shell_start(&j, "echo \xc3\xa0\xc3\xa8 ok", NULL, e, sizeof e) == 0);
    for (guard = 0; guard < 500; guard++) {
        n = shell_read(&j, buf, sizeof buf);
        if (n > 0)
            sbuf_add(&out, buf, (size_t)n);
        else if (n == 0)
            break;
        else
            Sleep(10);
    }
    CHECK(out.len == 8 && memcmp(out.p, "\xc3\xa0\xc3\xa8 ok\n", 8) == 0);
    shell_close(&j);
    sbuf_free(&out);

    /* stopping one ends it at once */
    CHECK(shell_start(&j, "ping -n 30 127.0.0.1", NULL, e, sizeof e) == 0);
    t0 = (long)time(NULL);
    shell_stop(&j);
    for (guard = 0; guard < 300 && !shell_reap(&j); guard++)
        Sleep(10);
    CHECK(shell_reap(&j) && (long)time(NULL) - t0 < 3);
    shell_close(&j);
}
#endif

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
    char dir[] = "/tmp/xted_run_XXXXXX";
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
        CHECK(b->nlines > 5 && strstr(b->lines[b->nlines - 2].s, "exited with 3") != NULL);
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
    CHECK(buf_is(e.buf, "keep") && strstr(e.msg, "error") != NULL);
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
    /* a command with several keys lists them all */
    keymap_label(&m, command_by_id("next_win"), out, sizeof out);
    CHECK(strstr(out, "Alt+O") && strstr(out, "Alt+Right") && strstr(out, ", "));
    keymap_label(&m, command_by_id("next_win"), out, 8);
    CHECK(strlen(out) < 8);
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
        "autopair = off\n"
        "\n"
        "[ keys ]\n"
        "ctrl+s = save\n"
        "alt+y = duplicate_down\n"
        "ctrl+shift+f = none\n";

    config_init(&c);
    CHECK(config_parse(&c, text, strlen(text)) == 0 && c.first_error[0] == '\0');
    CHECK(strcmp(c.theme, "chiaro") == 0 && c.clipboard == 0 && c.git == 1);
    CHECK(c.nkeys == 3 && strcmp(c.keys[0].key, "ctrl+s") == 0 && strcmp(c.keys[0].cmd, "save") == 0);
    CHECK(strcmp(c.keys[2].cmd, "none") == 0 && c.keys[2].line == 17);
    l = config_lang(&c, "C");
    CHECK(l.tab_width == 4 && l.expand_tab == 1 && l.final_newline == 1 && l.trim_trailing_space == 0);
    CHECK(l.autosuggest == 1 && l.suggest_min == 2 && l.autopair == 1);
    l = config_lang(&c, "python");
    CHECK(l.tab_width == 2 && l.expand_tab == 1 && l.trim_trailing_space == 1 && l.autopair == 0);
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
        CHECK(strstr(c.first_error, "line 1:") != NULL && strstr(c.first_error, "tab_width") != NULL);
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
        CHECK(d && strcmp(d, "/tmp/xdgtest/xted") == 0);
        free(d);
        unsetenv("XDG_CONFIG_HOME");
        setenv("HOME", "/tmp/hometest", 1);
        d = config_dir();
        CHECK(d && strcmp(d, "/tmp/hometest/.config/xted") == 0);
        free(d);
    }

    /* loading from a folder; a missing file is not an error */
    {
        char dir[] = "/tmp/xted_cfg_XXXXXX";
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
    char dir[] = "/tmp/xted_xdg_XXXXXX";
    char path[300];
    FILE *f;
    Key k;
    char err[64];

    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/xted", dir);
    CHECK(mkdir(path, 0700) == 0);
    snprintf(path, sizeof path, "%s/xted/config", dir);
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
    CHECK(strcmp(e.theme->name, "light") == 0 && e.clipboard_osc52 == 0);
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
    snprintf(path, sizeof path, "%s/xted/config", dir);
    f = fopen(path, "w");
    fputs("theme = nonesiste\n[keys]\nalt+j = no_such_command\nhyper+x = save\nctrl+i = save\n", f);
    fclose(f);
    ed_load(&e, "x");
    editor_load_config(&e);
    CHECK(e.cfg.nerrors == 4 && strncmp(e.msg, "config:", 7) == 0);
    CHECK(strstr(e.msg, "nonesiste") != NULL && strstr(e.msg, "other") != NULL);
    editor_free(&e);

    unsetenv("XDG_CONFIG_HOME");
    remove(path);
    snprintf(path, sizeof path, "%s/xted", dir);
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
    char dir[] = "/tmp/xted_user_XXXXXX";
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
    CHECK(theme_by_name("dark") == 0 && theme_by_name("light") == 1 && theme_by_name("scuro") == 0 &&
          theme_by_name("chiaro") == 1 && theme_by_name("zzz") == -1);
    CHECK(theme_get(n0)->has_bg && theme_get(n0 + 1) == theme_get(0));      /* wraps around */
    CHECK(theme_add_user("name X\nbackground none\n", 24, err, sizeof err) == 0);
    CHECK(!theme_get(n0 + 1)->has_bg);
    /* errors */
    CHECK(theme_add_user("name X\ncomment red\n", 20, err, sizeof err) < 0 && strstr(err, "line 2") != NULL);
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
    CHECK(syn_add_user("name Bad\nfrobnicate\n", 20, err, sizeof err) < 0 && strstr(err, "line 2") != NULL);

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
        char xdg[] = "/tmp/xted_xdg2_XXXXXX";
        Buffer b2;
        Syntax *s2;

        CHECK(mkdtemp(xdg) != NULL);
        snprintf(sub, sizeof sub, "%s/xted", xdg);
        CHECK(mkdir(sub, 0700) == 0);
        snprintf(sub, sizeof sub, "%s/xted/themes", xdg);
        CHECK(mkdir(sub, 0700) == 0);
        put_file(sub, "notte", "name Notte\nbackground #101010\n");
        snprintf(sub, sizeof sub, "%s/xted/syntax", xdg);
        CHECK(mkdir(sub, 0700) == 0);
        put_file(sub, "foo", "name Foo\next foo\nwords keyword wibble\n");
        snprintf(sub, sizeof sub, "%s/xted", xdg);
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
    char dir[] = "/tmp/xted_git_XXXXXX";
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
    char dir[] = "/tmp/xted_tags_XXXXXX";
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


/* ---- symbols and member completion -------------------------------------- */

typedef struct {
    char got[1 << 19];          /* the whole standard library is reported by sym_globals */
    int n;
} Got;

static void got_cb(void *ctx, const char *name, int kind, const char *sig, const char *ret,
                   int rank)
{
    Got *g = ctx;
    size_t len = strlen(g->got);

    (void)rank;
    snprintf(g->got + len, sizeof g->got - len, "[%s %d (%s) %s]", name, kind, sig, ret);
    g->n++;
}

static int has_sym(const Got *g, const char *name)
{
    char key[128];

    snprintf(key, sizeof key, "[%s ", name);
    return strstr(g->got, key) != NULL;
}

static const Sym *find_sym(const SymIndex *x, const char *name, int kind)
{
    size_t i;

    for (i = 0; i < x->n; i++)
        if (strcmp(x->v[i].name, name) == 0 && x->v[i].kind == kind)
            return &x->v[i];
    return NULL;
}

/* A Doc on the stack (doc_free frees the Doc itself). */
static void doc_release(Doc *d)
{
    syn_free(d->syn);
    buf_free(&d->buf);
    free(d->title);
}

static void test_symbol_patterns(void)
{
    PatMatch m;
    const char *t;

    t = "public static function foo(int $a, $b = (1)): ?\\App\\Bar {";
    CHECK(pat_match("public|static* function @ ( ^ : %", t, strlen(t), 0, &m));
    CHECK(strcmp(m.name, "foo") == 0 && strcmp(m.params, "int $a, $b = (1)") == 0 &&
          strcmp(m.type, "Bar") == 0);
    /* a keyword is never a name */
    t = "if (x) {";
    CHECK(!pat_match("@ ( ^", t, strlen(t), 0, &m));
    /* a modifier counts only when a name follows */
    t = "get(key) {";
    CHECK(pat_match("static|get* @ ( ^", t, strlen(t), 0, &m) && strcmp(m.name, "get") == 0);
    t = "static get size() {";
    CHECK(pat_match("static|get* @ ( ^", t, strlen(t), 0, &m) && strcmp(m.name, "size") == 0);
    /* look-ahead and end of line */
    t = "struct node";
    CHECK(pat_match("struct @ $$", t, strlen(t), 0, &m) && strcmp(m.name, "node") == 0);
    t = "struct node *make(void)";
    CHECK(!pat_match("struct @ ={|:", t, strlen(t), 0, &m));
    t = "struct node {";
    CHECK(pat_match("struct @ ={", t, strlen(t), 0, &m) && m.end == 12);
    /* types: qualified, generic, pointer */
    t = "std::vector<std::string> *items;";
    CHECK(pat_match("% @ =;", t, strlen(t), 0, &m) && strcmp(m.type, "vector") == 0 &&
          strcmp(m.name, "items") == 0);
    t = "template <typename T> class Box : public Base {";
    CHECK(pat_match("template? <> class|struct @ : public|protected|private|virtual* % ={", t,
                    strlen(t), 0, &m) &&
          strcmp(m.name, "Box") == 0 && strcmp(m.type, "Base") == 0);
    /* paths */
    t = "from ..pkg.mod import x";
    CHECK(pat_match("from & import", t, strlen(t), 0, &m) && strcmp(m.path, "./../pkg/mod") == 0);
    t = "const a = require('./lib/a');";
    CHECK(pat_match("require ~", t, strlen(t), 10, &m) && strcmp(m.path, "./lib/a") == 0);
}

static void call_body_is(const char *name, int kind, const char *sig, int want, const char *body)
{
    Cand c;
    SBuf b = SBUF_INIT;
    int r;

    c.text = (char *)name;
    c.sig = (char *)sig;
    c.kind = kind;
    r = complete_call_body(&c, &b);
    sbuf_add(&b, "", 1);
    CHECK(r == want);
    if (want)
        CHECK(strcmp(b.p, body) == 0);
    sbuf_free(&b);
}

static void test_call_body(void)
{
    call_body_is("strlen", CAND_FUNC, "strlen(const char *s): size_t", 1,
                 "strlen(${1:const char *s})$0");
    call_body_is("put", CAND_METHOD, "put(map<a,b> m, int n)", 1, "put(${1:map<a,b> m}, ${2:int n})$0");
    call_body_is("f", CAND_FUNC, "f($x, $y = 1)", 1, "f(${1:$$x})$0");
    call_body_is("g", CAND_FUNC, "g(a=1, *args)", 1, "g($1)$0");
    call_body_is("h", CAND_FUNC, "h(): int", 2, "h()");
    call_body_is("k", CAND_PROP, "k: int", 0, "");
    call_body_is("m", CAND_FUNC, "m(a, ...)", 1, "m(${1:a})$0");
}

/* The call at a position, the parameter to highlight, the variables of a function. */
static char loc_names[256];

static void loc_cb(void *ctx, const char *name, int kind, const char *sig, const char *ret, int rank)
{
    (void)ctx;
    (void)kind;
    (void)sig;
    (void)ret;
    (void)rank;
    strcat(loc_names, name);
    strcat(loc_names, " ");
}

static int loc_has(const char *name)
{
    char w[80];

    snprintf(w, sizeof w, " %s ", name);
    return strstr(loc_names - 0, w) != NULL || strncmp(loc_names, w + 1, strlen(w) - 1) == 0;
}

static size_t col_after(const Doc *d, size_t y, const char *what)
{
    const char *p = strstr(d->buf.lines[y].s, what);

    return p ? (size_t)(p - d->buf.lines[y].s) + strlen(what) : 0;
}

static void test_params(void)
{
    char dir[64], path[128], err[128], hint[256];
    Doc d;
    SymCache *c = sym_cache_new();
    Doc *docs[1];
    SymCall call;
    size_t from, to, hf, ht;

    CHECK(sym_param_span("const char *dest, const char *src, size_t n", 1, &from, &to) &&
          strncmp(&"const char *dest, const char *src, size_t n"[from], "const char *src", to - from) == 0 &&
          to - from == 15);
    CHECK(!sym_param_span("a, b", 2, &from, &to));
    CHECK(sym_param_span("fmt, ...", 4, &from, &to) && to - from == 3);
    CHECK(sym_param_span("map<a,b> m, int n", 1, &from, &to) && to - from == 5);
    CHECK(!sym_param_span("", 0, &from, &to));

    CHECK(make_dir(dir, sizeof dir) != NULL);
    put_file(dir, "calls.c",
             "#include <string.h>\n"
             "int add(int a, int b)\n"
             "{\n"
             "    int total = 0;\n"
             "    char *buf, *tmp;\n"
             "    for (int i = 0; i < 3; i++) {\n"
             "        total += add(i, total);\n"
             "    }\n"
             "    strncpy(buf, tmp, 2);\n"
             "}\n"
             "int other(int z) { return z; }\n"
             "static int proto(int p);\n"
             "int later;\n");
    snprintf(path, sizeof path, "%s/calls.c", dir);
    memset(&d, 0, sizeof d);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    docs[0] = &d;

    CHECK(sym_call_at(d.syn, &d.buf, 6, col_after(&d, 6, "add(i, "), &call));
    CHECK(strcmp(call.name, "add") == 0 && call.arg == 1 && !call.decl);
    CHECK(sym_call_at(d.syn, &d.buf, 1, col_after(&d, 1, "int add("), &call));
    CHECK(strcmp(call.name, "add") == 0 && call.decl && call.typepos);
    CHECK(sym_call_at(d.syn, &d.buf, 1, col_after(&d, 1, "int add(int a, "), &call) && call.decl && call.arg == 1 &&
          call.typepos);
    CHECK(sym_call_at(d.syn, &d.buf, 1, col_after(&d, 1, "int add(int a, int "), &call) && !call.typepos);
    CHECK(sym_call_at(d.syn, &d.buf, 5, col_after(&d, 5, "for ("), &call) && call.name[0] == '\0' && !call.decl);
    CHECK(!sym_call_at(d.syn, &d.buf, 3, 4, &call));            /* no open parenthesis */
    CHECK(!sym_call_at(d.syn, &d.buf, 7, 0, &call));

    /* the signature of the call at the cursor, with the current parameter */
    CHECK(sym_call_hint(c, docs, 1, &d, 6, col_after(&d, 6, "add(i, "), hint, sizeof hint, &hf, &ht) &&
          strcmp(hint, "add(int a, int b)") == 0 && ht - hf == 5 && strncmp(hint + hf, "int b", 5) == 0);
    CHECK(sym_call_hint(c, docs, 1, &d, 8, col_after(&d, 8, "strncpy(buf, "), hint, sizeof hint, &hf, &ht) &&
          strncmp(hint, "strncpy(", 8) == 0 && ht > hf);
    CHECK(!sym_call_hint(c, docs, 1, &d, 6, col_after(&d, 6, "total += "), hint, sizeof hint, &hf, &ht));

    loc_names[0] = '\0';
    sym_locals(c, &d, 8, col_after(&d, 8, "    "), loc_cb, NULL);
    CHECK(loc_has("a") && loc_has("b") && loc_has("total") && loc_has("buf") && loc_has("tmp") && loc_has("i"));
    CHECK(!loc_has("z") && !loc_has("p"));
    loc_names[0] = '\0';
    sym_locals(c, &d, 10, 0, loc_cb, NULL);                       /* inside other() */
    CHECK(loc_has("z") && !loc_has("total"));
    loc_names[0] = '\0';
    sym_locals(c, &d, 12, 0, loc_cb, NULL);                       /* after a prototype: nothing */
    CHECK(!loc_has("p") && !loc_has("z"));
    syn_free(d.syn);
    buf_free(&d.buf);

    put_file(dir, "k.py",
             "def work(items, limit=3, *rest, **kw):\n"
             "    total = 0\n"
             "    a, b = 1, 2\n"
             "    count: int = 3\n"
             "    for item, n in items:\n"
             "        total += n\n"
             "    with open(items) as fh:\n"
             "        pass\n"
             "    if total == 1:\n"
             "        x = 1\n"
             "\n"
             "def other(q):\n"
             "    return q\n"
             "\n"
             "value = 1\n");
    snprintf(path, sizeof path, "%s/k.py", dir);
    sym_cache_free(c);
    c = sym_cache_new();            /* a Doc at the same address would reuse the old index */
    memset(&d, 0, sizeof d);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    loc_names[0] = '\0';
    sym_locals(c, &d, 9, 0, loc_cb, NULL);
    CHECK(loc_has("items") && loc_has("limit") && loc_has("rest") && loc_has("kw"));
    CHECK(loc_has("total") && loc_has("a") && loc_has("b") && loc_has("count") && loc_has("item") &&
          loc_has("n") && loc_has("fh") && loc_has("x") == 0);        /* x is on the cursor line: not yet */
    loc_names[0] = '\0';
    sym_locals(c, &d, 12, 0, loc_cb, NULL);
    CHECK(loc_has("q") && !loc_has("total"));
    loc_names[0] = '\0';
    sym_locals(c, &d, 14, 0, loc_cb, NULL);                       /* module level */
    CHECK(loc_names[0] == '\0');
    CHECK(sym_call_at(d.syn, &d.buf, 0, 12, &call) && call.decl && strcmp(call.name, "work") == 0);
    syn_free(d.syn);
    buf_free(&d.buf);

    put_file(dir, "m.php",
             "<?php\n"
             "function f(Rep $r, $x) {\n"
             "    $v = 1;\n"
             "    $this->go($v, \"a $s\");\n"
             "}\n");
    snprintf(path, sizeof path, "%s/m.php", dir);
    sym_cache_free(c);
    c = sym_cache_new();
    memset(&d, 0, sizeof d);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    loc_names[0] = '\0';
    sym_locals(c, &d, 3, 0, loc_cb, NULL);
    CHECK(loc_has("r") && loc_has("x") && loc_has("v") && !loc_has("this") && !loc_has("s"));
    CHECK(sym_call_at(d.syn, &d.buf, 3, col_after(&d, 3, "go($v, "), &call) && call.arg == 1 &&
          strcmp(call.name, "go") == 0);
    CHECK(sym_call_at(d.syn, &d.buf, 1, col_after(&d, 1, "function f("), &call) && call.decl && call.typepos);
    CHECK(sym_call_at(d.syn, &d.buf, 1, col_after(&d, 1, "function f(Rep "), &call) && !call.typepos);
    CHECK(sym_call_at(d.syn, &d.buf, 1, col_after(&d, 1, "function f(Rep $r, "), &call) && call.typepos);
    CHECK(sym_call_at(d.syn, &d.buf, 1, col_after(&d, 1, "function f(Rep $r, $"), &call) && !call.typepos);
    syn_free(d.syn);
    buf_free(&d.buf);
    sym_cache_free(c);
}

/* The outline of a file: definitions in file order, no doc-comment members or attributes. */
static char out_text[1024];

static void out_cb(void *ctx, int kind, const char *name, const char *sig, size_t y, size_t x)
{
    char one[160];

    (void)ctx;
    snprintf(one, sizeof one, "%d:%s(%s)@%zu,%zu ", kind, name, sig, y, x);
    strcat(out_text, one);
}

static void test_outline(void)
{
    char dir[64], path[128], err[128];
    Doc d;
    SymCache *c = sym_cache_new();

    CHECK(make_dir(dir, sizeof dir) != NULL);
    put_file(dir, "o.php",
             "<?php\n"
             "/**\n"
             " * @method static Foo make()\n"
             " */\n"
             "class Thing extends Base\n"
             "{\n"
             "    const MAX = 3;\n"
             "    public $level = 1;\n"
             "    public function run(int $n)\n"
             "    {\n"
             "        $this->extra = 1;\n"
             "    }\n"
             "}\n"
             "function helper($a, $b) {}\n");
    snprintf(path, sizeof path, "%s/o.php", dir);
    memset(&d, 0, sizeof d);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    out_text[0] = '\0';
    CHECK(sym_outline(c, &d, out_cb, NULL) == 5);
    CHECK(strstr(out_text, "0:Thing()@4,6") != NULL);                  /* the class, on its line */
    CHECK(strstr(out_text, "2:Thing::run(int $n)@8,20") != NULL);
    CHECK(strstr(out_text, "1:helper($a, $b)@13,9") != NULL);
    CHECK(strstr(out_text, "make") == NULL && strstr(out_text, "extra") == NULL);
    CHECK(strstr(out_text, "Thing::MAX") != NULL && strstr(out_text, "Thing::level") != NULL);
    /* file order: the class before its members before the function */
    CHECK(strstr(out_text, "0:Thing") < strstr(out_text, "Thing::run") &&
          strstr(out_text, "Thing::run") < strstr(out_text, "1:helper"));
    syn_free(d.syn);
    buf_free(&d.buf);
    sym_cache_free(c);
    c = sym_cache_new();
    put_file(dir, "o.py", "class K:\n    def go(self, a):\n        pass\n\ndef free():\n    x = 1\n");
    snprintf(path, sizeof path, "%s/o.py", dir);
    memset(&d, 0, sizeof d);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    out_text[0] = '\0';
    CHECK(sym_outline(c, &d, out_cb, NULL) == 3);
    CHECK(strstr(out_text, "K.go(a)@1,8") != NULL && strstr(out_text, "free()@4,4") != NULL);
    syn_free(d.syn);
    buf_free(&d.buf);
    sym_cache_free(c);
}

/* Names longer than any keyword (13+ letters) are still classes, methods and types. */
static void test_long_names(void)
{
    char dir[64], path[128], err[128];
    Doc d;
    SymCache *c = sym_cache_new();
    Doc *docs[1];
    static Got g;
    size_t y, sep;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    put_file(dir, "long.php",
             "<?php\n"
             "class PiGardenSocketClient\n"
             "{\n"
             "    protected $socketResource;\n"
             "    public function openConnection() {}\n"
             "    public function run()\n"
             "    {\n"
             "        $this->\n"
             "        $probe = new PiGardenAdminController();\n"
             "        $probe->\n"
             "    }\n"
             "}\n"
             "class PiGardenAdminController { public function resetEverything() {} }\n");
    snprintf(path, sizeof path, "%s/long.php", dir);
    memset(&d, 0, sizeof d);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    docs[0] = &d;
    y = 7;
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &d, y, sep, got_cb, &g) == 1);
    CHECK(has_sym(&g, "socketResource") && has_sym(&g, "openConnection") &&
          !has_sym(&g, "resetEverything"));
    y = 9;
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &d, y, sep, got_cb, &g) == 1);
    CHECK(has_sym(&g, "resetEverything") && !has_sym(&g, "openConnection") &&
          !has_sym(&g, "run"));
    sym_cache_free(c);
    doc_release(&d);
}

/* "use X as Y" and static factories give the type of a variable. */
static void test_alias_static(void)
{
    char dir[64], path[128], err[128], t[64];
    Doc d;
    SymCache *c = sym_cache_new();
    Doc *docs[1];
    size_t y, sep;
    static const char *const want[] = { "Foo", "Foo", "Other", "Foo" };

    CHECK(make_dir(dir, sizeof dir) != NULL);
    put_file(dir, "al.php",
             "<?php\n"
             "use App\\Services\\Foo as Bar;\n"
             "class Foo { public static function make() {} public static function o(): Other {}\n"
             "  public static function me(): static {} }\n"
             "class Other {}\n"
             "function f()\n"
             "{\n"
             "    $a = new Bar();\n"
             "    $a->\n"
             "    $b = Foo::make();\n"
             "    $b->\n"
             "    $c = Foo::o();\n"
             "    $c->\n"
             "    $e = \\App\\Foo::me();\n"
             "    $e->\n"
             "    $g = Foo::BAR;\n"
             "    $g->\n"
             "}\n");
    snprintf(path, sizeof path, "%s/al.php", dir);
    memset(&d, 0, sizeof d);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    docs[0] = &d;
    for (y = 8; y <= 14; y += 2) {
        sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
        t[0] = '\0';
        sym_type_before(c, docs, 1, &d, y, sep, t, sizeof t);
        CHECK(strcmp(t, want[(y - 8) / 2]) == 0);
    }
    sep = sym_member_at(d.syn, &d.buf, 16, d.buf.lines[16].len);
    CHECK(!sym_type_before(c, docs, 1, &d, 16, sep, t, sizeof t));     /* a constant: unknown */
    sym_cache_free(c);
    doc_release(&d);
}

/* Laravel: facades documented with @method, global aliases, Composer's PSR-4 map. */
static void count_file(const char *rel, void *ctx)
{
    (void)rel;
    (*(size_t *)ctx)++;
}

/* The scans of the project stop at a time limit and go on at the next call. */
/* The folders were called "ked" before the rename: they stay in use if they are the only ones. */
static void test_app_dir(void)
{
    char dir[64], path[160], *d, *f;
    Session r;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    d = app_dir(dir);
    snprintf(path, sizeof path, "%s/xted", dir);
    CHECK(d && strcmp(d, path) == 0);                           /* neither exists: the new name */
    free(d);
    snprintf(path, sizeof path, "%s/ked", dir);
    CHECK(mkdir(path, 0700) == 0);
    d = app_dir(dir);
    CHECK(d && strcmp(d, path) == 0);                           /* only the old one: it is used */
    free(d);
    snprintf(path, sizeof path, "%s/xted", dir);
    CHECK(mkdir(path, 0700) == 0);
    d = app_dir(dir);
    CHECK(d && strcmp(d, path) == 0);                           /* both: the new one wins */
    free(d);

    /* a session saved by the old name still loads */
    put_file(dir, "old-session", "ked-session 1\nactive 0\nsidebar 0\nbuf\t3\t1\t0\t/tmp/a.txt\n");
    snprintf(path, sizeof path, "%s/old-session", dir);
    CHECK(session_load(path, &r) == 0 && r.n == 1 && r.e[0].y == 3);
    session_free(&r);

    /* the session and config folders follow the same rule */
    CHECK(make_dir(dir, sizeof dir) != NULL);
    snprintf(path, sizeof path, "%s/ked", dir);
    CHECK(mkdir(path, 0700) == 0);
    setenv("XDG_STATE_HOME", dir, 1);
    f = session_file();
#ifdef _WIN32
    CHECK(f && strstr(f, "/ked/sessions/") != NULL && strstr(f, "/xted/") == NULL);
#else
    CHECK(f && strstr(f, "/ked/sessions/%") != NULL && strstr(f, "/xted/") == NULL);
#endif
    free(f);
    unsetenv("XDG_STATE_HOME");
    setenv("XDG_CONFIG_HOME", dir, 1);
    d = config_dir();
    CHECK(d && strcmp(d, path) == 0);
    free(d);
    unsetenv("XDG_CONFIG_HOME");
}

/* The whole file as a string (static buffer), "" if it cannot be read. */
static const char *slurp_file(const char *path)
{
    static char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n = 0;

    if (f) {
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return buf;
}

static void test_cfgsave(void)
{
    char dir[64], file[200], err[200], used[200];
    CfgPair p2[2] = { { "expand_tab", "true" }, { "indent_width", "4" } };
    CfgPair p1[1] = { { "theme", "Chiaro" } };
    char *pr;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    /* a new file, in a folder that does not exist yet */
    snprintf(file, sizeof file, "%s/new/xted/config", dir);
    CHECK(cfgsave_write(file, "Python", p2, 2, err, sizeof err) == 0);
    CHECK(strcmp(slurp_file(file), "[Python]\nexpand_tab = true\nindent_width = 4\n") == 0);
    CHECK(cfgsave_write(file, NULL, p1, 1, err, sizeof err) == 0);       /* top level goes first */
    CHECK(strcmp(slurp_file(file), "theme = Chiaro\n[Python]\nexpand_tab = true\nindent_width = 4\n") == 0);

    /* comments, order and unknown lines stay; a key that is there is replaced in place */
    put_file(dir, "c1",
             "# my settings\n"
             "mouse = off\n"
             "\n"
             "[Python]\n"
             "# four\n"
             "indent_width = 8\n"
             "weird line\n"
             "\n"
             "[keys]\n"
             "ctrl+s = save\n");
    snprintf(file, sizeof file, "%s/c1", dir);
    CHECK(cfgsave_write(file, "python", p2, 2, err, sizeof err) == 0);
    CHECK(strcmp(slurp_file(file),
                 "# my settings\nmouse = off\n\n[Python]\n# four\nindent_width = 4\nweird line\n"
                 "expand_tab = true\n\n[keys]\nctrl+s = save\n") == 0);
    CHECK(cfgsave_write(file, NULL, p1, 1, err, sizeof err) == 0);
    CHECK(strstr(slurp_file(file), "# my settings\nmouse = off\ntheme = Chiaro\n\n[Python]") != NULL);
    CHECK(cfgsave_write(file, "PHP", p2, 1, err, sizeof err) == 0);       /* a new section at the end */
    CHECK(strstr(slurp_file(file), "ctrl+s = save\n\n[PHP]\nexpand_tab = true\n") != NULL);

#ifndef _WIN32
    /* a folder that cannot be written is reported, and nothing is left behind (no /proc on
     * Windows: it would be made) */
    CHECK(cfgsave_write("/proc/xted-no/config", NULL, p1, 1, err, sizeof err) == -1 && err[0]);
#endif

    /* project files */
    snprintf(file, sizeof file, "%s/proj", dir);
    CHECK(mkdir(file, 0700) == 0);
    snprintf(file, sizeof file, "%s/proj/.git", dir);
    CHECK(mkdir(file, 0700) == 0);
    snprintf(file, sizeof file, "%s/proj/src", dir);
    CHECK(mkdir(file, 0700) == 0);
    snprintf(file, sizeof file, "%s/proj/src/a.py", dir);
    pr = cfgsave_find_project(file);
    CHECK(pr == NULL);
    pr = cfgsave_project_root(file);
    snprintf(used, sizeof used, "%s/proj", dir);
    CHECK(pr && strcmp(pr, used) == 0);
    free(pr);
    snprintf(file, sizeof file, "%s/proj/.xted", dir);
    CHECK(cfgsave_write(file, NULL, p2, 1, err, sizeof err) == 0);
    snprintf(file, sizeof file, "%s/proj/src/a.py", dir);
    pr = cfgsave_find_project(file);
    snprintf(used, sizeof used, "%s/proj/.xted", dir);
    CHECK(pr && strcmp(pr, used) == 0);
    free(pr);

    /* .editorconfig: none yet, so one is created in the project folder with root = true */
    snprintf(file, sizeof file, "%s/proj/src/a.py", dir);
    CHECK(cfgsave_editorconfig(file, p2, 2, used, sizeof used, err, sizeof err) == 2);
    snprintf(file, sizeof file, "%s/proj/.editorconfig", dir);
    CHECK(strcmp(used, file) == 0);
    CHECK(strcmp(slurp_file(file), "root = true\n\n[*.py]\nindent_style = space\nindent_size = 4\n") == 0);
    /* a key unknown to .editorconfig is skipped */
    snprintf(file, sizeof file, "%s/proj/src/a.py", dir);
    CHECK(cfgsave_editorconfig(file, p1, 1, used, sizeof used, err, sizeof err) == 0);

    /* an existing file: the last matching section that has the key is the one changed */
    put_file(dir, "proj/.editorconfig",
             "root = true\n\n[*]\nindent_style = tab\nindent_size = 8\n\n[*.py]\nindent_size = 2\n\n"
             "[*.md]\nindent_size = 3\n");
    snprintf(file, sizeof file, "%s/proj/src/a.py", dir);
    CHECK(cfgsave_editorconfig(file, p2, 2, used, sizeof used, err, sizeof err) == 2);
    snprintf(file, sizeof file, "%s/proj/.editorconfig", dir);
    CHECK(strcmp(slurp_file(file),
                 "root = true\n\n[*]\nindent_style = tab\nindent_size = 8\n\n"
                 "[*.py]\nindent_size = 4\nindent_style = space\n\n[*.md]\nindent_size = 3\n") == 0);
    /* the nearest .editorconfig is the one written (a deeper file wins over the root) */
    put_file(dir, "proj/src/.editorconfig", "[*.js]\nindent_size = 2\n");
    snprintf(file, sizeof file, "%s/proj/src/a.py", dir);
    CHECK(cfgsave_editorconfig(file, p2, 1, used, sizeof used, err, sizeof err) == 1);
    snprintf(file, sizeof file, "%s/proj/src/.editorconfig", dir);
    CHECK(strcmp(used, file) == 0 &&
          strcmp(slurp_file(file), "[*.js]\nindent_size = 2\n\n[*.py]\nindent_style = space\n") == 0);
}

/* A project's settings go over the user's: only what the project file says explicitly. */
/* Saving the settings of several documents, and the global ones, in the project's .xted. */
static void test_save_project(void)
{
    char dir[64], cfg[200], path[200], err[200], *old = getenv("XDG_CONFIG_HOME");
    char saved[200] = "";
    Editor e, e2;
    Doc *a, *b, *a2;
    const char *t;
    size_t i;

    if (old)
        snprintf(saved, sizeof saved, "%s", old);
    CHECK(make_dir(dir, sizeof dir) != NULL);
    put_file(dir, "package.json", "{}\n");
    put_file(dir, "a.py", "x = 1\n");
    put_file(dir, "b.php", "<?php\n");
    snprintf(cfg, sizeof cfg, "%s/cfg", dir);
    setenv("XDG_CONFIG_HOME", cfg, 1);

    editor_init(&e);
    editor_load_config(&e);
    snprintf(path, sizeof path, "%s/a.py", dir);
    a = editor_open_doc(&e, path, err, sizeof err);
    snprintf(path, sizeof path, "%s/b.php", dir);
    b = editor_open_doc(&e, path, err, sizeof err);
    CHECK(a && b);
    editor_activate(&e, a);
    editor_cmd_indent_spaces2(&e);
    editor_activate(&e, b);
    editor_cmd_indent_spaces4(&e);
    editor_cmd_theme(&e);
    editor_cmd_toggle_mouse(&e);
    editor_cmd_save_settings(&e);
    CHECK(e.picker.on && e.picker_kind == PK_SAVECFG);
    /* the list says that the other document is saved too */
    CHECK(strstr(e.picker.items[0], "Python") != NULL);
    for (i = 0; i < e.ndests; i++)
        if (e.dests[i].kind == DEST_PROJECT && e.dests[i].lang == 1)
            break;
    CHECK(i < e.ndests);
    picker_select_item(&e.picker, i);
    kkey(&e, KEY_ENTER, 0);
    snprintf(path, sizeof path, "%s/.xted", dir);
    t = slurp_file(path);
    CHECK(strstr(t, "theme = ") != NULL && strstr(t, "mouse = false") != NULL);
    CHECK(strstr(t, "[PHP]\nexpand_tab = true\nindent_width = 4") != NULL);
    CHECK(strstr(t, "[Python]\nexpand_tab = true\nindent_width = 2") != NULL);
    CHECK(e.pend == 0 && a->man.set == 0 && b->man.set == 0);
    {
        char name[64];

        snprintf(name, sizeof name, "%s", e.theme->name);
        editor_free(&e);

        /* a new session in the project reads the global keys from the .xted */
        editor_init(&e2);
        editor_load_config(&e2);
        snprintf(path, sizeof path, "%s/a.py", dir);
        a2 = editor_open_doc(&e2, path, err, sizeof err);
        CHECK(a2 != NULL);
        editor_activate(&e2, a2);
        editor_load_project(&e2);
        CHECK(e2.proj != NULL && strcmp(e2.theme->name, name) == 0 && e2.mouse_on == 0);
        CHECK(a2->expand_tab == 1 && a2->indent_w == 2);
        editor_free(&e2);
    }
    if (saved[0])
        setenv("XDG_CONFIG_HOME", saved, 1);
    else
        unsetenv("XDG_CONFIG_HOME");
    rm_rf(dir);
}

static void test_i18n(void)
{
    char dir[64], path[200], codes[16][16], real[160];
    char *old = getenv("LC_ALL"), saved[64] = "";
    FILE *f;
    char line[1024];
    size_t n;
    Editor e;

    if (old)
        snprintf(saved, sizeof saved, "%s", old);
    CHECK(make_dir(dir, sizeof dir) != NULL);
    snprintf(path, sizeof path, "%s/lang", dir);
    mkdir(path, 0755);
    put_file(path, "xx",
             "# comment\n"
             "@name\tLingua X\n"
             "Save\tSalvataggio\n"
             "Line %zu of %s\tRiga %zu di %s\n"
             "Bad %s\tSbagliato %d\n"
             "Order %s %d\tOrdine %d %s\n"
             "Two\\nlines\tDue\\nrighe\n"
             "Empty\t\n"
             "Percent 100%%\tPercento 100%%\n");
    i18n_init(dir);
    CHECK(i18n_set("en") == 0 && strcmp(i18n_code(), "en") == 0 && strcmp(tr("Save"), "Save") == 0);
    CHECK(i18n_set("xx") == 0 && strcmp(i18n_code(), "xx") == 0 && strcmp(i18n_name(), "Lingua X") == 0);
    CHECK(strcmp(tr("Save"), "Salvataggio") == 0);
    CHECK(strcmp(tr("Line %zu of %s"), "Riga %zu di %s") == 0);
    CHECK(strcmp(tr("Percent 100%%"), "Percento 100%%") == 0);
    CHECK(strcmp(tr("Two\nlines"), "Due\nrighe") == 0);
    /* a translation with other conversions is dropped, so is an empty one; unknown texts stay */
    CHECK(strcmp(tr("Bad %s"), "Bad %s") == 0 && strcmp(tr("Order %s %d"), "Order %s %d") == 0);
    CHECK(strcmp(tr("Empty"), "Empty") == 0 && strcmp(tr("Never seen"), "Never seen") == 0);
    CHECK(i18n_same_formats("%s %d", "%s %d") && !i18n_same_formats("%s %d", "%d %s") &&
          i18n_same_formats("100%% %-5s", "100%% %-5s") && !i18n_same_formats("%zu", "%d") &&
          i18n_same_formats("none", "nulla") && !i18n_same_formats("%s", "none"));
    n = i18n_available(codes, 16);
    {
        size_t k;
        int has_xx = 0;

        for (k = 1; k < n; k++)     /* others may be installed in the system folder */
            has_xx |= strcmp(codes[k], "xx") == 0;
        CHECK(n >= 2 && strcmp(codes[0], "en") == 0 && has_xx);
    }
    /* a language without a catalog: English, and the caller is told */
    CHECK(i18n_set("zz") == -1 && strcmp(i18n_code(), "en") == 0 && strcmp(tr("Save"), "Save") == 0);
    CHECK(i18n_set("../x") == -1);
    /* auto: from the environment */
    setenv("LC_ALL", "xx_XX.UTF-8", 1);
    CHECK(i18n_set("auto") == 0 && strcmp(i18n_code(), "xx") == 0);
    setenv("LC_ALL", "C", 1);
    CHECK(i18n_set("auto") == 0 && strcmp(i18n_code(), "en") == 0);

    /* the catalog that is shipped: every line keeps its conversions, and the palette reads it */
    f = fopen(start_file("lang/it"), "r");
    CHECK(f != NULL);
    while (f && fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');

        if (line[0] == '#' || line[0] == '@' || !tab)
            continue;
        *tab = '\0';
        CHECK(i18n_same_formats(line, tab + 1));
    }
    if (f)
        fclose(f);
    snprintf(real, sizeof real, "%s/lang", dir);
    {
        char *txt;
        long sz;

        f = fopen(start_file("lang/it"), "r");
        CHECK(f != NULL);
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        rewind(f);
        txt = malloc((size_t)sz + 1);
        CHECK(fread(txt, 1, (size_t)sz, f) == (size_t)sz);
        txt[sz] = '\0';
        fclose(f);
        put_file(real, "it", txt);
        free(txt);
    }
    CHECK(i18n_set("it") == 0 && strcmp(i18n_name(), "Italiano") == 0);
    ed_load(&e, "x");
    editor_cmd_palette(&e);
    CHECK(strstr(e.picker.items[0], "Azioni") != NULL && strstr(e.picker.items[1], "Salva") != NULL);
    /* the row of the language says where its catalog was read from */
    {
        size_t k;
        int found = 0;

        for (k = 0; k < e.picker.nitems; k++)
            if (strstr(e.picker.items[k], "Lingua") && strstr(e.picker.items[k], "lang/it") &&
                strstr(e.picker.items[k], "Italiano"))
                found = 1;
        CHECK(found);
        CHECK(i18n_path() != NULL && strstr(i18n_path(), "/lang/it") != NULL);
    }
    picker_close(&e.picker);
    editor_free(&e);
    i18n_set("en");
    i18n_init(NULL);
    if (saved[0])
        setenv("LC_ALL", saved, 1);
    else
        setenv("LC_ALL", "C", 1);
    rm_rf(dir);
}

/* Accepting a symbol of the C library adds the #include it needs, in the same undo step. */
static void test_c_include(void)
{
    char dir[64], path[128], err[128];
    Editor e;
    Doc *d;
    size_t i;
    struct { const char *file, *text, *typed; size_t y, x; const char *line; size_t at; const char *after; } t[] = {
        { "a.c", "#include <stdio.h>\n\nint main(void)\n{\n    \n}\n", "strle", 4, 4,
          "#include <string.h>", 1, "#include <stdio.h>" },
        { "b.c", "#include <stdio.h>\n#include <stdlib.h>\n\nint main(void)\n{\n    \n}\n", "strle", 5, 4,
          "#include <string.h>", 2, "#include <stdlib.h>" },
        { "c.c", "#include <string.h>\n\nint main(void)\n{\n    \n}\n", "strle", 4, 4, NULL, 0, NULL },
        { "d.c", "/* a program */\n\nint main(void)\n{\n    \n}\n", "strle", 4, 4,
          "#include <string.h>", 2, "" },
        { "e.c", "#include \"mine.h\"\n\nint main(void)\n{\n    \n}\n", "strle", 4, 4,
          "#include <string.h>", 0, "" },
        { "f.c", "#ifndef X\n#define X\n\nint main(void)\n{\n    \n}\n#endif\n", "strle", 5, 4,
          "#include <string.h>", 3, "" },
        { "g.c", "#include <stdio.h>\n\nint main(void)\n{\n    \n}\n", "prin", 4, 4, NULL, 0, NULL },
        { "h.py", "def f():\n    \n", "strle", 1, 4, NULL, 0, NULL },
        /* C++: its own library first, so <cstdio> and <vector>; a C file keeps <stdio.h> */
        { "i.cpp", "#include <iostream>\n\nint main()\n{\n    \n}\n", "vecto", 4, 4,
          "#include <vector>", 1, "#include <iostream>" },
        { "j.cpp", "#include <vector>\n\nint main()\n{\n    \n}\n", "stable_so", 4, 4,
          "#include <algorithm>", 0, "" },
        { "k.cpp", "#include <memory>\n\nint main()\n{\n    \n}\n", "snprin", 4, 4,
          "#include <cstdio>", 0, "" },
        { "l.cpp", "#include <stdio.h>\n\nint main()\n{\n    \n}\n", "snprin", 4, 4, NULL, 0, NULL },
        { "m.cpp", "#include <bits/stdc++.h>\n\nint main()\n{\n    \n}\n", "vecto", 4, 4, NULL, 0, NULL },
        { "n.cpp", "#include <cstring>\n\nint main()\n{\n    \n}\n", "strle", 4, 4, NULL, 0, NULL },
        { "o.c", "#include <stdlib.h>\n\nint main(void)\n{\n    \n}\n", "snprin", 4, 4,
          "#include <stdio.h>", 0, "" },
    };

    CHECK(make_dir(dir, sizeof dir) != NULL);
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        const char *q;
        size_t nl0;

        put_file(dir, t[i].file, t[i].text);
        snprintf(path, sizeof path, "%s/%s", dir, t[i].file);
        editor_init(&e);
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d != NULL);
        editor_activate(&e, d);
        nl0 = e.buf->nlines;
        e.cy = t[i].y;
        e.cx = t[i].x;
        for (q = t[i].typed; *q; q++)
            kkey(&e, (uint32_t)(unsigned char)*q, 0);
        if (strstr(t[i].file, ".c"))
            CHECK(e.comp.on);
        if (e.comp.on)
            kkey(&e, KEY_ENTER, 0);
        if (t[i].line) {
            if (!(e.buf->nlines >= nl0 + 1 && line_is(e.buf, t[i].at, t[i].line))) {
                size_t q2;

                fprintf(stderr, "%s:\n", t[i].file);
                for (q2 = 0; q2 < e.buf->nlines; q2++)
                    fprintf(stderr, "  | %.*s\n", (int)e.buf->lines[q2].len, e.buf->lines[q2].s);
            }
            CHECK(e.buf->nlines >= nl0 + 1 && line_is(e.buf, t[i].at, t[i].line));
            CHECK(strcmp(t[i].after, "") == 0 || line_is(e.buf, t[i].at - 1, t[i].after));
            if (strcmp(t[i].file, "e.c") == 0)
                CHECK(line_is(e.buf, 1, "") && line_is(e.buf, 2, "#include \"mine.h\""));
            if (strcmp(t[i].file, "d.c") == 0)
                CHECK(line_is(e.buf, 1, "") && line_is(e.buf, 3, ""));
            /* the cursor stays on its line, which moved down, and one Undo takes back all */
            CHECK(e.cy == t[i].y + (e.buf->nlines - nl0));
            edit_undo(&e);
            CHECK(e.buf->nlines == nl0 && line_is(e.buf, 0, strncmp(t[i].text, "#include <stdio", 14) == 0
                                                           ? "#include <stdio.h>"
                                                           : strncmp(t[i].text, "#include <iostream>", 19) == 0
                                                           ? "#include <iostream>"
                                                           : strncmp(t[i].text, "#include <vector>", 17) == 0
                                                           ? "#include <vector>"
                                                           : strncmp(t[i].text, "#include <memory>", 17) == 0
                                                           ? "#include <memory>"
                                                           : strncmp(t[i].text, "#include <stdlib", 15) == 0
                                                           ? "#include <stdlib.h>"
                                                           : strncmp(t[i].text, "/*", 2) == 0 ? "/* a program */"
                                                           : strncmp(t[i].text, "#ifndef", 7) == 0 ? "#ifndef X"
                                                           : "#include \"mine.h\""));
        } else {
            CHECK(e.buf->nlines == nl0);        /* nothing was added */
        }
        editor_free(&e);
    }
    rm_rf(dir);
}

/* Accepting a symbol of Python's standard library adds the import it needs. */
static void test_py_import(void)
{
    char dir[64], path[128], err[128];
    Editor e;
    Doc *d;
    size_t i;
    struct { const char *text, *typed, *head; size_t y, x; int changes; } t[] = {
        { "import sys\n\ndef f():\n    \n", "pathli", "import pathlib\nimport sys\n\ndef f():\n", 3, 4, 1 },
        { "import os\nimport sys\n\ndef f():\n    \n", "Ordered",
          "import os\nimport sys\nfrom collections import OrderedDict\n\ndef f():\n", 4, 4, 1 },
        { "from collections import deque\n\ndef f():\n    \n", "Ordered",
          "from collections import deque, OrderedDict\n\ndef f():\n", 3, 4, 1 },
        { "from collections import OrderedDict\n\ndef f():\n    \n", "Ordered",
          "from collections import OrderedDict\n\ndef f():\n", 3, 4, 0 },
        { "def f():\n    \n", "os.getc", "import os\n\ndef f():\n", 1, 4, 1 },
        { "\"\"\"Doc.\"\"\"\n\ndef f():\n    \n", "pathli", "\"\"\"Doc.\"\"\"\n\nimport pathlib\n\ndef f():\n", 3, 4, 1 },
        { "\"\"\"Doc\nmore.\n\"\"\"\ndef f():\n    \n", "pathli",
          "\"\"\"Doc\nmore.\n\"\"\"\n\nimport pathlib\n\ndef f():\n", 4, 4, 1 },
        { "from __future__ import annotations\n\ndef f():\n    \n", "pathli",
          "from __future__ import annotations\nimport pathlib\n\ndef f():\n", 3, 4, 1 },
        { "import os, pathlib\n\ndef f():\n    \n", "pathli", "import os, pathlib\n\ndef f():\n", 3, 4, 0 },
        { "from pathlib import Path\n\ndef f():\n    \n", "Pat", "from pathlib import Path\n\ndef f():\n", 3, 4, 0 },
        { "def f():\n    \n", "ValueEr", "def f():\n", 1, 4, 0 },
        { "pathlib = 1\ndef f():\n    \n", "pathlib.", "pathlib = 1\ndef f():\n", 2, 4, 0 },
        { "os = 1\ndef f():\n    \n", "os.getc", "os = 1\ndef f():\n", 2, 4, 0 },
    };

    CHECK(make_dir(dir, sizeof dir) != NULL);
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        const char *q;
        size_t y, nl0;
        SBuf all = SBUF_INIT;

        put_file(dir, "t.py", t[i].text);
        snprintf(path, sizeof path, "%s/t.py", dir);
        editor_init(&e);
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d != NULL);
        editor_activate(&e, d);
        nl0 = e.buf->nlines;
        e.cy = t[i].y;
        e.cx = t[i].x;
        for (q = t[i].typed; *q; q++)
            kkey(&e, (uint32_t)(unsigned char)*q, 0);
        if (e.comp.on)
            kkey(&e, KEY_ENTER, 0);
        for (y = 0; y < e.buf->nlines; y++) {
            sbuf_add(&all, e.buf->lines[y].s, e.buf->lines[y].len);
            sbuf_adds(&all, "\n");
        }
        sbuf_add(&all, "", 1);
        if (strncmp(all.p, t[i].head, strlen(t[i].head)) != 0)
            fprintf(stderr, "case %zu:\n%s---\n", i, all.p);
        CHECK(strncmp(all.p, t[i].head, strlen(t[i].head)) == 0);
        if (t[i].changes) {
            /* one Undo takes back the import and the word */
            edit_undo(&e);
            CHECK(e.buf->nlines == nl0);
        }
        sbuf_free(&all);
        editor_free(&e);
    }
    rm_rf(dir);
}

/* Accepting a Node module or one of its classes adds the import or require it needs. */
static void test_js_import(void)
{
    char dir[64], path[128], err[128];
    Editor e;
    Doc *d;
    size_t i;
    struct { const char *file, *text, *typed, *head; size_t y, x; int changes; } t[] = {
        { "a.js", "const path = require('path');\n\nfunction f() {\n    \n}\n", "fs.readF",
          "const fs = require('fs');\nconst path = require('path');\n\nfunction f() {\n", 3, 4, 1 },
        { "b.ts", "import path from 'path';\n\nfunction f() {\n    \n}\n", "fs.readF",
          "import fs from 'fs';\nimport path from 'path';\n\nfunction f() {\n", 3, 4, 1 },
        { "c.ts", "import path from \"path\"\n\nfunction f() {\n    \n}\n", "fs.readF",
          "import fs from \"fs\"\nimport path from \"path\"\n\nfunction f() {\n", 3, 4, 1 },
        { "d.ts", "import fs from 'fs';\n\nfunction f() {\n    \n}\n", "fs.readF",
          "import fs from 'fs';\n\nfunction f() {\n", 3, 4, 0 },
        { "e.ts", "import fs from 'fs';\n\nfunction f() {\n    \n}\n", "EventEm",
          "import { EventEmitter } from 'events';\nimport fs from 'fs';\n\nfunction f() {\n", 3, 4, 1 },
        { "f.ts", "import { Readable } from 'stream';\n\nfunction f() {\n    \n}\n", "Writab",
          "import { Readable, Writable } from 'stream';\n\nfunction f() {\n", 3, 4, 1 },
        { "g.js", "'use strict';\n\nfunction f() {\n    \n}\n", "fs.readF",
          "'use strict';\n\nconst fs = require('fs');\n\nfunction f() {\n", 3, 4, 1 },
        { "h.js", "const fs = require('fs');\n\nfunction f() {\n    \n}\n", "fs.readF",
          "const fs = require('fs');\n\nfunction f() {\n", 3, 4, 0 },
        { "i.js", "let fs = 3;\nfunction f() {\n    \n}\n", "fs.readF", "let fs = 3;\nfunction f() {\n", 2, 4, 0 },
        { "j.ts", "import * as path from 'path';\n\nfunction f() {\n    \n}\n", "fs.readF",
          "import * as fs from 'fs';\nimport * as path from 'path';\n\nfunction f() {\n", 3, 4, 1 },
        { "k.ts", "function f() {\n    \n}\n", "fs.readF", "import fs from 'fs';\n\nfunction f() {\n", 1, 4, 1 },
        { "l.ts", "function f() {\n    \n}\n", "Buff", "function f() {\n", 1, 4, 0 },
        { "m.ts", "import {\n  a,\n  fs,\n} from './x';\n\nfunction f() {\n    \n}\n", "fs.readF",
          "import {\n  a,\n  fs,\n} from './x';\n\nfunction f() {\n", 6, 4, 0 },
        { "n.ts", "// header\n/* more\n   text */\nfunction f() {\n    \n}\n", "fs.readF",
          "// header\n/* more\n   text */\n\nimport fs from 'fs';\n\nfunction f() {\n", 4, 4, 1 },
    };

    CHECK(make_dir(dir, sizeof dir) != NULL);
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        const char *q;
        size_t y, nl0;
        SBuf all = SBUF_INIT;

        put_file(dir, t[i].file, t[i].text);
        snprintf(path, sizeof path, "%s/%s", dir, t[i].file);
        editor_init(&e);
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d != NULL);
        editor_activate(&e, d);
        nl0 = e.buf->nlines;
        e.cy = t[i].y;
        e.cx = t[i].x;
        for (q = t[i].typed; *q; q++)
            kkey(&e, (uint32_t)(unsigned char)*q, 0);
        if (e.comp.on)
            kkey(&e, KEY_ENTER, 0);
        for (y = 0; y < e.buf->nlines; y++) {
            sbuf_add(&all, e.buf->lines[y].s, e.buf->lines[y].len);
            sbuf_adds(&all, "\n");
        }
        sbuf_add(&all, "", 1);
        if (strncmp(all.p, t[i].head, strlen(t[i].head)) != 0)
            fprintf(stderr, "case %zu (%s):\n%s---\n", i, t[i].file, all.p);
        CHECK(strncmp(all.p, t[i].head, strlen(t[i].head)) == 0);
        if (t[i].changes) {
            edit_undo(&e);                      /* one Undo takes back the import and the word */
            CHECK(e.buf->nlines == nl0);
        }
        sbuf_free(&all);
        editor_free(&e);
    }
    /* package.json with "type": "module": a .js file without imports uses import */
    put_file(dir, "package.json", "{ \"name\": \"x\", \"type\": \"module\" }\n");
    put_file(dir, "z.js", "function f() {\n    \n}\n");
    snprintf(path, sizeof path, "%s/z.js", dir);
    editor_init(&e);
    d = editor_open_doc(&e, path, err, sizeof err);
    CHECK(d != NULL);
    editor_activate(&e, d);
    e.cy = 1;
    e.cx = 4;
    {
        const char *q;

        for (q = "fs.readF"; *q; q++)
            kkey(&e, (uint32_t)(unsigned char)*q, 0);
    }
    if (e.comp.on)
        kkey(&e, KEY_ENTER, 0);
    CHECK(line_is(e.buf, 0, "import fs from 'fs';") && line_is(e.buf, 1, "") && line_is(e.buf, 2, "function f() {"));
    editor_free(&e);
    rm_rf(dir);
}

/* A function of a header of the project that the file does not include yet: suggested, and
 * accepting it adds #include "header.h" written from where the file is. */
static void test_c_project_include(void)
{
    char dir[64], path[200], err[128], sub[200];
    Editor e;
    Doc *d;
    size_t i, k;
    struct { const char *file, *text, *typed, *head; size_t y, x; int changes; } t[] = {
        { "src/a.c", "#include <stdio.h>\n\nint main(void)\n{\n    \n}\n", "buf_ins",
          "#include <stdio.h>\n\n#include \"buf.h\"\n\nint main(void)\n", 4, 4, 1 },
        { "tests/t.c", "int main(void)\n{\n    \n}\n", "buf_ins",
          "#include \"../src/buf.h\"\n\nint main(void)\n", 2, 4, 1 },
        { "src/b.c", "#include \"buf.h\"\n\nint main(void)\n{\n    \n}\n", "buf_ins",
          "#include \"buf.h\"\n\nint main(void)\n", 4, 4, 0 },
        { "src/c.c", "#include \"aaa.h\"\n#include \"zzz.h\"\n\nint main(void)\n{\n    \n}\n", "buf_ins",
          "#include \"aaa.h\"\n#include \"buf.h\"\n#include \"zzz.h\"\n", 5, 4, 1 },
        { "src/d.c", "/* d */\n\nint main(void)\n{\n    \n}\n", "Buf_t",
          "/* d */\n\n#include \"buf.h\"\n\nint main(void)\n", 4, 4, 1 },
    };

    CHECK(make_dir(dir, sizeof dir) != NULL);
    snprintf(sub, sizeof sub, "%s/.git", dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/src", dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/tests", dir);
    mkdir(sub, 0755);
    put_file(dir, "src/buf.h", "#ifndef BUF_H\n#define BUF_H\n\ntypedef struct { int n; } Buf_t;\n"
                               "int buf_insert(int where, const char *text);\n#define BUF_MAX 64\n#endif\n");
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        const char *q;
        size_t y, nl0;
        SBuf all = SBUF_INIT;

        put_file(dir, t[i].file, t[i].text);
        snprintf(path, sizeof path, "%s/%s", dir, t[i].file);
        editor_init(&e);
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d != NULL);
        editor_activate(&e, d);
        nl0 = e.buf->nlines;
        e.cy = t[i].y;
        e.cx = t[i].x;
        for (q = t[i].typed; *q; q++)
            kkey(&e, (uint32_t)(unsigned char)*q, 0);
        /* the guard of the header is not a symbol to suggest */
        for (k = 0; k < e.comp.n; k++)
            CHECK(strcmp(e.comp.v[k].text, "BUF_H") != 0 || !e.comp.v[k].inc);
        if (e.comp.on)
            kkey(&e, KEY_ENTER, 0);
        for (y = 0; y < e.buf->nlines; y++) {
            sbuf_add(&all, e.buf->lines[y].s, e.buf->lines[y].len);
            sbuf_adds(&all, "\n");
        }
        sbuf_add(&all, "", 1);
        if (strncmp(all.p, t[i].head, strlen(t[i].head)) != 0)
            fprintf(stderr, "case %zu (%s):\n%s---\n", i, t[i].file, all.p);
        CHECK(strncmp(all.p, t[i].head, strlen(t[i].head)) == 0);
        if (t[i].changes) {
            edit_undo(&e);
            CHECK(e.buf->nlines == nl0);
        }
        sbuf_free(&all);
        editor_free(&e);
    }
    rm_rf(dir);
}

/* What another module of the project exports: suggested, and accepting it adds its import. */
static void test_js_project_import(void)
{
    char dir[64], path[200], err[128], sub[200];
    Editor e;
    Doc *d;
    size_t i;
    struct { const char *file, *text, *typed, *head; size_t y, x; int changes; } t[] = {
        { "src/app.ts", "import fs from 'fs';\n\nfunction f() {\n    \n}\n", "formatD",
          "import fs from 'fs';\nimport { formatDate } from './util';\n\nfunction f() {\n", 3, 4, 1 },
        { "src/app2.ts", "function f() {\n    \n}\n", "Datast",
          "import Datastore from './util';\n\nfunction f() {\n", 1, 4, 1 },
        { "src/deep/x.ts", "function f() {\n    \n}\n", "formatD",
          "import { formatDate } from '../util';\n\nfunction f() {\n", 1, 4, 1 },
        { "src/app3.ts", "import { MAX_ITEMS } from './util';\n\nfunction f() {\n    \n}\n", "formatD",
          "import { MAX_ITEMS, formatDate } from './util';\n\nfunction f() {\n", 3, 4, 1 },
        { "src/app4.ts", "function f() {\n    \n}\n", "libHel",
          "import { libHelper } from './lib';\n\nfunction f() {\n", 1, 4, 1 },
        { "src/app5.ts", "import { formatDate } from './util';\n\nfunction f() {\n    \n}\n", "formatD",
          "import { formatDate } from './util';\n\nfunction f() {\n", 3, 4, 0 },
        { "cjs/main.js", "const path = require('path');\n\nfunction f() {\n    \n}\n", "slugi",
          "const path = require('path');\nconst { slugify } = require('./helpers');\n\nfunction f() {\n", 3, 4, 1 },
        { "cjs/main2.js", "const path = require('path');\n\nfunction f() {\n    \n}\n", "camelC",
          "const path = require('path');\nconst { camelCase } = require('./helpers');\n\nfunction f() {\n", 3, 4, 1 },
    };

    CHECK(make_dir(dir, sizeof dir) != NULL);
    snprintf(sub, sizeof sub, "%s/.git", dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/src", dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/src/deep", dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/src/lib", dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/cjs", dir);
    mkdir(sub, 0755);
    put_file(dir, "src/util.ts", "export function formatDate(d: Date, fmt: string): string {\n  return '';\n}\n"
                                 "export const MAX_ITEMS = 10;\nexport default class Datastore {\n}\n"
                                 "export interface Item {\n}\nfunction hidden() {}\n");
    put_file(dir, "src/lib/index.ts", "export function libHelper(x) {\n}\n");
    put_file(dir, "cjs/helpers.js", "function slugify(s) {}\nfunction trimAll(s) {}\n"
                                    "module.exports = { slugify, trimAll };\nexports.camelCase = (s) => s;\n");
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        const char *q;
        size_t y, nl0, k;
        SBuf all = SBUF_INIT;

        put_file(dir, t[i].file, t[i].text);
        snprintf(path, sizeof path, "%s/%s", dir, t[i].file);
        editor_init(&e);
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d != NULL);
        editor_activate(&e, d);
        nl0 = e.buf->nlines;
        e.cy = t[i].y;
        e.cx = t[i].x;
        for (q = t[i].typed; *q; q++)
            kkey(&e, (uint32_t)(unsigned char)*q, 0);
        /* what a module does not export is not suggested from it */
        for (k = 0; k < e.comp.n; k++)
            CHECK(strcmp(e.comp.v[k].text, "hidden") != 0 || !e.comp.v[k].inc);
        if (e.comp.on)
            kkey(&e, KEY_ENTER, 0);
        for (y = 0; y < e.buf->nlines; y++) {
            sbuf_add(&all, e.buf->lines[y].s, e.buf->lines[y].len);
            sbuf_adds(&all, "\n");
        }
        sbuf_add(&all, "", 1);
        if (strncmp(all.p, t[i].head, strlen(t[i].head)) != 0)
            fprintf(stderr, "case %zu (%s):\n%s---\n", i, t[i].file, all.p);
        CHECK(strncmp(all.p, t[i].head, strlen(t[i].head)) == 0);
        if (t[i].changes) {
            edit_undo(&e);
            CHECK(e.buf->nlines == nl0);
        }
        sbuf_free(&all);
        editor_free(&e);
    }
    rm_rf(dir);
}

/* What another module of a Python project defines: suggested, and accepting it adds
 * "from package.module import name" into the last group of imports. */
static void test_py_project_import(void)
{
    char dir[64], path[200], err[128], sub[200];
    Editor e;
    Doc *d;
    size_t i;
    struct { const char *file, *text, *typed, *head; size_t y, x; int changes; } t[] = {
        { "myapp/views.py", "import os\nimport sys\n\nfrom django.db import models\n\ndef f():\n    \n", "slugi",
          "import os\nimport sys\n\nfrom django.db import models\nfrom myapp.utils import slugify\n\ndef f():\n", 6, 4, 1 },
        { "main.py", "def main():\n    \n", "Confi", "from myapp.utils import Config\n\ndef main():\n", 1, 4, 1 },
        { "main2.py", "from myapp.utils import slugify\n\ndef f():\n    \n", "MAX_SI",
          "from myapp.utils import slugify, MAX_SIZE\n\ndef f():\n", 3, 4, 1 },
        { "scripts/run.py", "def f():\n    \n", "run_to", "from tool_helpers import run_tool\n\ndef f():\n", 1, 4, 1 },
        { "main3.py", "from myapp.utils import slugify\n\ndef f():\n    \n", "slugi",
          "from myapp.utils import slugify\n\ndef f():\n", 3, 4, 0 },
        { "main4.py", "def f():\n    \n", "pack_he", "from myapp import pack_helper\n\ndef f():\n", 1, 4, 1 },
    };

    CHECK(make_dir(dir, sizeof dir) != NULL);
    snprintf(sub, sizeof sub, "%s/.git", dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/myapp", dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/scripts", dir);
    mkdir(sub, 0755);
    snprintf(sub, sizeof sub, "%s/venv", dir);
    mkdir(sub, 0755);
    put_file(dir, "myapp/__init__.py", "def pack_helper():\n    pass\n");
    put_file(dir, "myapp/utils.py", "def slugify(text, sep='-'):\n    pass\n\nclass Config:\n    pass\n\n"
                                    "MAX_SIZE = 10\n_private = 1\ndef _hidden():\n    pass\n");
    put_file(dir, "scripts/tool_helpers.py", "def run_tool(x):\n    pass\n");
    put_file(dir, "venv/lib.py", "def slugify_venv(x):\n    pass\n");
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        const char *q;
        size_t y, nl0, k;
        SBuf all = SBUF_INIT;

        put_file(dir, t[i].file, t[i].text);
        snprintf(path, sizeof path, "%s/%s", dir, t[i].file);
        editor_init(&e);
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d != NULL);
        editor_activate(&e, d);
        nl0 = e.buf->nlines;
        e.cy = t[i].y;
        e.cx = t[i].x;
        for (q = t[i].typed; *q; q++)
            kkey(&e, (uint32_t)(unsigned char)*q, 0);
        /* private names and virtual environments are left out */
        for (k = 0; k < e.comp.n; k++)
            CHECK((strcmp(e.comp.v[k].text, "_hidden") != 0 && strcmp(e.comp.v[k].text, "slugify_venv") != 0) ||
                  !e.comp.v[k].inc);
        if (e.comp.on)
            kkey(&e, KEY_ENTER, 0);
        for (y = 0; y < e.buf->nlines; y++) {
            sbuf_add(&all, e.buf->lines[y].s, e.buf->lines[y].len);
            sbuf_adds(&all, "\n");
        }
        sbuf_add(&all, "", 1);
        if (strncmp(all.p, t[i].head, strlen(t[i].head)) != 0)
            fprintf(stderr, "case %zu (%s):\n%s---\n", i, t[i].file, all.p);
        CHECK(strncmp(all.p, t[i].head, strlen(t[i].head)) == 0);
        if (t[i].changes) {
            edit_undo(&e);
            CHECK(e.buf->nlines == nl0);
        }
        sbuf_free(&all);
        editor_free(&e);
    }
    rm_rf(dir);
}

/* PHP: a member chosen after Name:: imports the class Name (an alias of Laravel, or a class of the
 * project) in a file that has a namespace. */
static void test_php_static_use(void)
{
    char dir[64], path[256], err[128];
    Editor e;
    Doc *d;
    size_t i;
    static const char *const sub[] = { "config", "vendor", "vendor/composer", "vendor/lv", "vendor/lv/src",
                                       "vendor/lv/src/Support", "vendor/lv/src/Support/Facades", "app",
                                       "app/Models", "app/Http" };
    struct { const char *text, *typed, *want; size_t y; int changes; } t[] = {
        { "<?php\n\nnamespace App\\Http;\n\nuse Illuminate\\Http\\Request;\n\nclass C\n{\n    function f()\n    {\n        Route::\n    }\n}\n",
          "ge", "use Illuminate\\Http\\Request;\nuse Lv\\Support\\Facades\\Route;\n", 10, 1 },
        { "<?php\n\nnamespace App\\Http;\n\nclass C\n{\n    function f()\n    {\n        User::\n    }\n}\n",
          "fin", "namespace App\\Http;\n\nuse App\\Models\\User;\n\nclass C\n", 8, 1 },
        { "<?php\n\nnamespace App\\Http;\n\nuse Lv\\Support\\Facades\\Route;\n\nclass C\n{\n    function f()\n    {\n        Route::\n    }\n}\n",
          "ge", "use Lv\\Support\\Facades\\Route;\n\nclass C\n", 10, 0 },
        { "<?php\n\nnamespace App\\Models;\n\nclass D\n{\n    function f()\n    {\n        User::\n    }\n}\n",
          "fin", "namespace App\\Models;\n\nclass D\n", 8, 0 },
    };

    CHECK(make_dir(dir, sizeof dir) != NULL);
    for (i = 0; i < sizeof sub / sizeof sub[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, sub[i]);
        mkdir(path, 0700);
    }
    put_file(dir, "composer.json", "{}\n");
    put_file(dir, "config/app.php",
             "<?php\nreturn [\n    'aliases' => [\n        'Route' => Lv\\Support\\Facades\\Route::class,\n    ],\n];\n");
    put_file(dir, "vendor/composer/autoload_psr4.php",
             "<?php\n$vendorDir = dirname(dirname(__FILE__));\n$baseDir = dirname($vendorDir);\nreturn array(\n"
             "    'Lv\\\\' => array($vendorDir . '/lv/src'),\n    'App\\\\' => array($baseDir . '/app'),\n);\n");
    put_file(dir, "vendor/lv/src/Support/Facades/Route.php",
             "<?php\nnamespace Lv\\Support\\Facades;\n/**\n * @method static void get(string $uri, $action)\n */\n"
             "class Route extends Facade {}\n");
    put_file(dir, "app/Models/User.php",
             "<?php\nnamespace App\\Models;\nclass User\n{\n    public static function find($id) {}\n}\n");
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        const char *q;
        size_t y, nl0;
        SBuf all = SBUF_INIT;

        put_file(dir, i == 3 ? "app/Models/D.php" : "app/Http/C.php", t[i].text);
        snprintf(path, sizeof path, "%s/%s", dir, i == 3 ? "app/Models/D.php" : "app/Http/C.php");
        editor_init(&e);
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d != NULL);
        editor_activate(&e, d);
        nl0 = e.buf->nlines;
        e.cy = t[i].y;
        e.cx = e.buf->lines[e.cy].len;
        for (q = t[i].typed; *q; q++)
            kkey(&e, (uint32_t)(unsigned char)*q, 0);
        CHECK(e.comp.on);
        if (e.comp.on)
            kkey(&e, KEY_ENTER, 0);
        for (y = 0; y < e.buf->nlines; y++) {
            sbuf_add(&all, e.buf->lines[y].s, e.buf->lines[y].len);
            sbuf_adds(&all, "\n");
        }
        sbuf_add(&all, "", 1);
        if (!strstr(all.p, t[i].want) || (t[i].changes ? e.buf->nlines == nl0 : e.buf->nlines != nl0))
            fprintf(stderr, "case %zu:\n%s---\n", i, all.p);
        CHECK(strstr(all.p, t[i].want) != NULL);
        CHECK(t[i].changes ? e.buf->nlines > nl0 : e.buf->nlines == nl0);
        if (t[i].changes) {
            edit_undo(&e);
            CHECK(e.buf->nlines == nl0);
        }
        sbuf_free(&all);
        editor_free(&e);
    }
    rm_rf(dir);
}

/* PHP without Composer: a function or class of another file of the project gets its require_once;
 * with Composer: a function or constant in a namespace of the "files" it loads gets its use. */
static void php_case(const char *dir, const char *file, const char *text, const char *typed, size_t cy,
                     const char *want, int changes)
{
    char path[256], err[128];
    Editor e;
    Doc *d;
    const char *q;
    size_t y, nl0;
    SBuf all = SBUF_INIT;

    put_file(dir, file, text);
    snprintf(path, sizeof path, "%s/%s", dir, file);
    editor_init(&e);
    d = editor_open_doc(&e, path, err, sizeof err);
    CHECK(d != NULL);
    editor_activate(&e, d);
    nl0 = e.buf->nlines;
    e.cy = cy;
    e.cx = e.buf->lines[e.cy].len;
    for (q = typed; *q; q++)
        kkey(&e, (uint32_t)(unsigned char)*q, 0);
    if (e.comp.on)
        kkey(&e, KEY_ENTER, 0);
    for (y = 0; y < e.buf->nlines; y++) {
        sbuf_add(&all, e.buf->lines[y].s, e.buf->lines[y].len);
        sbuf_adds(&all, "\n");
    }
    sbuf_add(&all, "", 1);
    /* changes: 1 lines were added, 2 a line was changed (the name joined an import), 0 nothing */
    if (!strstr(all.p, want) || (changes == 1 ? e.buf->nlines == nl0 : e.buf->nlines != nl0))
        fprintf(stderr, "%s (%s):\n%s---\n", file, typed, all.p);
    CHECK(strstr(all.p, want) != NULL);
    CHECK(changes == 1 ? e.buf->nlines > nl0 : e.buf->nlines == nl0);
    if (changes == 2) {
        edit_undo(&e);
        CHECK(e.buf->lines[0].len < strlen(want) || memcmp(e.buf->lines[0].s, want, strlen(want)) != 0);
    } else if (changes) {
        edit_undo(&e);
        CHECK(e.buf->nlines == nl0);
    }
    sbuf_free(&all);
    editor_free(&e);
}

static void test_php_require_and_use_function(void)
{
    char dir[64], dir2[64], sub[200];
    static const char *const s1[] = { ".git", "lib", "pages", "ns" };
    static const char *const s2[] = { ".git", "app", "app/Http", "app/Support" };
    size_t i;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    for (i = 0; i < sizeof s1 / sizeof s1[0]; i++) {
        snprintf(sub, sizeof sub, "%s/%s", dir, s1[i]);
        mkdir(sub, 0755);
    }
    put_file(dir, "lib/utils.php", "<?php\nfunction format_money($v, $cur = 'EUR') {\n}\nclass Mailer {\n}\n"
                                   "define('APP_NAME', 'x');\n");
    put_file(dir, "ns/Thing.php", "<?php\nnamespace Foo;\nfunction thing_fn() {}\n");
    php_case(dir, "index.php", "<?php\n\nfunction main() {\n    \n}\n", "format_mo", 3,
             "<?php\n\nrequire_once __DIR__ . '/lib/utils.php';\n\nfunction main() {\n", 1);
    php_case(dir, "pages/a.php", "<?php\nrequire_once __DIR__ . '/../lib/db.php';\n\nfunction f() {\n    \n}\n",
             "Maile", 4,
             "require_once __DIR__ . '/../lib/db.php';\nrequire_once __DIR__ . '/../lib/utils.php';\n\nfunction", 1);
    php_case(dir, "b.php", "<?php\nrequire_once 'lib/utils.php';\n\nfunction f() {\n    \n}\n", "format_mo", 4,
             "<?php\nrequire_once 'lib/utils.php';\n\nfunction f() {\n", 0);
    php_case(dir, "c.php", "<?php\n\nfunction f() {\n    \n}\n", "thing_f", 3, "<?php\n\nfunction f() {\n", 0);
    rm_rf(dir);

    CHECK(make_dir(dir2, sizeof dir2) != NULL);
    for (i = 0; i < sizeof s2 / sizeof s2[0]; i++) {
        snprintf(sub, sizeof sub, "%s/%s", dir2, s2[i]);
        mkdir(sub, 0755);
    }
    put_file(dir2, "composer.json", "{\n  \"autoload\": {\n    \"psr-4\": { \"App\\\\\": \"app/\" },\n"
                                    "    \"files\": [\"app/helpers.php\", \"app/Support/money.php\"]\n  }\n}\n");
    put_file(dir2, "app/helpers.php", "<?php\nfunction app_helper($x) {}\n");
    put_file(dir2, "app/Support/money.php", "<?php\nnamespace App\\Support;\nfunction money_fmt($v) {}\n"
                                            "const CURRENCY = 'EUR';\n");
    {
        static const char *ctl = "<?php\n\nnamespace App\\Http;\n\nuse Illuminate\\Http\\Request;\n\nclass C\n{\n"
                                 "    function f()\n    {\n        \n    }\n}\n";

        php_case(dir2, "app/Http/C.php", ctl, "money_fm", 10,
                 "use Illuminate\\Http\\Request;\n\nuse function App\\Support\\money_fmt;\n\nclass C\n", 1);
        php_case(dir2, "app/Http/C.php", ctl, "CURREN", 10,
                 "use Illuminate\\Http\\Request;\n\nuse const App\\Support\\CURRENCY;\n\nclass C\n", 1);
        php_case(dir2, "app/Http/C.php", ctl, "app_hel", 10, "use Illuminate\\Http\\Request;\n\nclass C\n", 0);
        php_case(dir2, "app/Http/C.php",
                 "<?php\n\nnamespace App\\Http;\n\nuse Illuminate\\Http\\Request;\n\nuse function App\\Support\\money_fmt;\n\n"
                 "class C\n{\n    function f()\n    {\n        \n    }\n}\n",
                 "CURREN", 12, "use function App\\Support\\money_fmt;\n\nuse const App\\Support\\CURRENCY;\n\nclass C\n", 1);
        php_case(dir2, "app/Support/other.php", "<?php\n\nnamespace App\\Support;\n\nfunction g()\n{\n    \n}\n",
                 "money_fm", 6, "namespace App\\Support;\n\nfunction g()\n", 0);
    }
    rm_rf(dir2);
}

/* Facades with no @see (October): the class comes from "Resolves to:" (even with the wrong
 * namespace) or from getFacadeAccessor; the aliases are in a file config/app.php includes; a
 * module with its own composer.json is still in the project; a base class named with "as". */
static void test_php_facades(void)
{
    char dir[64], sub[200];
    static const char *const dirs[] = {
        "config", "modules", "modules/backend", "vendor", "vendor/composer", "vendor/lib",
        "vendor/lib/src", "vendor/lib/src/Facades", "vendor/lib/src/Flash", "vendor/lib/src/Support",
        "vendor/base", "vendor/base/src"
    };
    size_t i;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(sub, sizeof sub, "%s/%s", dir, dirs[i]);
        mkdir(sub, 0755);
    }
    put_file(dir, "vendor/composer/autoload_psr4.php",
             "<?php\n$vendorDir = dirname(dirname(__FILE__));\n$baseDir = dirname($vendorDir);\n"
             "return array(\n    'Lib\\\\' => array($vendorDir . '/lib/src'),\n"
             "    'Base\\\\' => array($vendorDir . '/base/src'),\n);\n");
    put_file(dir, "config/app.php", "<?php\nreturn [\n    'aliases' => array_merge(include(base_path('modules/aliases.php')), [\n"
                                    "        'Old' => 'Lib\\Facades\\Old',\n    ]),\n];\n");
    put_file(dir, "modules/aliases.php", "<?php\nreturn [\n    'Flash' => Lib\\Facades\\Flash::class,\n"
                                         "    'Str' => Lib\\Facades\\Str::class,\n];\n");
    put_file(dir, "modules/backend/composer.json", "{}\n");
    put_file(dir, "vendor/lib/src/Facades/Flash.php",
             "<?php namespace Lib\\Facades;\n\nclass Flash extends Facade\n{\n    /**\n     * Resolves to:\n"
             "     * - Lib\\Support\\FlashBag\n     */\n    protected static function getFacadeAccessor() { return 'flash'; }\n}\n");
    put_file(dir, "vendor/lib/src/Flash/FlashBag.php",
             "<?php namespace Lib\\Flash;\n\nclass FlashBag\n{\n    public function success($m) {}\n}\n");
    put_file(dir, "vendor/lib/src/Facades/Str.php",
             "<?php namespace Lib\\Facades;\n\nclass Str extends Facade\n{\n    /**\n     * Resolves to:\n"
             "     * - Lib\\Support\\Str\n     */\n    protected static function getFacadeAccessor() { return 'string'; }\n}\n");
    put_file(dir, "vendor/lib/src/Support/Str.php",
             "<?php namespace Lib\\Support;\n\nuse Base\\Str as StrHelper;\n\nclass Str extends StrHelper\n{\n"
             "    public static function ordinal($n) {}\n}\n");
    put_file(dir, "vendor/base/src/Str.php", "<?php namespace Base;\n\nclass Str\n{\n    public static function camel($v) {}\n}\n");
    put_file(dir, "vendor/lib/src/Facades/Old.php",
             "<?php namespace Lib\\Facades;\n\nuse Lib\\Flash\\FlashBag;\n\nclass Old extends Facade\n{\n"
             "    protected static function getFacadeAccessor()\n    {\n        return FlashBag::class;\n    }\n}\n");
    {
        static const char *ctl = "<?php namespace Backend\\Classes;\n\nuse Flash;\nuse Str;\nuse Old;\n\nclass C\n{\n"
                                 "    function f()\n    {\n        \n    }\n}\n";

        php_case(dir, "modules/backend/C.php", ctl, "Flash::succ", 10, "        Flash::success(", 0);
        php_case(dir, "modules/backend/C.php", ctl, "Str::ordi", 10, "        Str::ordinal(", 0);
        php_case(dir, "modules/backend/C.php", ctl, "Str::came", 10, "        Str::camel(", 0);
        php_case(dir, "modules/backend/C.php", ctl, "Old::succ", 10, "        Old::success(", 0);
    }
    rm_rf(dir);
}

/* Many classes reached from one file: the index cache must not drop one that is in use. */
static void test_sym_cache_pressure(void)
{
    char dir[64], path[256], err[128], name[64], body[160];
    Doc d;
    SymCache *c = sym_cache_new();
    Doc *docs[1];
    static Got g;
    SBuf main = SBUF_INIT;
    int i, round;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    snprintf(path, sizeof path, "%s/app", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/vendor", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/vendor/composer", dir);
    mkdir(path, 0700);
    put_file(dir, "composer.json", "{}\n");
    put_file(dir, "vendor/composer/autoload_psr4.php",
             "<?php\n$vendorDir = dirname(dirname(__FILE__));\n$baseDir = dirname($vendorDir);\n"
             "return array(\n    'App\\\\' => array($baseDir . '/app'),\n);\n");
    sbuf_adds(&main, "<?php\nnamespace App;\n");
    for (i = 0; i < 70; i++) {
        snprintf(name, sizeof name, "app/K%d.php", i);
        snprintf(body, sizeof body, "<?php\nnamespace App;\nclass K%d\n{\n    public static function m%d() {}\n}\n", i, i);
        put_file(dir, name, body);
        snprintf(body, sizeof body, "use App\\K%d;\n", i);
        sbuf_adds(&main, body);
    }
    for (i = 0; i < 70; i++) {
        snprintf(body, sizeof body, "K%d::m%d();\n", i, i);
        sbuf_adds(&main, body);
    }
    sbuf_add(&main, "", 1);
    put_file(dir, "main.php", main.p);
    sbuf_free(&main);
    memset(&d, 0, sizeof d);
    snprintf(path, sizeof path, "%s/main.php", dir);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    docs[0] = &d;
    for (round = 0; round < 4; round++) {
        memset(&g, 0, sizeof g);
        sym_globals(c, docs, 1, &d, got_cb, &g);
        /* a class whose file is not there any more is the case of an index read long ago */
        if (round == 1) {
            snprintf(path, sizeof path, "%s/app/K3.php", dir);
            unlink(path);
        }
    }
    CHECK(has_sym(&g, "K0"));
    sym_cache_free(c);
    doc_release(&d);
    rm_rf(dir);
}

/* Go and Rust: the index (methods of a type written outside it, impl blocks), the standard
 * library after a package or a module, and the import or use it needs. */
static void test_go_rust(void)
{
    char dir[64], path[128], err[128];
    Doc d;
    SymCache *c = sym_cache_new();
    Doc *docs[1];
    static Got g;
    size_t y, sep;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    put_file(dir, "s.go", "package main\n\ntype Server struct {\n\tPort int\n\tName string\n}\n\n"
                          "func (s *Server) Start(addr string) error {\n\treturn nil\n}\n\n"
                          "func NewServer() *Server {\n\treturn &Server{}\n}\n\nfunc main() {\n"
                          "\tsrv := &Server{}\n\tsrv.\n\tfmt.\n}\n");
    snprintf(path, sizeof path, "%s/s.go", dir);
    memset(&d, 0, sizeof d);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    out_text[0] = '\0';
    sym_outline(c, &d, out_cb, NULL);
    CHECK(strstr(out_text, "Server()") && strstr(out_text, "Server.Start(addr string)") &&
          strstr(out_text, "NewServer()") && strstr(out_text, "main()"));
    docs[0] = &d;
    y = 17;                                     /* srv. : the fields and the method */
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &d, y, sep, got_cb, &g) == 1);
    CHECK(has_sym(&g, "Start") && has_sym(&g, "Port") && has_sym(&g, "Name"));
    y = 18;                                     /* fmt. : the package of the library */
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    sym_members(c, docs, 1, &d, y, sep, got_cb, &g);
    CHECK(has_sym(&g, "Println") && has_sym(&g, "Sprintf"));
    doc_release(&d);
    sym_cache_free(c);
    c = sym_cache_new();                        /* the next Doc has the same address */

    put_file(dir, "p.rs", "use std::fmt;\n\npub struct Point {\n    pub x: f64,\n    y: f64,\n}\n\n"
                          "impl Point {\n    pub fn new(x: f64, y: f64) -> Point {\n        Point { x, y }\n    }\n"
                          "    fn dist(&self, other: &Point) -> f64 {\n        0.0\n    }\n}\n\n"
                          "trait Shape {\n    fn area(&self) -> f64;\n}\n\nfn main() {\n"
                          "    let p = Point::new(1.0, 2.0);\n    p.\n    HashMap::\n}\n\npub(crate) fn helper(x: i32) {}\n");
    snprintf(path, sizeof path, "%s/p.rs", dir);
    memset(&d, 0, sizeof d);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    out_text[0] = '\0';
    sym_outline(c, &d, out_cb, NULL);
    CHECK(strstr(out_text, "Point()") && strstr(out_text, "Point.new(x: f64, y: f64)") &&
          strstr(out_text, "Point.dist(other: &Point)") && strstr(out_text, "Shape()") &&
          strstr(out_text, "main()") && strstr(out_text, "helper(x: i32)"));
    docs[0] = &d;
    y = 22;                                     /* p. : the fields and the methods of impl */
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &d, y, sep, got_cb, &g) == 1);
    CHECK(has_sym(&g, "dist") && has_sym(&g, "x"));
    y = 23;                                     /* HashMap:: : the library type */
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    sym_members(c, docs, 1, &d, y, sep, got_cb, &g);
    CHECK(has_sym(&g, "new") && has_sym(&g, "insert"));
    doc_release(&d);
    sym_cache_free(c);

    /* Go: the other files of the package (not those of another package or the tests) */
    {
        char gd[64];

        CHECK(make_dir(gd, sizeof gd) != NULL);
        put_file(gd, "types.go", "package srv\n\ntype Store struct {\n\tItems []string\n}\n\nfunc helperFn(n int) int {\n\treturn n\n}\n");
        put_file(gd, "methods.go", "package srv\n\nfunc (s *Store) Save(path string) error {\n\treturn nil\n}\n");
        put_file(gd, "x_test.go", "package srv\n\nfunc testOnly() {}\n");
        put_file(gd, "gen.go", "package other\n\nfunc otherFn() {}\n");
        put_file(gd, "main.go", "package srv\n\nfunc run() {\n\tst := &Store{}\n\tst.\n}\n");
        snprintf(path, sizeof path, "%s/main.go", gd);
        c = sym_cache_new();
        memset(&d, 0, sizeof d);
        CHECK(doc_load(&d, path, err, sizeof err) == 0);
        docs[0] = &d;
        memset(&g, 0, sizeof g);
        sym_globals(c, docs, 1, &d, got_cb, &g);
        CHECK(has_sym(&g, "helperFn") && has_sym(&g, "Store"));
        CHECK(!has_sym(&g, "otherFn") && !has_sym(&g, "testOnly") && !has_sym(&g, "Save"));
        y = 4;
        sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
        memset(&g, 0, sizeof g);
        CHECK(sym_members(c, docs, 1, &d, y, sep, got_cb, &g) == 1);
        CHECK(has_sym(&g, "Save"));
        CHECK(has_sym(&g, "Items"));
        doc_release(&d);
        sym_cache_free(c);
        rm_rf(gd);
    }

    /* Go: a package of the module (go.mod), its exported names and its import path */
    {
        char md[64], sub[128];

        CHECK(make_dir(md, sizeof md) != NULL);
        snprintf(sub, sizeof sub, "%s/store", md);
        mkdir(sub, 0755);
        snprintf(sub, sizeof sub, "%s/cmd", md);
        mkdir(sub, 0755);
        put_file(md, "go.mod", "module example.com/app\n\ngo 1.22\n");
        put_file(md, "store/store.go", "package store\n\ntype DB struct {\n\tPath string\n}\n\n"
                                       "func Open(path string) *DB {\n\treturn &DB{}\n}\n\nfunc hidden() {}\n");
        php_case(md, "cmd/main.go", "package main\n\nimport \"fmt\"\n\nfunc main() {\n\t\n}\n", "store.Op", 5,
                 "import (\n\t\"fmt\"\n\n\t\"example.com/app/store\"\n)\n", 1);
        /* gofmt's groups: a module joins the modules, the library its own group */
        php_case(md, "cmd/c.go", "package main\n\nimport (\n\t\"os\"\n\n\t\"github.com/x/y\"\n)\n\nfunc main() {\n\t\n}\n",
                 "store.Op", 9, "import (\n\t\"os\"\n\n\t\"example.com/app/store\"\n\t\"github.com/x/y\"\n)\n", 1);
        php_case(md, "cmd/d.go", "package main\n\nimport (\n\t\"os\"\n\n\t\"github.com/x/y\"\n)\n\nfunc main() {\n\t\n}\n",
                 "fmt.Printl", 9, "import (\n\t\"fmt\"\n\t\"os\"\n\n\t\"github.com/x/y\"\n)\n", 1);
        php_case(md, "cmd/e.go", "package main\n\nimport (\n\t\"os\"\n\t\"strings\"\n)\n\nfunc main() {\n\t\n}\n",
                 "store.Op", 8, "import (\n\t\"os\"\n\t\"strings\"\n\n\t\"example.com/app/store\"\n)\n", 1);
        php_case(md, "cmd/f.go", "package main\n\nimport (\n\t\"github.com/x/y\"\n)\n\nfunc main() {\n\t\n}\n",
                 "fmt.Printl", 7, "import (\n\t\"fmt\"\n\n\t\"github.com/x/y\"\n)\n", 1);
        php_case(md, "cmd/b.go", "package main\n\nimport \"example.com/app/store\"\n\nfunc main() {\n\t\n}\n",
                 "store.Op", 5, "package main\n\nimport \"example.com/app/store\"\n\nfunc main() {\n", 0);
        /* a package imported under another name: its members after the alias, no new import */
        {
            static const char *al = "package main\n\nimport (\n\tf \"fmt\"\n\tm \"example.com/app/store\"\n)\n\n"
                                    "func main() {\n\t\n}\n";

            php_case(md, "cmd/g.go", al, "m.Op", 8, "\tm.Open(", 0);
            php_case(md, "cmd/g.go", al, "f.Printl", 8, "\tf.Println(", 0);
        }
        snprintf(path, sizeof path, "%s/cmd/main.go", md);
        c = sym_cache_new();
        memset(&d, 0, sizeof d);
        CHECK(doc_load(&d, path, err, sizeof err) == 0);
        docs[0] = &d;
        {
            char out[128];

            CHECK(sym_go_package(c, &d, "store", out, sizeof out) == 1 && strcmp(out, "example.com/app/store") == 0);
            CHECK(sym_go_package(c, &d, "nothere", out, sizeof out) == 0);
        }
        doc_release(&d);
        sym_cache_free(c);
        rm_rf(md);
    }

    /* Rust: the pub items of the other modules of the crate, with their use crate:: */
    {
        char rd[64], sub[128];

        CHECK(make_dir(rd, sizeof rd) != NULL);
        snprintf(sub, sizeof sub, "%s/src", rd);
        mkdir(sub, 0755);
        snprintf(sub, sizeof sub, "%s/src/net", rd);
        mkdir(sub, 0755);
        snprintf(sub, sizeof sub, "%s/src/bin", rd);
        mkdir(sub, 0755);
        put_file(rd, "Cargo.toml", "[package]\nname = \"app\"\n");
        put_file(rd, "src/net/http.rs", "pub fn fetch(url: &str) -> String {\n    String::new()\n}\n\n"
                                        "pub struct Client {\n    base: String,\n}\n\npub struct Zeta;\n\nfn private_fn() {}\n");
        put_file(rd, "src/net/mod.rs", "pub mod http;\n");
        put_file(rd, "src/orphan.rs", "pub fn orphan_fn() {}\n");
        put_file(rd, "src/util.rs", "pub(crate) const fn limit_of(n: usize) -> usize {\n    n\n}\n");
        put_file(rd, "src/bin/tool.rs", "pub fn tool_only() {}\n");
        php_case(rd, "src/main.rs", "mod net;\n\nfn main() {\n    \n}\n\nmod net;\nmod util;\n", "fetc", 3,
                 "mod net;\n\nuse crate::net::http::fetch;\n\nfn main() {\n", 1);
        php_case(rd, "src/main.rs", "use std::fs;\n\nfn main() {\n    \n}\n\nmod net;\nmod util;\n", "Clien", 3,
                 "use std::fs;\n\nuse crate::net::http::Client;\n\nfn main", 1);
        /* rustfmt's layout: the name joins the braces of its module, a use its group in order */
        php_case(rd, "src/main.rs", "use crate::net::http::{Client, Other};\n\nfn main() {\n    \n}\n\nmod net;\nmod util;\n", "fetc", 3,
                 "use crate::net::http::{fetch, Client, Other};\n\nfn main", 2);
        php_case(rd, "src/main.rs", "use crate::net::http::{Client};\n\nfn main() {\n    \n}\n\nmod net;\nmod util;\n", "Zet", 3,
                 "use crate::net::http::{Client, Zeta};\n\nfn main", 2);
        php_case(rd, "src/main.rs",
                 "use std::fs;\n\nuse serde::Serialize;\n\nuse crate::a::B;\n#[cfg(x)]\nuse crate::z::Y;\nuse crate::Top;\n\n"
                 "fn main() {\n    \n}\n\nmod net;\nmod util;\n", "Clien", 10,
                 "use crate::a::B;\nuse crate::net::http::Client;\n#[cfg(x)]\nuse crate::z::Y;\nuse crate::Top;\n\nfn main", 1);
        php_case(rd, "src/main.rs", "use serde::Serialize;\n\nuse crate::a::B;\n\nfn main() {\n    \n}\n\nmod net;\nmod util;\n", "HashMap::ne", 5,
                 "use std::collections::HashMap;\n\nuse serde::Serialize;\n\nuse crate::a::B;\n", 1);
        php_case(rd, "src/main.rs", "use crate::a::{\n    B,\n    fetch,\n};\n\nfn main() {\n    \n}\n\nmod net;\nmod util;\n", "Clien", 6,
                 "use crate::a::{\n    B,\n    fetch,\n};\nuse crate::net::http::Client;\n\nfn main", 1);
        php_case(rd, "src/main.rs", "fn main() {\n    \n}\n\nmod net;\nmod util;\n", "limit_o", 1,
                 "use crate::util::limit_of;\n\nfn main() {\n", 1);
        php_case(rd, "src/main.rs", "use crate::net::http::fetch;\n\nfn main() {\n    \n}\n\nmod net;\nmod util;\n", "fetc", 3,
                 "use crate::net::http::fetch;\n\nfn main() {\n", 0);
        php_case(rd, "src/main.rs", "fn main() {\n    \n}\n\nmod net;\nmod util;\n", "tool_on", 1, "fn main() {\n", 0);
        php_case(rd, "src/main.rs", "fn main() {\n    \n}\n\nmod net;\nmod util;\n", "private_f", 1, "fn main() {\n", 0);
        /* a file no mod declares is not part of the crate */
        php_case(rd, "src/main.rs", "fn main() {\n    \n}\n\nmod net;\nmod util;\n", "orphan_f", 1, "fn main() {\n", 0);
        rm_rf(rd);
    }

    /* Rust: a program of src/bin reaches the library by the package's name, only through what
     * lib.rs makes pub; its own modules are crate:: */
    {
        char rd[64], sub[160];
        static const char *const dirs[] = { "src", "src/bin", "src/bin/tool" };
        size_t k;

        CHECK(make_dir(rd, sizeof rd) != NULL);
        for (k = 0; k < 3; k++) {
            snprintf(sub, sizeof sub, "%s/%s", rd, dirs[k]);
            mkdir(sub, 0755);
        }
        put_file(rd, "Cargo.toml", "[package]\nname = \"my-app\"\nversion = \"0.1.0\"\n\n[dependencies]\nname = \"x\"\n");
        put_file(rd, "src/lib.rs", "pub mod net;\nmod secret;\npub use secret::{Helper, other as _};\n");
        put_file(rd, "src/net.rs", "pub fn fetch() {}\n");
        put_file(rd, "src/secret.rs", "pub fn hush() {}\npub struct Helper;\n");
        put_file(rd, "src/bin/tool/cli.rs", "pub fn parse_args() {}\n");
        php_case(rd, "src/bin/tool/main.rs", "mod cli;\n\nfn main() {\n    \n}\n", "fetc", 3,
                 "mod cli;\n\nuse my_app::net::fetch;\n\nfn main", 1);
        php_case(rd, "src/bin/tool/main.rs", "mod cli;\n\nfn main() {\n    \n}\n", "hus", 3, "mod cli;\n\nfn main", 0);
        php_case(rd, "src/bin/tool/main.rs", "mod cli;\n\nfn main() {\n    \n}\n", "Helpe", 3,
                 "mod cli;\n\nuse my_app::Helper;\n\nfn main", 1);
        php_case(rd, "src/bin/tool/main.rs", "mod cli;\n\nfn main() {\n    \n}\n", "parse_ar", 3,
                 "mod cli;\n\nuse crate::cli::parse_args;\n\nfn main", 1);
        php_case(rd, "src/bin/tool/main.rs", "mod cli;\n\nuse crate::{\n    cli::parse_args,\n};\n\nfn main() {\n    \n}\n",
                 "parse_ar", 7, "use crate::{\n    cli::parse_args,\n};\n\nfn main", 0);
        /* the braces of a use of the library (rustfmt's vertical and horizontal layouts) */
        php_case(rd, "src/bin/tool/main.rs", "mod cli;\n\nuse my_app::{\n    a::B,\n    zz::Y,\n    Helper,\n};\n\nfn main() {\n    \n}\n",
                 "fetc", 9, "use my_app::{\n    a::B,\n    net::fetch,\n    zz::Y,\n    Helper,\n};\n", 1);
        php_case(rd, "src/bin/tool/main.rs", "mod cli;\n\nuse my_app::{\n    a::B,\n};\n\nfn main() {\n    \n}\n",
                 "Helpe", 7, "use my_app::{\n    a::B,\n    Helper,\n};\n", 1);
        php_case(rd, "src/bin/tool/main.rs", "mod cli;\n\nuse my_app::{\n    a::B, zz::Y,\n};\n\nfn main() {\n    \n}\n",
                 "fetc", 7, "use my_app::{\n    a::B, net::fetch, zz::Y,\n};\n", 2);
        php_case(rd, "src/bin/tool/main.rs", "mod cli;\n\nuse my_app::{a::B, Helper};\n\nfn main() {\n    \n}\n",
                 "fetc", 5, "use my_app::{a::B, net::fetch, Helper};\n", 2);
        php_case(rd, "src/bin/tool/main.rs", "mod cli;\n\nuse my_app::{a::{B, C}, Helper};\n\nfn main() {\n    \n}\n",
                 "fetc", 5, "use my_app::{a::{B, C}, net::fetch, Helper};\n", 2);
        /* in the library: the other modules, private ones too, and what lib.rs re-exports */
        php_case(rd, "src/net.rs", "pub fn fetch() {}\n\nfn g() {\n    \n}\n", "hus", 3,
                 "use crate::secret::hush;\n\npub fn fetch", 1);
        php_case(rd, "src/net.rs", "use crate::secret::Helper;\n\nfn g() {\n    \n}\n", "Helpe", 3,
                 "use crate::secret::Helper;\n\nfn g", 0);
        rm_rf(rd);
    }

    /* the imports */
    php_case(dir, "a.go", "package main\n\nfunc main() {\n\t\n}\n", "fmt.Printl", 3,
             "package main\n\nimport \"fmt\"\n\nfunc main() {\n", 1);
    php_case(dir, "b.go", "package main\n\nimport (\n\t\"os\"\n\t\"strings\"\n)\n\nfunc main() {\n\t\n}\n", "fmt.Printl", 8,
             "import (\n\t\"fmt\"\n\t\"os\"\n\t\"strings\"\n)\n", 1);
    php_case(dir, "c.go", "package main\n\nimport \"os\"\n\nfunc main() {\n\t\n}\n", "fmt.Printl", 5,
             "package main\n\nimport (\n\t\"fmt\"\n\t\"os\"\n)\n\nfunc main() {\n", 1);
    php_case(dir, "d.go", "package main\n\nimport \"fmt\"\n\nfunc main() {\n\t\n}\n", "http.Ge", 5,
             "import (\n\t\"fmt\"\n\t\"net/http\"\n)\n", 1);
    php_case(dir, "e.go", "package main\n\nimport \"fmt\"\n\nfunc main() {\n\t\n}\n", "fmt.Printl", 5,
             "package main\n\nimport \"fmt\"\n\nfunc main() {\n", 0);
    php_case(dir, "a.rs", "fn main() {\n    \n}\n", "HashMap::ne", 1,
             "use std::collections::HashMap;\n\nfn main() {\n", 1);
    php_case(dir, "b.rs", "use std::fs;\nuse std::io;\n\nfn main() {\n    \n}\n", "HashMap::ne", 4,
             "use std::collections::HashMap;\nuse std::fs;\nuse std::io;\n", 1);
    php_case(dir, "c.rs", "use std::collections::{HashMap, HashSet};\n\nfn main() {\n    \n}\n", "HashMap::ne", 3,
             "use std::collections::{HashMap, HashSet};\n\nfn main() {\n", 0);
    php_case(dir, "d.rs", "fn main() {\n    \n}\n", "fs::read_to_st", 1, "use std::fs;\n\nfn main() {\n", 1);
    php_case(dir, "e.rs", "fn main() {\n    \n}\n", "Vec::ne", 1, "fn main() {\n", 0);
    php_case(dir, "f.rs", "//! Crate docs\n#![allow(dead_code)]\n\nfn main() {\n    \n}\n", "HashMap::ne", 4,
             "//! Crate docs\n#![allow(dead_code)]\n\nuse std::collections::HashMap;\n\nfn main() {\n", 1);
    rm_rf(dir);
}

/* Java: the index (classes, methods with their type, constructors, fields), the standard
 * library after a typed variable or a class, the same package without import, the other packages
 * and the library with their import in order. */
static void test_java(void)
{
    char dir[64], sub[200], path[256], err[128];
    static const char *const dirs[] = { "src", "src/main", "src/main/java", "src/main/java/com",
                                        "src/main/java/com/acme", "src/main/java/com/acme/app",
                                        "src/main/java/com/acme/util", "src/test", "src/test/java",
                                        "src/test/java/com", "src/test/java/com/acme",
                                        "src/test/java/com/acme/util" };
    size_t i, y, sep;
    Doc d;
    SymCache *c;
    Doc *docs[1];
    static Got g;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(sub, sizeof sub, "%s/%s", dir, dirs[i]);
        mkdir(sub, 0755);
    }
    put_file(dir, "pom.xml", "<project/>\n");
    put_file(dir, "src/main/java/com/acme/util/TextUtils.java",
             "package com.acme.util;\n\npublic final class TextUtils {\n    public static String shout(String s) {\n"
             "        return s;\n    }\n}\n");
    put_file(dir, "src/test/java/com/acme/util/TextUtilsTest.java",
             "package com.acme.util;\n\npublic class TextUtilsTest {\n}\n");
    put_file(dir, "src/main/java/com/acme/app/Helper.java",
             "package com.acme.app;\n\npublic class Helper {\n    public int helpMe(int n) {\n        return n;\n    }\n}\n");
    put_file(dir, "src/main/java/com/acme/app/Order.java",
             "package com.acme.app;\n\nimport java.util.List;\n\npublic class Order {\n    private final String id;\n"
             "    private List<String> tags;\n\n    public Order(String id) {\n        this.id = id;\n    }\n\n"
             "    public static <T> List<T> wrap(T x) {\n        return null;\n    }\n\n"
             "    public String[] parts(int n) {\n        return null;\n    }\n\n    void run() {\n"
             "        Helper h = new Helper();\n        h.\n        tags.stream().filter(null).\n        System.out.\n    }\n}\n");
    snprintf(path, sizeof path, "%s/src/main/java/com/acme/app/Order.java", dir);
    c = sym_cache_new();
    memset(&d, 0, sizeof d);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    out_text[0] = '\0';
    sym_outline(c, &d, out_cb, NULL);
    CHECK(strstr(out_text, "Order()") && strstr(out_text, "Order.Order(String id)") &&
          strstr(out_text, "Order.wrap(T x)") && strstr(out_text, "Order.parts(int n)") && strstr(out_text, "Order.run()"));
    docs[0] = &d;
    for (y = 22; y <= 24; y++) {
        sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
        CHECK(sep != (size_t)-1);
        if (sep == (size_t)-1)
            continue;
        memset(&g, 0, sizeof g);
        CHECK(sym_members(c, docs, 1, &d, y, sep, got_cb, &g) == 1);
        CHECK(y == 22 ? has_sym(&g, "helpMe") : y == 23 ? has_sym(&g, "map") && has_sym(&g, "collect")
                                                       : has_sym(&g, "println"));
    }
    doc_release(&d);
    sym_cache_free(c);

    /* the imports */
    {
        static const char *m = "package com.acme.app;\n\nimport java.util.List;\nimport java.util.Map;\n\npublic class M {\n"
                               "    void f() {\n        \n    }\n}\n";

        php_case(dir, "src/main/java/com/acme/app/M.java", m, "Collections.so", 7,
                 "import java.util.Collections;\nimport java.util.List;\nimport java.util.Map;\n", 1);
        php_case(dir, "src/main/java/com/acme/app/M.java", m, "Files.readStr", 7,
                 "import java.nio.file.Files;\nimport java.util.List;\n", 1);
        php_case(dir, "src/main/java/com/acme/app/M.java", m, "TextUt", 7,
                 "import java.util.Map;\n\nimport com.acme.util.TextUtils;\n\npublic class M", 1);
        php_case(dir, "src/main/java/com/acme/app/M.java", m, "List.o", 7, "import java.util.Map;\n\npublic class M", 0);
        php_case(dir, "src/main/java/com/acme/app/M.java", m, "TextUtils.shou", 7,
                 "import java.util.Map;\n\nimport com.acme.util.TextUtils;\n\npublic class M", 1);
        php_case(dir, "src/main/java/com/acme/app/M.java", m, "Helpe", 7, "import java.util.Map;\n\npublic class M", 0);
        php_case(dir, "src/main/java/com/acme/app/M.java", m, "TextUtilsTe", 7, "import java.util.Map;\n\npublic class M", 0);
        php_case(dir, "src/main/java/com/acme/app/N.java", "package com.acme.app;\n\npublic class N {\n    void f() {\n        \n    }\n}\n",
                 "ArrayLi", 4, "package com.acme.app;\n\nimport java.util.ArrayList;\n\npublic class N", 1);
        php_case(dir, "src/main/java/com/acme/app/N.java",
                 "package com.acme.app;\n\nimport java.util.*;\n\npublic class N {\n    void f() {\n        \n    }\n}\n",
                 "ArrayLi", 6, "import java.util.*;\n\npublic class N", 0);
        php_case(dir, "src/main/java/com/acme/app/N.java",
                 "package com.acme.app;\n\nimport com.other.List;\n\npublic class N {\n    void f() {\n        \n    }\n}\n",
                 "List.o", 6, "import com.other.List;\n\npublic class N", 0);
    }
    rm_rf(dir);
}

/* Java: the constants of an enum, the members an import static brings, and in a build of
 * several modules only the classes of the modules the file's one depends on. */
static void test_java_more(void)
{
    char dir[64], sub[200];
    static const char *const dirs[] = {
        "app", "app/src", "app/src/main", "app/src/main/java", "app/src/main/java/app",
        "core", "core/src", "core/src/main", "core/src/main/java", "core/src/main/java/core",
        "base", "base/src", "base/src/main", "base/src/main/java", "base/src/main/java/base",
        "extra", "extra/src", "extra/src/main", "extra/src/main/java", "extra/src/main/java/extra"
    };
    size_t i;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(sub, sizeof sub, "%s/%s", dir, dirs[i]);
        mkdir(sub, 0755);
    }
    put_file(dir, "settings.gradle", "include 'app', 'core', 'base', 'extra'\n");
    put_file(dir, "app/build.gradle", "dependencies {\n    implementation project(':core')\n}\n");
    put_file(dir, "core/build.gradle", "dependencies {\n    api project(\":base\")\n}\n");
    put_file(dir, "base/build.gradle", "\n");
    put_file(dir, "extra/build.gradle", "\n");
    put_file(dir, "core/src/main/java/core/Engine.java", "package core;\n\npublic class Engine {\n"
             "    public static int powerOf(int n) {\n        return n;\n    }\n}\n");
    put_file(dir, "base/src/main/java/base/Basement.java", "package base;\n\npublic class Basement {\n}\n");
    put_file(dir, "extra/src/main/java/extra/Gadget.java", "package extra;\n\npublic class Gadget {\n}\n");
    put_file(dir, "app/src/main/java/app/Color.java",
             "package app;\n\npublic enum Color {\n    RED(\"r\"),\n    // a comment\n    GREEN(\"g\") {\n        void f() {}\n    },\n"
             "    BLUE_SKY(\"b\");\n\n    private final String code;\n\n    Color(String code) {\n        this.code = code;\n    }\n}\n");
    {
        static const char *m = "package app;\n\nimport static java.lang.Math.max;\nimport static core.Engine.*;\n\n"
                                "public class M {\n    void f() {\n        \n    }\n}\n";

        /* the enum's constants after its name */
        php_case(dir, "app/src/main/java/app/M.java", m, "Color.GRE", 7, "        Color.GREEN", 0);
        php_case(dir, "app/src/main/java/app/M.java", m, "Color.BLUE_S", 7, "        Color.BLUE_SKY", 0);
        /* import static: a member of the library, every member of a class of the project */
        php_case(dir, "app/src/main/java/app/M.java", m, "ma", 7, "        max(", 0);
        php_case(dir, "app/src/main/java/app/M.java", m, "powerO", 7, "        powerOf(", 0);
        /* modules: core and, through it, base; not extra */
        php_case(dir, "app/src/main/java/app/M.java", m, "Engin", 7, "import static core.Engine.*;\n\nimport core.Engine;\n", 1);
        php_case(dir, "app/src/main/java/app/M.java", m, "Basemen", 7, "import base.Basement;\n", 1);
        php_case(dir, "app/src/main/java/app/M.java", m, "Gadge", 7, "import static core.Engine.*;\n\npublic class M", 0);
    }
    rm_rf(dir);

    /* Maven: the artifactIds of the module's dependencies */
    {
        char md[64];
        static const char *const mdirs[] = {
            "web", "web/src", "web/src/main", "web/src/main/java", "web/src/main/java/web",
            "model", "model/src", "model/src/main", "model/src/main/java", "model/src/main/java/model",
            "tools", "tools/src", "tools/src/main", "tools/src/main/java", "tools/src/main/java/tools"
        };

        CHECK(make_dir(md, sizeof md) != NULL);
        for (i = 0; i < sizeof mdirs / sizeof mdirs[0]; i++) {
            snprintf(sub, sizeof sub, "%s/%s", md, mdirs[i]);
            mkdir(sub, 0755);
        }
        put_file(md, "pom.xml", "<project>\n  <artifactId>parent</artifactId>\n  <modules>\n    <module>web</module>\n  </modules>\n</project>\n");
        put_file(md, "web/pom.xml", "<project>\n  <parent>\n    <artifactId>parent</artifactId>\n  </parent>\n"
                                     "  <artifactId>shop-web</artifactId>\n  <dependencies>\n    <dependency>\n"
                                     "      <artifactId>shop-model</artifactId>\n    </dependency>\n  </dependencies>\n</project>\n");
        put_file(md, "model/pom.xml", "<project>\n  <artifactId>shop-model</artifactId>\n</project>\n");
        put_file(md, "tools/pom.xml", "<project>\n  <artifactId>shop-tools</artifactId>\n</project>\n");
        put_file(md, "model/src/main/java/model/Product.java", "package model;\n\npublic class Product {\n}\n");
        put_file(md, "tools/src/main/java/tools/Toolkit.java", "package tools;\n\npublic class Toolkit {\n}\n");
        php_case(md, "web/src/main/java/web/W.java", "package web;\n\npublic class W {\n    void f() {\n        \n    }\n}\n",
                 "Produc", 4, "package web;\n\nimport model.Product;\n\npublic class W", 1);
        php_case(md, "web/src/main/java/web/W.java", "package web;\n\npublic class W {\n    void f() {\n        \n    }\n}\n",
                 "Toolki", 4, "package web;\n\npublic class W", 0);
        rm_rf(md);
    }
}

/* Java builds: the forms of a dependency between modules. Gradle: project accessors
 * (projects.myLib for my-lib), project(path: ...), the root's subprojects { }, test
 * configurations; Maven: the parent's dependencies, not <dependencyManagement> nor the plugins'
 * ones, scope test only from a test. */
/* Creates root/rel and the folders above it. */
static void mkdirs(const char *root, const char *rel)
{
    char d[400];
    size_t k, n;

    n = (size_t)snprintf(d, sizeof d, "%s/%s", root, rel);
    for (k = strlen(root) + 1; k <= n && k < sizeof d; k++)
        if (d[k] == '/' || d[k] == '\0') {
            char ch = d[k];

            d[k] = '\0';
            mkdir(d, 0755);
            d[k] = ch;
        }
}

/* A module of a Java build with, if cls, a public class in package pkg (main or test sources). */
static void jmod(const char *root, const char *mod, const char *pkg, const char *cls, int test)
{
    char rel[300], body[200];

    snprintf(rel, sizeof rel, "%s/src/%s/java/%s", mod, test ? "test" : "main", pkg);
    mkdirs(root, rel);
    if (cls) {
        snprintf(rel, sizeof rel, "%s/src/%s/java/%s/%s.java", mod, test ? "test" : "main", pkg, cls);
        snprintf(body, sizeof body, "package %s;\n\npublic class %s {\n}\n", pkg, cls);
        put_file(root, rel, body);
    }
}

static void test_java_builds(void)
{
    char dir[64];
    static const char *w = "package app;\n\npublic class W {\n    void f() {\n        \n    }\n}\n";

    CHECK(make_dir(dir, sizeof dir) != NULL);
    jmod(dir, "app", "app", NULL, 0);
    jmod(dir, "app", "app", NULL, 1);
    jmod(dir, "my-lib", "mylib", "LibThing", 0);
    jmod(dir, "pathdep", "pathdep", "PathThing", 0);
    jmod(dir, "common", "common", "CommonThing", 0);
    jmod(dir, "testkit", "testkit", "KitThing", 0);
    jmod(dir, "other", "other", "OtherThing", 0);
    put_file(dir, "settings.gradle", "include 'app', 'my-lib', 'pathdep', 'common', 'testkit', 'other'\n");
    put_file(dir, "build.gradle", "subprojects {\n    dependencies {\n        implementation project(':common')\n    }\n}\n");
    put_file(dir, "app/build.gradle", "dependencies {\n    implementation(projects.myLib)\n"
                                      "    implementation project(path: ':pathdep')\n    implementation libs.guava\n"
                                      "    testImplementation project(':testkit')\n    // implementation project(':other')\n}\n");
    put_file(dir, "my-lib/build.gradle", "\n");
    put_file(dir, "pathdep/build.gradle", "\n");
    put_file(dir, "common/build.gradle", "\n");
    put_file(dir, "testkit/build.gradle", "\n");
    put_file(dir, "other/build.gradle", "\n");
    php_case(dir, "app/src/main/java/app/W.java", w, "LibThin", 4, "import mylib.LibThing;\n", 1);
    php_case(dir, "app/src/main/java/app/W.java", w, "PathThin", 4, "import pathdep.PathThing;\n", 1);
    php_case(dir, "app/src/main/java/app/W.java", w, "CommonThin", 4, "import common.CommonThing;\n", 1);
    php_case(dir, "app/src/main/java/app/W.java", w, "KitThin", 4, "package app;\n\npublic class W", 0);
    php_case(dir, "app/src/test/java/app/W.java", w, "KitThin", 4, "import testkit.KitThing;\n", 1);
    php_case(dir, "app/src/main/java/app/W.java", w, "OtherThin", 4, "package app;\n\npublic class W", 0);
    rm_rf(dir);

    CHECK(make_dir(dir, sizeof dir) != NULL);
    jmod(dir, "web", "web", NULL, 0);
    jmod(dir, "web", "web", NULL, 1);
    jmod(dir, "common", "common", "CommonThing", 0);
    jmod(dir, "tools", "tools", "ToolThing", 0);
    jmod(dir, "plug", "plug", "PlugThing", 0);
    jmod(dir, "fixtures", "fixtures", "FixThing", 0);
    put_file(dir, "pom.xml", "<project>\n  <artifactId>shop-parent</artifactId>\n  <dependencies>\n    <dependency>\n"
                             "      <artifactId>shop-common</artifactId>\n    </dependency>\n  </dependencies>\n"
                             "  <dependencyManagement>\n    <dependencies>\n      <dependency>\n"
                             "        <artifactId>shop-tools</artifactId>\n      </dependency>\n    </dependencies>\n"
                             "  </dependencyManagement>\n</project>\n");
    put_file(dir, "web/pom.xml", "<project>\n  <parent>\n    <artifactId>shop-parent</artifactId>\n  </parent>\n"
                                 "  <artifactId>shop-web</artifactId>\n  <dependencies>\n    <dependency>\n"
                                 "      <artifactId>shop-fixtures</artifactId>\n      <scope>test</scope>\n    </dependency>\n"
                                 "    <!-- <dependency><artifactId>shop-tools</artifactId></dependency> -->\n"
                                 "  </dependencies>\n  <build>\n    <plugins>\n      <plugin>\n        <dependencies>\n"
                                 "          <dependency>\n            <artifactId>shop-plug</artifactId>\n"
                                 "          </dependency>\n        </dependencies>\n      </plugin>\n    </plugins>\n"
                                 "  </build>\n</project>\n");
    put_file(dir, "common/pom.xml", "<project>\n  <artifactId>shop-common</artifactId>\n</project>\n");
    put_file(dir, "tools/pom.xml", "<project>\n  <artifactId>shop-tools</artifactId>\n</project>\n");
    put_file(dir, "plug/pom.xml", "<project>\n  <artifactId>shop-plug</artifactId>\n</project>\n");
    put_file(dir, "fixtures/pom.xml", "<project>\n  <artifactId>shop-fixtures</artifactId>\n</project>\n");
    {
        static const char *v = "package web;\n\npublic class V {\n    void f() {\n        \n    }\n}\n";

        php_case(dir, "web/src/main/java/web/V.java", v, "CommonThin", 4, "import common.CommonThing;\n", 1);
        php_case(dir, "web/src/main/java/web/V.java", v, "ToolThin", 4, "package web;\n\npublic class V", 0);
        php_case(dir, "web/src/main/java/web/V.java", v, "PlugThin", 4, "package web;\n\npublic class V", 0);
        php_case(dir, "web/src/main/java/web/V.java", v, "FixThin", 4, "package web;\n\npublic class V", 0);
        php_case(dir, "web/src/test/java/web/V.java", v, "FixThin", 4, "import fixtures.FixThing;\n", 1);
    }
    rm_rf(dir);
}

static int hdr_seen;
static const char *hdr_want;

static void hdr_find_cb(void *ctx, const char *name, int kind, const char *sig, const char *ret, const char *spec)
{
    (void)ctx;
    (void)kind;
    (void)sig;
    (void)ret;
    (void)spec;
    if (strcmp(name, hdr_want) == 0)
        hdr_seen = 1;
}

/* Gradle projects whose folder is not their name (settings.gradle's projectDir); a module of
 * many classes is read whole. */
static void test_java_settings_and_size(void)
{
    char dir[64], rel[200], body[160], path[256], err[128];
    static const char *w = "package app;\n\npublic class W {\n    void f() {\n        \n    }\n}\n";
    int i;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    jmod(dir, "app", "app", NULL, 0);
    jmod(dir, "libs/engine-impl", "engine", "Motor", 0);
    jmod(dir, "libs/spare", "spare", "SparePart", 0);
    put_file(dir, "settings.gradle", "include ':app', ':core', ':spare'\n"
                                     "project(':core').projectDir = file('libs/engine-impl')\n"
                                     "project(':spare').projectDir = new File(rootDir, './libs/spare')\n");
    put_file(dir, "app/build.gradle", "dependencies {\n    implementation project(':core')\n}\n");
    put_file(dir, "libs/engine-impl/build.gradle", "\n");
    put_file(dir, "libs/spare/build.gradle", "\n");
    php_case(dir, "app/src/main/java/app/W.java", w, "Moto", 4, "import engine.Motor;\n", 1);
    php_case(dir, "app/src/main/java/app/W.java", w, "SparePar", 4, "package app;\n\npublic class W", 0);
    put_file(dir, "settings.gradle", "");
    put_file(dir, "settings.gradle.kts", "include(\":app\", \":core\")\nproject(\":core\").projectDir = file(\"libs/engine-impl\")\n");
    php_case(dir, "app/src/main/java/app/W.java", w, "Moto", 4, "import engine.Motor;\n", 1);
    rm_rf(dir);

    CHECK(make_dir(dir, sizeof dir) != NULL);
    jmod(dir, "app", "app", NULL, 0);
    jmod(dir, "big", "big", NULL, 0);
    put_file(dir, "settings.gradle", "include 'app', 'big'\n");
    put_file(dir, "app/build.gradle", "dependencies {\n    implementation project(':big')\n}\n");
    put_file(dir, "big/build.gradle", "\n");
    for (i = 0; i < 900; i++) {
        snprintf(rel, sizeof rel, "big/src/main/java/big/Big%d.java", i);
        snprintf(body, sizeof body, "package big;\n\npublic class Big%d {\n}\n", i);
        put_file(dir, rel, body);
    }
    put_file(dir, "app/src/main/java/app/W.java", w);
    snprintf(path, sizeof path, "%s/app/src/main/java/app/W.java", dir);
    {
        Doc d;
        Doc *docs[1];
        SymCache *c = sym_cache_new();
        static const char *const want[] = { "Big0", "Big450", "Big899" };
        size_t k;

        memset(&d, 0, sizeof d);
        CHECK(doc_load(&d, path, err, sizeof err) == 0);
        docs[0] = &d;
        for (k = 0; k < 3; k++) {
            hdr_seen = 0;
            hdr_want = want[k];
            sym_project_symbols(c, docs, 1, &d, hdr_find_cb, NULL);
            CHECK(hdr_seen);
        }
        doc_release(&d);
        sym_cache_free(c);
    }
    rm_rf(dir);
}

/* C#: the index (classes, methods, generic methods, constructors, properties, enum constants),
 * the base library after a typed variable or a type, and the usings: in order with System first,
 * in the block of the same namespace, not for the implicit usings of the .csproj or of
 * Directory.Build.props nor for the global usings of the project, nor for the file's namespace
 * or those around it; the public types of the referenced projects only. */
static void test_csharp(void)
{
    char dir[64], path[256], err[128];
    size_t y, sep;
    Doc d;
    SymCache *c;
    Doc *docs[1];
    static Got g;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    mkdirs(dir, "App/Models");
    mkdirs(dir, "Core");
    mkdirs(dir, "Tools");
    mkdirs(dir, "Old");
    put_file(dir, "Acme.sln", "\n");
    put_file(dir, "App/App.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\">\n  <PropertyGroup>\n"
                                    "    <ImplicitUsings>enable</ImplicitUsings>\n  </PropertyGroup>\n  <ItemGroup>\n"
                                    "    <ProjectReference Include=\"..\\Core\\Core.csproj\" />\n    <Using Include=\"Acme.Extra\" />\n"
                                    "  </ItemGroup>\n</Project>\n");
    put_file(dir, "App/Usings.cs", "global using Acme.Glob;\n");
    put_file(dir, "Core/Core.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\">\n</Project>\n");
    put_file(dir, "Tools/Tools.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\">\n</Project>\n");
    put_file(dir, "Old/Old.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\">\n</Project>\n");
    put_file(dir, "Directory.Build.props", "<Project>\n</Project>\n");
    put_file(dir, "Core/TextUtils.cs", "namespace Acme.Core;\n\npublic static class TextUtils\n{\n"
                                       "    public static string Shout(string s) => s;\n}\n\ninternal class Hidden\n{\n}\n");
    put_file(dir, "Tools/Tooling.cs", "namespace Acme.Tools;\n\npublic class Tooling\n{\n}\n");
    put_file(dir, "App/Models/Order.cs",
             "namespace Acme.App.Models\n{\n    public enum Status\n    {\n        Open,\n        Closed = 2,\n    }\n\n"
             "    public class Order\n    {\n        public string Id { get; set; }\n        private int qty;\n\n"
             "        public Order(string id)\n        {\n            this.Id = id;\n        }\n\n"
             "        public int Total(int price) => qty * price;\n\n        public List<T> Wrap<T>(T x) => null;\n\n"
             "        void Run()\n        {\n            List<string> names = new List<string>();\n            names.\n"
             "            Console.\n            Status.\n        }\n    }\n}\n");
    snprintf(path, sizeof path, "%s/App/Models/Order.cs", dir);
    c = sym_cache_new();
    memset(&d, 0, sizeof d);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    out_text[0] = '\0';
    sym_outline(c, &d, out_cb, NULL);
    CHECK(strstr(out_text, "Order()") && strstr(out_text, "Order.Order(string id)") &&
          strstr(out_text, "Order.Total(int price)") && strstr(out_text, "Order.Wrap(T x)") && strstr(out_text, "Order.Id"));
    docs[0] = &d;
    for (y = 25; y <= 27; y++) {
        sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
        CHECK(sep != (size_t)-1);
        if (sep == (size_t)-1)
            continue;
        memset(&g, 0, sizeof g);
        CHECK(sym_members(c, docs, 1, &d, y, sep, got_cb, &g) == 1);
        CHECK(y == 25 ? has_sym(&g, "Where") && has_sym(&g, "Add") : y == 26 ? has_sym(&g, "WriteLine")
                                                                         : has_sym(&g, "Open") && has_sym(&g, "Closed"));
    }
    doc_release(&d);
    sym_cache_free(c);

    {
        static const char *p = "using System.Text;\nusing Acme.Zeta;\n\nnamespace Acme.App;\n\npublic class P\n{\n"
                               "    void F()\n    {\n        \n    }\n}\n";
        static const char *q = "namespace Acme.App\n{\n    using System;\n    using System.Linq;\n\n    public class Q\n    {\n"
                               "        void F()\n        {\n            \n        }\n    }\n}\n";

        php_case(dir, "App/P.cs", p, "Regex.IsMat", 9,
                 "using System.Text;\nusing System.Text.RegularExpressions;\nusing Acme.Zeta;\n", 1);
        php_case(dir, "App/P.cs", p, "Stopwatch.StartNe", 9, "using System.Diagnostics;\nusing System.Text;\n", 1);
        php_case(dir, "App/P.cs", p, "Console.WriteLi", 9, "using System.Text;\nusing Acme.Zeta;\n\nnamespace", 0);
        php_case(dir, "App/P.cs", p, "File.ReadAllTe", 9, "using System.Text;\nusing Acme.Zeta;\n\nnamespace", 0);
        php_case(dir, "App/Q.cs", q, "Regex.IsMat", 9,
                 "    using System;\n    using System.Linq;\n    using System.Text.RegularExpressions;\n", 1);
        /* the project: a referenced one (public only), not another; the namespace around */
        php_case(dir, "App/P.cs", p, "TextUti", 9, "using Acme.Core;\nusing Acme.Zeta;\n", 1);
        php_case(dir, "App/P.cs", p, "Hidde", 9, "using Acme.Zeta;\n\nnamespace", 0);
        php_case(dir, "App/P.cs", p, "Toolin", 9, "using Acme.Zeta;\n\nnamespace", 0);
        php_case(dir, "App/P.cs", p, "Orde", 9, "using Acme.App.Models;\nusing Acme.Zeta;\n", 1);
        php_case(dir, "App/P.cs", p, "TextUtils.Shou", 9, "        TextUtils.Shout(", 1);
        /* a project without implicit usings: System.IO is written */
        php_case(dir, "Old/O.cs", "namespace Acme.Old;\n\npublic class O\n{\n    void F()\n    {\n        \n    }\n}\n",
                 "File.ReadAllTe", 6, "using System.IO;\n\nnamespace Acme.Old;\n", 1);
        /* Directory.Build.props for all the projects under it */
        put_file(dir, "Directory.Build.props", "<Project>\n  <PropertyGroup>\n    <ImplicitUsings>enable</ImplicitUsings>\n"
                                               "  </PropertyGroup>\n</Project>\n");
        php_case(dir, "Old/O.cs", "namespace Acme.Old;\n\npublic class O\n{\n    void F()\n    {\n        \n    }\n}\n",
                 "File.ReadAllTe", 6, "namespace Acme.Old;\n\npublic class O", 0);
    }
    {
        /* the usings the project gives: <Using Include> and global using */
        SymCache *k = sym_cache_new();
        Doc dd;

        snprintf(path, sizeof path, "%s/App/P.cs", dir);
        memset(&dd, 0, sizeof dd);
        CHECK(doc_load(&dd, path, err, sizeof err) == 0);
        CHECK(sym_cs_using_known(k, &dd, "Acme.Extra") && sym_cs_using_known(k, &dd, "Acme.Glob") &&
              sym_cs_using_known(k, &dd, "System.Linq") && sym_cs_using_known(k, &dd, "Acme") &&
              !sym_cs_using_known(k, &dd, "System.Text.Json") && !sym_cs_using_known(k, &dd, "Acme.Core"));
        doc_release(&dd);
        sym_cache_free(k);
    }
    rm_rf(dir);
}

/* .env: the names before "=" (also after export), comments only at the start of a line (a "#"
 * in a value is part of it), quoted values over more lines, ${VAR} and $VAR; the names are the
 * file's outline. */
static void test_dotenv(void)
{
    Buffer b;
    Syntax *s;
    uint16_t st = 0;
    size_t y = 0, x = 0;
    static const char *text = "# db\nAPP_NAME=\"My App\"\nexport PATH=$HOME:${X}\nDEBUG=true\nURL=a#b\n"
                              "MULTI=\"one\ntwo\"\n  PORT = 5432\n";

    buf_init(&b);
    b.path = xstrdup(".env");
    s = syn_detect(&b);
    CHECK(s != NULL);
    if (!s) {
        buf_free(&b);
        return;
    }
    CLS(s, st, "# db", "cccc");
    CLS(s, st, "APP_NAME=\"My App\"", "vvvvvvvvossssssss");
    CLS(s, st, "export PATH=$HOME:${X}", "kkkkkk.vvvvovvvvv.vvvv");
    CLS(s, st, "DEBUG=true", "vvvvvoCCCC");
    CLS(s, st, "URL=a#b", "vvvo...");
    CLS(s, st, "MULTI=\"one", "vvvvvossss");
    CLS(s, st, "two\"", "ssss");
    CLS(s, st, "  PORT = 5432", "..vvvv.o.nnnn");
    CLS(s, st, "x y=1", "...on");
    syn_free(s);
    buf_insert_text(&b, &y, &x, text, strlen(text));
    {
        Doc d;
        SymCache *c = sym_cache_new();

        memset(&d, 0, sizeof d);
        d.buf = b;
        d.syn = syn_detect(&d.buf);
        out_text[0] = '\0';
        sym_outline(c, &d, out_cb, NULL);
        CHECK(strstr(out_text, "APP_NAME") && strstr(out_text, "PATH") && strstr(out_text, "DEBUG") &&
              strstr(out_text, "PORT"));
        syn_free(d.syn);
        sym_cache_free(c);
    }
    buf_free(&b);
}

/* The names of the project's .env variables where a program reads them, in each language;
 * nowhere else. */
static void test_env_suggest(void)
{
    char dir[64];
    static const struct { const char *file, *text, *typed, *want; size_t cy; } t[] = {
        { "app/a.php", "<?php\n$x = 1;\n", "env('APP_N", "env('APP_NAME')", 1 },
        { "app/a.php", "<?php\n$x = 1;\n", "getenv(\"DB_P", "getenv(\"DB_PORT\")", 1 },
        { "app/a.php", "<?php\n$x = 1;\n", "$_ENV['LOCAL_O", "$_ENV['LOCAL_ONLY']", 1 },
        { "app/a.js", "let x = 1;\n", "process.env.APP_N", "process.env.APP_NAME", 0 },
        { "app/a.js", "let x = 1;\n", "import.meta.env.DB_P", "import.meta.env.DB_PORT", 0 },
        { "app/a.py", "x = 1\n", "os.getenv('APP_N", "os.getenv('APP_NAME')", 0 },
        { "app/a.py", "x = 1\n", "os.environ['DB_P", "os.environ['DB_PORT']", 0 },
        { "app/a.go", "package main\n\n", "os.Getenv(\"APP_N", "os.Getenv(\"APP_NAME\")", 1 },
        { "app/a.rs", "fn main() {}\n", "env::var(\"APP_N", "env::var(\"APP_NAME\")", 0 },
        { "app/A.java", "class A {}\n", "System.getenv(\"APP_N", "System.getenv(\"APP_NAME\")", 0 },
        { "app/a.cs", "class A {}\n", "Environment.GetEnvironmentVariable(\"APP_N",
          "Environment.GetEnvironmentVariable(\"APP_NAME\")", 0 },
        { "app/a.c", "int x;\n", "getenv(\"APP_N", "getenv(\"APP_NAME\")", 0 },
        { "app/a.rb", "x = 1\n\n", "ENV['APP_N", "ENV['APP_NAME']", 1 },
        { "app/a.sh", "x=1\n", "echo $APP_N", "echo $APP_NAME", 0 },
        { "app/a.sh", "x=1\n", "echo ${DB_P", "echo ${DB_PORT}", 0 },
        { "app/sub/b.php", "<?php\n$x = 1;\n", "env('SUB_O", "env('SUB_ONLY')", 1 },
        { "app/sub/b.php", "<?php\n$x = 1;\n", "env('APP_N", "env('APP_NAME')", 1 },
    };
    size_t i;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    mkdirs(dir, ".git");
    mkdirs(dir, "app/sub");
    put_file(dir, "app/.env", "# settings\nAPP_NAME=\"My App\"\nexport DB_PORT=5432\nSECRET=x#y\n");
    put_file(dir, "app/.env.local", "LOCAL_ONLY=1\nAPP_NAME=override\n");
    put_file(dir, "app/sub/.env", "SUB_ONLY=1\n");
    put_file(dir, "app/.env.php", "<?php OTHER_THING=1\n");
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        char text[200];

        snprintf(text, sizeof text, "%s", t[i].text);
        php_case(dir, t[i].file, text, t[i].typed, t[i].cy, t[i].want, 0);
    }
    /* a string that no call reads as a variable stays a plain word */
    {
        Doc *d;
        Editor e;
        char path[256], err[128];
        const char *q;

        put_file(dir, "app/c.php", "<?php\n$x = 1;\n");
        snprintf(path, sizeof path, "%s/app/c.php", dir);
        editor_init(&e);
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d != NULL);
        editor_activate(&e, d);
        e.cy = 1;
        e.cx = e.buf->lines[1].len;
        for (q = "echo('APP_N"; *q; q++)
            kkey(&e, (uint32_t)(unsigned char)*q, 0);
        CHECK(!e.comp.on);
        editor_free(&e);
    }
    rm_rf(dir);
}

/* How many colours: from COLORTERM, TERM (PuTTY and KiTTY send "xterm" over SSH: 256) and
 * WT_SESSION; "colors =" in the configuration forces it. */
static void test_color_depth(void)
{
    static const struct { const char *term, *colorterm, *wt; int want; } t[] = {
        { "xterm", NULL, NULL, COLOR_256 }, { "putty", NULL, NULL, COLOR_256 },
        { "xterm-256color", NULL, NULL, COLOR_256 }, { "screen", NULL, NULL, COLOR_256 },
        { "tmux-256color", NULL, NULL, COLOR_256 }, { "rxvt-unicode", NULL, NULL, COLOR_256 },
        { "xterm", "truecolor", NULL, COLOR_TRUE }, { "xterm-direct", NULL, NULL, COLOR_TRUE },
        { "xterm-kitty", NULL, NULL, COLOR_TRUE }, { "alacritty", NULL, NULL, COLOR_TRUE },
        { "xterm-256color", NULL, "1234", COLOR_TRUE }, { "linux", NULL, NULL, COLOR_16 },
        { "vt100", NULL, NULL, COLOR_16 }, { "dumb", NULL, NULL, COLOR_NONE },
        { "xtermx", NULL, NULL, COLOR_16 },
    };
    char *keep_t = getenv("TERM") ? xstrdup(getenv("TERM")) : NULL;
    char *keep_c = getenv("COLORTERM") ? xstrdup(getenv("COLORTERM")) : NULL;
    char *keep_w = getenv("WT_SESSION") ? xstrdup(getenv("WT_SESSION")) : NULL;
    char *keep_n = getenv("NO_COLOR") ? xstrdup(getenv("NO_COLOR")) : NULL;
    size_t i;
    Config c;
    static const char *text = "colors = truecolor\n";

    unsetenv("NO_COLOR");
    for (i = 0; i < sizeof t / sizeof t[0]; i++) {
        setenv("TERM", t[i].term, 1);
        if (t[i].colorterm)
            setenv("COLORTERM", t[i].colorterm, 1);
        else
            unsetenv("COLORTERM");
        if (t[i].wt)
            setenv("WT_SESSION", t[i].wt, 1);
        else
            unsetenv("WT_SESSION");
        if (theme_detect_depth() != t[i].want)
            fprintf(stderr, "TERM=%s: %d, want %d\n", t[i].term, theme_detect_depth(), t[i].want);
        CHECK(theme_detect_depth() == t[i].want);
    }
    config_init(&c);
    CHECK(c.colors == -1);
    config_parse(&c, text, strlen(text));
    CHECK(c.colors == COLOR_TRUE && c.nerrors == 0);
    config_parse(&c, "colors = 16\n", 12);
    CHECK(c.colors == COLOR_16);
    config_parse(&c, "colors = auto\n", 14);
    CHECK(c.colors == -1);
    config_parse(&c, "colors = many\n", 14);
    CHECK(c.nerrors == 1);
    CHECK(c.glyphs == -1);
    config_parse(&c, "glyphs = ascii\n", 15);
    CHECK(c.glyphs == 2);
    config_parse(&c, "glyphs = windows\n", 17);
    CHECK(c.glyphs == 1);
    config_parse(&c, "glyphs = unicode\n", 17);
    CHECK(c.glyphs == 0 && c.nerrors == 1);
    config_parse(&c, "glyphs = fancy\n", 15);
    CHECK(c.nerrors == 2);
    config_free(&c);
    if (keep_t) setenv("TERM", keep_t, 1); else unsetenv("TERM");
    if (keep_c) setenv("COLORTERM", keep_c, 1); else unsetenv("COLORTERM");
    if (keep_w) setenv("WT_SESSION", keep_w, 1); else unsetenv("WT_SESSION");
    if (keep_n) setenv("NO_COLOR", keep_n, 1);
    free(keep_t);
    free(keep_c);
    free(keep_w);
    free(keep_n);
}

/* The palette's Colors: auto shows what the terminal was found to show; each choice changes the
 * colours at once and is saved like the other settings. */
static void test_palette_colors(void)
{
    char dir[64], cfg[200], path[260], err[200], lab[64], *old = getenv("XDG_CONFIG_HOME");
    char *keep_t = getenv("TERM") ? xstrdup(getenv("TERM")) : NULL;
    char *keep_c = getenv("COLORTERM") ? xstrdup(getenv("COLORTERM")) : NULL;
    char saved[200] = "";
    Editor e;
    Doc *a;
    const char *t;
    size_t i;
    int idx = -1;

    if (old)
        snprintf(saved, sizeof saved, "%s", old);
    setenv("TERM", "xterm", 1);
    unsetenv("COLORTERM");
    CHECK(make_dir(dir, sizeof dir) != NULL);
    put_file(dir, "a.txt", "x\n");
    snprintf(cfg, sizeof cfg, "%s/cfg", dir);
    setenv("XDG_CONFIG_HOME", cfg, 1);
    editor_init(&e);
    editor_load_config(&e);
    snprintf(path, sizeof path, "%s/a.txt", dir);
    a = editor_open_doc(&e, path, err, sizeof err);
    CHECK(a != NULL);
    editor_activate(&e, a);
    for (i = 0; i < ncommands; i++)
        if (strcmp(commands[i].id, "colors") == 0)
            idx = (int)i;
    CHECK(idx >= 0);
    CHECK(e.colors_mode == -1 && e.color_depth == COLOR_256);
    CHECK(command_state(&e, idx, lab, sizeof lab) == 2 && strcmp(lab, "auto (256)") == 0);
    editor_cmd_colors(&e);
    CHECK(e.colors_mode == COLOR_TRUE && e.color_depth == COLOR_TRUE);
    command_state(&e, idx, lab, sizeof lab);
    CHECK(strcmp(lab, "truecolor") == 0);
    editor_cmd_colors(&e);
    CHECK(e.color_depth == COLOR_256);
    editor_cmd_colors(&e);
    CHECK(e.color_depth == COLOR_16);
    editor_cmd_colors(&e);
    CHECK(e.color_depth == COLOR_NONE);
    command_state(&e, idx, lab, sizeof lab);
    CHECK(strcmp(lab, "none") == 0);
    editor_cmd_colors(&e);
    CHECK(e.colors_mode == -1 && e.color_depth == COLOR_256);
    editor_cmd_colors(&e);                      /* truecolor, then saved for the user */
    CHECK(e.glyphs == GLYPHS_UNICODE);
    editor_cmd_glyphs(&e);
    CHECK(e.glyphs == GLYPHS_WINDOWS);
    editor_cmd_glyphs(&e);                      /* ascii, saved with it */
    CHECK(e.glyphs == GLYPHS_ASCII);
    editor_cmd_save_settings(&e);
    CHECK(e.picker.on && e.picker_kind == PK_SAVECFG);
    for (i = 0; i < e.ndests; i++)
        if (e.dests[i].kind == DEST_USER)
            break;
    CHECK(i < e.ndests);
    picker_select_item(&e.picker, i);
    kkey(&e, KEY_ENTER, 0);
    snprintf(path, sizeof path, "%s/xted/config", cfg);
    t = slurp_file(path);
    CHECK(t && strstr(t, "colors = truecolor") != NULL && strstr(t, "glyphs = ascii") != NULL);
    editor_free(&e);
    /* the next session starts with it */
    editor_init(&e);
    editor_load_config(&e);
    CHECK(e.colors_mode == COLOR_TRUE && e.color_depth == COLOR_TRUE && e.glyphs == GLYPHS_ASCII);
    editor_free(&e);
    if (saved[0])
        setenv("XDG_CONFIG_HOME", saved, 1);
    else
        unsetenv("XDG_CONFIG_HOME");
    if (keep_t) setenv("TERM", keep_t, 1); else unsetenv("TERM");
    if (keep_c) setenv("COLORTERM", keep_c, 1); else unsetenv("COLORTERM");
    free(keep_t);
    free(keep_c);
    rm_rf(dir);
}

/* YAML: the keys (also of list items, quoted, with dashes; not a URL), comments only after a
 * blank, anchors, aliases and tags, block scalars (key: | and >) as long as their lines are
 * indented beyond the key, and the keys in the outline. */
static void test_yaml(void)
{
    Syntax *y = lang("yaml");
    uint16_t st = 0;
    Buffer b;
    size_t yy = 0, x = 0;
    static const char *text = "name: app\njobs:\n  build:\n    runs-on: ubuntu\n    steps:\n      - run: |\n"
                              "          make\n          make test\n      - uses: x\n";

    CHECK(y != NULL);
    if (!y)
        return;
    CLS(y, st, "name: app", "vvvvo....");
    CLS(y, st, "  - max-age: 3", "..o.vvvvvvvo.n");
    CLS(y, st, "\"a b\": 1", "vvvvvo.n");
    CLS(y, st, "http://x.y/a#b", "....o.........");
    CLS(y, st, "url: http://x#y # c", "vvvo.....o......ccc");
    CLS(y, st, "base: &b", "vvvvo.TT");
    CLS(y, st, "<<: *b", "vvo.TT");
    CLS(y, st, "n: !!int 3", "vo.TTTTT.n");
    CLS(y, st, "on: true", "vvo.CCCC");
    /* a block scalar: its lines while they are indented beyond the key */
    CLS(y, st, "script: |", "vvvvvvo.o");
    CHECK(st != 0);
    CLS(y, st, "  echo: not a key", "sssssssssssssssss");
    CLS(y, st, "", "");
    CLS(y, st, "  # not a comment", "sssssssssssssssss");
    CLS(y, st, "next: 1", "vvvvo.n");
    CHECK(st == 0);
    CLS(y, st, "  - run: >-", "..o.vvvo.o.");
    CLS(y, st, "      text", "ssssssssss");
    CLS(y, st, "    other: 2", "....vvvvvo.n");
    CLS(y, st, "- |", "o.o");
    CLS(y, st, "  item", "ssssss");
    CLS(y, st, "- next", "o.....");
    CLS(y, st, "say: a | b", "vvvo...o..");
    CLS(y, st, "    runs-on: ubuntu-latest", "....vvvvvvvo..............");
    CHECK(st == 0);
    syn_free(y);
    buf_init(&b);
    b.path = xstrdup("ci.yml");
    buf_insert_text(&b, &yy, &x, text, strlen(text));
    {
        Doc d;
        SymCache *c = sym_cache_new();

        memset(&d, 0, sizeof d);
        d.buf = b;
        d.syn = syn_detect(&d.buf);
        out_text[0] = '\0';
        sym_outline(c, &d, out_cb, NULL);
        CHECK(strstr(out_text, "name") && strstr(out_text, "jobs") && strstr(out_text, "runs-on") &&
              strstr(out_text, "steps") && strstr(out_text, "uses"));
        syn_free(d.syn);
        sym_cache_free(c);
    }
    buf_free(&b);
}

/* JSON: keys and values coloured apart; the check (comments allowed, a trailing comma only where
 * the files accept it) with the line and column of the first error; formatting and compacting,
 * refused with comments; the commands, the message after saving, the keys in Ctrl+T. */
static void test_json(void)
{
    static const struct { const char *text; int code; size_t line, col; } bad[] = {
        { "{\"a\": 1,}", JE_TRAIL, 0, 7 }, { "{\"a\" 1}", JE_COLON, 0, 5 }, { "{\"a\": 1 \"b\": 2}", JE_COMMA, 0, 8 },
        { "{a: 1}", JE_KEY, 0, 1 }, { "[1, 2", JE_END, 0, 5 }, { "{\"a\": \"x}", JE_STRING, 0, 6 },
        { "[\"\\q\"]", JE_ESCAPE, 0, 2 }, { "[01]", JE_NUMBER, 0, 1 }, { "[1.]", JE_NUMBER, 0, 1 },
        { "{}\n[]", JE_EXTRA, 1, 0 }, { "[tru]", JE_CHAR, 0, 1 }, { "{\n  \"a\": 1\n  \"b\": 2\n}", JE_COMMA, 2, 2 },
        { "/* x", JE_COMMENT, 0, 0 }, { "", JE_END, 0, 0 },
    };
    static const char *good[] = { "{}", "[]", "0", "-1.5e+3", "\"\\u00e8\"", "{\"a\":[1,{\"b\":null}]}",
                                  "// c\n{\"a\": /* x */ 1}" };
    JsonErr je;
    SBuf o = SBUF_INIT;
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int r = json_check(bad[i].text, strlen(bad[i].text), 0, &je);

        if (r == 0 || je.code != bad[i].code || je.line != bad[i].line || je.col != bad[i].col)
            fprintf(stderr, "json %s: %d %d %zu:%zu\n", bad[i].text, r, je.code, je.line, je.col);
        CHECK(r < 0 && je.code == bad[i].code && je.line == bad[i].line && je.col == bad[i].col);
    }
    for (i = 0; i < sizeof good / sizeof good[0]; i++)
        CHECK(json_check(good[i], strlen(good[i]), 0, &je) == 0);
    CHECK(json_check("{\"a\": [1, 2,],}", 15, 1, &je) == 0);  /* lenient: JSONC */
    CHECK(json_format("{\"a\":[1,2],\"b\":{},\"c\":{\"d\":\"x y\"}}", 34, 0, "  ", &o, &je) == 0);
    sbuf_add(&o, "", 1);
    CHECK(strcmp(o.p, "{\n  \"a\": [\n    1,\n    2\n  ],\n  \"b\": {},\n  \"c\": {\n    \"d\": \"x y\"\n  }\n}") == 0);
    o.len = 0;
    CHECK(json_format("{ \"a\" : [ 1 , 2 ] }", 19, 0, NULL, &o, &je) == 0);
    sbuf_add(&o, "", 1);
    CHECK(strcmp(o.p, "{\"a\":[1,2]}") == 0);
    o.len = 0;
    CHECK(json_format("{\"a\": 1 // c\n}", 14, 0, "  ", &o, &je) == 1 && o.len == 0);
    CHECK(json_format("{\"a\": 1,}", 9, 1, "  ", &o, &je) == 0);  /* a trailing comma goes */
    sbuf_add(&o, "", 1);
    CHECK(strcmp(o.p, "{\n  \"a\": 1\n}") == 0);
    sbuf_free(&o);

    /* the editor's commands */
    {
        char dir[64], path[256], err[128];
        Editor e;
        Doc *d;
        size_t k;
        int idx = -1;

        CHECK(make_dir(dir, sizeof dir) != NULL);
        put_file(dir, "a.json", "{\"name\": \"x\", \"list\": [1, 2], \"deep\": {\"k\": true}}\n");
        snprintf(path, sizeof path, "%s/a.json", dir);
        editor_init(&e);
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d != NULL);
        editor_activate(&e, d);
        d->expand_tab = 1;
        d->indent_w = 2;
        editor_cmd_json_check(&e);
        CHECK(strcmp(e.msg, "JSON: valid") == 0);
        editor_cmd_json_format(&e);
        CHECK(e.buf->nlines == 10 && line_is(e.buf, 1, "  \"name\": \"x\","));
        out_text[0] = '\0';
        {
            SymCache *c = sym_cache_new();

            sym_outline(c, d, out_cb, NULL);
            CHECK(strstr(out_text, "name") && strstr(out_text, "list") && strstr(out_text, "deep") &&
                  strstr(out_text, "k"));
            sym_cache_free(c);
        }
        editor_cmd_json_compact(&e);
        CHECK(e.buf->nlines == 1 && line_is(e.buf, 0, "{\"name\":\"x\",\"list\":[1,2],\"deep\":{\"k\":true}}"));
        edit_undo(&e);
        CHECK(e.buf->nlines == 10);
        edit_undo(&e);
        CHECK(e.buf->nlines == 1 && line_is(e.buf, 0, "{\"name\": \"x\", \"list\": [1, 2], \"deep\": {\"k\": true}}"));
        /* an error: the cursor goes there; saving says it */
        e.cy = 0;
        e.cx = e.buf->lines[0].len - 1;
        for (k = 0; k < 1; k++)
            kkey(&e, ',', 0);
        editor_cmd_json_check(&e);
        CHECK(strstr(e.msg, "line 1, column ") && strstr(e.msg, "comma before the closing bracket") && e.cy == 0);
        for (i = 0; i < ncommands; i++)
            if (strcmp(commands[i].id, "save") == 0)
                idx = (int)i;
        CHECK(idx >= 0);
        commands[idx].fn(&e);
        CHECK(strstr(e.msg, "JSON: line 1") != NULL);
        editor_cmd_json_format(&e);         /* refused: the text stays */
        CHECK(e.buf->nlines == 1);
        editor_free(&e);
        /* comments: checked, not rewritten */
        put_file(dir, "b.jsonc", "{\n  // c\n  \"a\": 1,\n}\n");
        snprintf(path, sizeof path, "%s/b.jsonc", dir);
        editor_init(&e);
        d = editor_open_doc(&e, path, err, sizeof err);
        CHECK(d != NULL);
        editor_activate(&e, d);
        editor_cmd_json_check(&e);
        CHECK(strcmp(e.msg, "JSON: valid") == 0);
        editor_cmd_json_format(&e);
        CHECK(strstr(e.msg, "comments") && e.buf->nlines == 4 && line_is(e.buf, 1, "  // c"));
        editor_free(&e);
        rm_rf(dir);
    }
}

/* The rendered Markdown: the text, the style of its bytes and the source line of each line. */
static void test_md_render(void)
{
    static const char *src =
        "# Title\n"                                   /* 0 */
        "\n"
        "Text with **bold**, *it*, `code`, [link](http://x.y) and a_b_c.\n"  /* 2 */
        "\n"
        "- one\n"                                     /* 4 */
        "  - two\n"
        "- [x] done\n"
        "\n"
        "> quote\n"                                   /* 8 */
        "\n"
        "```c\n"                                      /* 10 */
        "int x;\n"
        "```\n"
        "\n"
        "| A | B |\n"                                 /* 14 */
        "|---|--:|\n"
        "| xy | 1 |\n"
        "\n"
        "Sub\n"                                       /* 18 */
        "---\n"
        "\n"
        "***\n";                                      /* 21 */
    Buffer b, out;
    MdPreview m;
    size_t y = 0, x = 0, i;
    const uint8_t *st;
    const char *t;

    buf_init(&b);
    buf_init(&out);
    buf_insert_text(&b, &y, &x, src, strlen(src));
    memset(&m, 0, sizeof m);
    md_build(&m, &b, &out, 40, 0);
    CHECK(m.n == out.nlines);
    CHECK(line_is(&out, 0, "Title"));
    st = md_styles(&m, 0);
    CHECK(st && (st[0] & MD_CLS) == SC_HEADING && (st[0] & MD_BOLD));
    CHECK(line_is(&out, 1, "\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90"));
    /* the paragraph: the marks gone, the styles on, wrapped at 40 columns */
    CHECK(line_is(&out, 3, "Text with bold, it, code, link"));
    st = md_styles(&m, 3);
    t = out.lines[3].s;
    CHECK((st[10] & MD_BOLD) && !(st[9] & MD_BOLD));                    /* bold */
    CHECK(st[16] & MD_ITALIC);                                           /* it */
    CHECK((st[20] & MD_CLS) == SC_CODE);                                 /* code */
    CHECK((st[26] & MD_CLS) == SC_LINK && (st[26] & MD_UNDER) && t[26] == 'l');
    CHECK(line_is(&out, 4, "(http://x.y) and a_b_c."));
    CHECK(m.srcline[3] == 2 && m.srcline[4] == 2);
    /* lists, nested, a task */
    CHECK(line_is(&out, 6, "\xe2\x80\xa2 one") && line_is(&out, 7, "  \xe2\x80\xa2 two") &&
          line_is(&out, 8, "\xe2\x98\x91 done"));
    CHECK(m.srcline[7] == 5);
    CHECK(line_is(&out, 10, "\xe2\x94\x82 quote") && (md_styles(&m, 10)[0] & MD_CLS) == SC_COMMENT);
    /* the code block: its background over the whole width, the C keyword coloured */
    for (i = 0; i < out.nlines && !line_is(&out, i, " int x;"); i++)
        if (out.lines[i].len >= 7 && memcmp(out.lines[i].s, " int x;", 7) == 0)
            break;
    CHECK(i < out.nlines && out.lines[i].len == 40);
    st = md_styles(&m, i);
    CHECK((st[0] & MD_CODEBG) && (st[39] & MD_CODEBG) && (st[1] & MD_CLS) == SC_TYPE && m.srcline[i] == 11);
    /* the table: columns aligned, the second to the right */
    for (i = 0; i < out.nlines; i++)
        if (out.lines[i].len > 3 && memcmp(out.lines[i].s, " A ", 3) == 0)
            break;
    CHECK(i + 3 < out.nlines);
    CHECK(line_is(&out, i, " A  \xe2\x94\x82 B ") && line_is(&out, i + 2, " xy \xe2\x94\x82 1 "));
    CHECK((md_styles(&m, i)[1] & MD_BOLD) && m.srcline[i + 2] == 16);
    /* a setext heading and a rule */
    for (i = 0; i < out.nlines && !line_is(&out, i, "Sub"); i++)
        ;
    CHECK(i < out.nlines && (md_styles(&m, i)[0] & MD_CLS) == SC_HEADING);
    CHECK(md_line_for(&m, 18) == i);
    CHECK(out.lines[out.nlines - 1].len == 40 * 3);          /* ─ is 3 bytes */
    /* ascii */
    md_build(&m, &b, &out, 40, 1);
    CHECK(line_is(&out, 1, "=====") && line_is(&out, 6, "* one") && line_is(&out, 8, "[x] done") &&
          line_is(&out, 10, "| quote"));
    md_free(&m);
    buf_free(&b);
    buf_free(&out);
}

/* The preview pane: Alt+U opens it beside the document, the source keeps the focus, the preview
 * follows the text and the cursor, Alt+U again closes it, closing the source takes it away. */
static void test_md_preview_pane(void)
{
    char dir[64], path[256], err[128];
    Editor e;
    Doc *d;
    Win *w[8];
    size_t n, k, i;
    Doc *pv = NULL;
    SBuf big = SBUF_INIT;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    sbuf_adds(&big, "# Doc\n\n");
    for (i = 0; i < 80; i++)
        sbuf_addf(&big, "Paragraph %zu.\n\n", i);
    sbuf_adds(&big, "## End\n");
    sbuf_add(&big, "", 1);
    put_file(dir, "r.md", big.p);
    sbuf_free(&big);
    put_file(dir, "a.txt", "x\n");
    editor_init(&e);
    e.rows = 30;
    e.cols = 120;
    snprintf(path, sizeof path, "%s/a.txt", dir);
    d = editor_open_doc(&e, path, err, sizeof err);
    editor_activate(&e, d);
    editor_cmd_md_preview(&e);
    CHECK(strstr(e.msg, "Markdown") && layout_count(&e.layout) == 1);
    snprintf(path, sizeof path, "%s/r.md", dir);
    d = editor_open_doc(&e, path, err, sizeof err);
    CHECK(d != NULL);
    editor_activate(&e, d);
    kkey(&e, 'u', MOD_ALT);
    CHECK(layout_count(&e.layout) == 2 && e.doc == d);
    n = layout_wins(&e.layout, w, 8);
    for (k = 0; k < n; k++)
        if (w[k]->doc->md)
            pv = w[k]->doc;
    CHECK(pv && pv->readonly && pv->scratch && strstr(pv->title, "r.md"));
    editor_render_prepare(&e);
    CHECK(pv->buf.nlines > 100 && line_is(&pv->buf, 0, "Doc"));
    /* the cursor at the end: the preview shows the end */
    e.cy = d->buf.nlines - 1;
    editor_render_prepare(&e);
    for (k = 0; k < n; k++)
        if (w[k]->doc == pv)
            CHECK(w[k]->view.rowoff > 100);
    /* an edit shows at once */
    e.cy = 0;
    e.cx = d->buf.lines[0].len;
    kkey(&e, 'X', 0);
    editor_render_prepare(&e);
    CHECK(line_is(&pv->buf, 0, "DocX"));
    /* Alt+U again closes it, from the source */
    kkey(&e, 'u', MOD_ALT);
    CHECK(layout_count(&e.layout) == 1 && e.doc == d);
    for (k = 0; k < e.ndocs; k++)
        CHECK(!e.docs[k]->md);                  /* the preview is no buffer of its own */
    kkey(&e, 'u', MOD_ALT);
    CHECK(layout_count(&e.layout) == 2);
    /* closing the source (without saving) takes the preview and its pane away */
    edit_undo(&e);
    editor_cmd_close_buffer(&e);
    CHECK(layout_count(&e.layout) == 1);
    for (k = 0; k < e.ndocs; k++)
        CHECK(!e.docs[k]->md);
    editor_free(&e);
    rm_rf(dir);
}

static void test_config_merge(void)
{
    Config user, proj;
    LangCfg l, pl;

    config_init(&user);
    config_init(&proj);
    {
        const char *u = "expand_tab = off\ntab_width = 4\n[Python]\nindent_width = 2\n";
        const char *p = "expand_tab = on\n[Python]\nautopair = off\n";

        config_parse(&user, u, strlen(u));
        config_parse(&proj, p, strlen(p));
    }
    l = config_lang(&user, "Python");
    pl = config_lang(&proj, "Python");
    CHECK((pl.set & CFG_EXPAND_TAB) && (pl.set & CFG_AUTOPAIR) && !(pl.set & CFG_TAB_WIDTH));
    config_merge(&l, &pl);
    CHECK(l.expand_tab == 1 && l.autopair == 0);                 /* the project's values */
    CHECK(l.tab_width == 4 && l.indent_width == 2);              /* the user's, untouched */
    CHECK((l.set & (CFG_EXPAND_TAB | CFG_AUTOPAIR | CFG_TAB_WIDTH | CFG_INDENT_WIDTH)) ==
          (CFG_EXPAND_TAB | CFG_AUTOPAIR | CFG_TAB_WIDTH | CFG_INDENT_WIDTH));
    config_free(&user);
    config_free(&proj);
}

static void test_scan_steps(void)
{
    char dir[64], path[256], err[128];
    size_t i, j, all = 0, got = 0, steps = 0;
    FScan *fs;
    Doc d;
    SymCache *c = sym_cache_new();
    Doc *docs[1];
    static Got g;
    size_t sep;
    int found = 0, busy_seen = 0;

    CHECK(make_dir(dir, sizeof dir) != NULL);
    for (i = 0; i < 30; i++) {
        snprintf(path, sizeof path, "%s/d%02zu", dir, i);
        CHECK(mkdir(path, 0700) == 0);
        for (j = 0; j < 40; j++) {
            char name[64];

            snprintf(name, sizeof name, "d%02zu/f%zu.txt", i, j);
            put_file(dir, name, "x\n");
        }
    }
    put_file(dir, "d29/Target.php",
             "<?php\nclass Target {\n    public function hello() {}\n}\n");
    put_file(dir, "composer.json", "{}\n");
    put_file(dir, "main.php", "<?php\nuse Target;\nTarget::\n");

    CHECK(files_scan(dir, count_file, &all) == all && all == 1203);
    fs = files_scan_begin(dir, NULL);
    while (!files_scan_step(fs, count_file, &got, 1))
        steps++;
    files_scan_free(fs);
    CHECK(got == all && steps > 10);

    memset(&d, 0, sizeof d);
    snprintf(path, sizeof path, "%s/main.php", dir);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    docs[0] = &d;
    sep = sym_member_at(d.syn, &d.buf, 2, d.buf.lines[2].len);
    /* Target.php is only found by walking the project (no Composer here) */
    for (i = 0; i < 2000 && !found; i++) {
        sym_budget(c, 1);
        memset(&g, 0, sizeof g);
        sym_members(c, docs, 1, &d, 2, sep, got_cb, &g);
        if (sym_busy(c))
            busy_seen = 1;
        found = has_sym(&g, "hello");
    }
    CHECK(found && i < 2000);
    (void)busy_seen;
    sym_budget(c, 0);
    CHECK(!sym_busy(c));
    sym_cache_free(c);
    doc_release(&d);
}

static void test_laravel(void)
{
    char dir[64], path[256], err[128], t[64], cwd[512];
    Doc d;
    SymCache *c = sym_cache_new();
    Doc *docs[1];
    static Got g;
    size_t y, sep;
    static const char *const sub[] = { "config", "vendor", "vendor/composer", "vendor/lv",
                                       "vendor/lv/src", "vendor/lv/src/Support",
                                       "vendor/lv/src/Support/Facades", "vendor/decoy" };
    size_t i;

    CHECK(getcwd(cwd, sizeof cwd) != NULL);
    CHECK(make_dir(dir, sizeof dir) != NULL);
    for (i = 0; i < sizeof sub / sizeof sub[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, sub[i]);
        CHECK(mkdir(path, 0700) == 0);
    }
    put_file(dir, "config/app.php",
             "<?php\nreturn [\n    'aliases' => [\n"
             "        'URL' => Lv\\Support\\Facades\\URL::class,\n    ],\n];\n");
    put_file(dir, "vendor/composer/autoload_psr4.php",
             "<?php\n$vendorDir = dirname(dirname(__FILE__));\nreturn array(\n"
             "    'Lv\\Support\\' => array($vendorDir . '/nothing'),\n"
             "    'Lv\\' => array($vendorDir . '/lv/src'),\n);\n");
    put_file(dir, "vendor/lv/src/Support/Facades/URL.php",
             "<?php\nnamespace Lv\\Support\\Facades;\n/**\n"
             " * @method static string to(string $path, $extra = [])\n"
             " * @method static \\Lv\\Other|null make()\n"
             " * @property string $tag\n */\nclass URL extends Facade {}\n");
    put_file(dir, "vendor/lv/src/Store.php",
             "<?php\nnamespace Lv;\nclass Store {\n    public function remember($k) {}\n"
             "    protected function secret() {}\n}\n");
    put_file(dir, "vendor/lv/src/Support/Facades/Cache.php",
             "<?php\nnamespace Lv\\Support\\Facades;\n/**\n * @see \\Lv\\Store\n */\n"
             "class Cache extends Facade {}\n");
    put_file(dir, "vendor/decoy/URL.php", "<?php\nclass URL { public function wrong() {} }\n");
    put_file(dir, "main.php",
             "<?php\nuse Lv\\Support\\Facades\\URL as U;\nuse Lv\\Support\\Facades\\Cache;\n"
             "URL::\nU::\nCache::\n");
    /* the working directory is not the project: the root comes from the file's location */
    memset(&d, 0, sizeof d);
    snprintf(path, sizeof path, "%s/main.php", dir);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    docs[0] = &d;
    for (y = 3; y <= 3; y++) {                  /* URL:: through the alias in config/app.php */
        sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
        memset(&g, 0, sizeof g);
        CHECK(sym_members(c, docs, 1, &d, y, sep, got_cb, &g) == 1);
        CHECK(has_sym(&g, "to") && has_sym(&g, "make") && has_sym(&g, "tag") &&
              !has_sym(&g, "wrong"));
        CHECK(strstr(g.got, "[to 2 (string $path, $extra = []) string]") != NULL);
        CHECK(sym_type_before(c, docs, 1, &d, y, sep, t, sizeof t) && strcmp(t, "URL") == 0);
    }
    /* a facade documented only with @see forwards to the public methods of that class */
    y = 5;
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &d, y, sep, got_cb, &g) == 1);
    CHECK(has_sym(&g, "remember") && !has_sym(&g, "secret"));
    CHECK(chdir(cwd) == 0);
    sym_cache_free(c);
    doc_release(&d);
}

static void test_symbols(void)
{
    char dir[64], path[128], err[128];
    Doc d, dp;
    SymIndex *x;
    SymCache *c = sym_cache_new();
    Doc *docs[1];
    static Got g;
    char t[64];
    size_t y, sep;
    const Sym *s;

    test_symbol_patterns();
    CHECK(make_dir(dir, sizeof dir) != NULL);
    snprintf(path, sizeof path, "%s/lib", dir);
    CHECK(mkdir(path, 0700) == 0);
    put_file(dir, "lib/helper.php",
             "<?php\n"
             "class Helper {\n"
             "    public $level = 1;\n"
             "    public function assist(string $what, int $n = 0): string { return $what; }\n"
             "    public function chain(): Helper { return $this; }\n"
             "    public function self(): static { return $this; }\n"
             "}\n"
             "function helper_fn(int $a): Helper { return new Helper(); }\n");
    put_file(dir, "main.php",
             "<?php\n"
             "require_once __DIR__ . '/lib/helper.php';\n"
             "\n"
             "abstract class Base\n"
             "{\n"
             "    protected int $id;\n"
             "    public function getId(): int { return $this->id; }\n"
             "}\n"
             "\n"
             "final class Thing extends Base\n"
             "{\n"
             "    const MAX = 3;\n"
             "    private Helper $helper;\n"
             "    public function __construct(Helper $h)\n"
             "    {\n"
             "        $this->helper = $h;\n"
             "        $this->extra = new Helper();\n"
             "    }\n"
             "\n"
             "    public function run(): self\n"
             "    {\n"
             "        $x = new Helper();\n"
             "        $this->\n"
             "        $x->\n"
             "        $x->chain()->self()->\n"
             "        $this->helper->\n"
             "        helper_fn(1)->\n"
             "        $x->assist('a')->\n"
             "        return $this;\n"
             "    }\n"
             "}\n"
             "class Other { public function only() {} }\n"
             "$this->\n");
    snprintf(path, sizeof path, "%s/main.php", dir);
    memset(&d, 0, sizeof d);
    CHECK(doc_load(&d, path, err, sizeof err) == 0);
    docs[0] = &d;
    CHECK(strcmp(syn_name(d.syn), "PHP") == 0);

    /* the index: classes, members, bases, types, includes */
    x = sym_build(&d.buf, d.syn);
    s = find_sym(x, "Thing", RULE_CLASS);
    CHECK(s && strcmp(s->ret, "Base") == 0 && s->y0 == 9 && s->y1 == 30);
    s = find_sym(x, "getId", RULE_METHOD);
    CHECK(s && s->owner >= 0 && strcmp(x->v[s->owner].name, "Base") == 0 && strcmp(s->ret, "int") == 0);
    s = find_sym(x, "helper", RULE_PROP);
    CHECK(s && strcmp(s->ret, "Helper") == 0);
    s = find_sym(x, "extra", RULE_PROP);                    /* made by an assignment in a method */
    CHECK(s && strcmp(s->ret, "Helper") == 0 && strcmp(x->v[s->owner].name, "Thing") == 0);
    s = find_sym(x, "__construct", RULE_METHOD);
    CHECK(s && strcmp(s->sig, "Helper $h") == 0);
    CHECK(find_sym(x, "MAX", RULE_CONST) != NULL);
    CHECK(x->ninc >= 1 && strcmp(x->inc[0], "/lib/helper.php") == 0);
    sym_index_free(x);

    /* $this-> : the members of the class around the cursor, with the inherited ones */
    y = 22;
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    CHECK(sep == 13);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &d, y, sep, got_cb, &g) == 1);
    CHECK(has_sym(&g, "helper") && has_sym(&g, "run") && has_sym(&g, "getId") &&
          has_sym(&g, "MAX") && has_sym(&g, "extra") && !has_sym(&g, "only"));
    /* an object created above, from a file that is included, not open */
    y = 23;
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &d, y, sep, got_cb, &g) == 1);
    CHECK(has_sym(&g, "assist") && has_sym(&g, "level") && !has_sym(&g, "run"));
    CHECK(strstr(g.got, "[assist 2 (string $what, int $n = 0) string]") != NULL);
    /* chains: methods returning their class, "static", a typed property, a function result */
    y = 24;
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    CHECK(sym_type_before(c, docs, 1, &d, y, sep, t, sizeof t) && strcmp(t, "Helper") == 0);
    y = 25;
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    CHECK(sym_type_before(c, docs, 1, &d, y, sep, t, sizeof t) && strcmp(t, "Helper") == 0);
    y = 26;
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    CHECK(sym_type_before(c, docs, 1, &d, y, sep, t, sizeof t) && strcmp(t, "Helper") == 0);
    /* a chain that ends in a scalar: no members, and the type is not a class we know */
    y = 27;
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    CHECK(!sym_type_before(c, docs, 1, &d, y, sep, t, sizeof t) || strcmp(t, "string") == 0);
    /* outside any class: unknown, so every member of every class is offered */
    y = 32;
    sep = sym_member_at(d.syn, &d.buf, y, d.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &d, y, sep, got_cb, &g) == 0);
    CHECK(has_sym(&g, "only") && has_sym(&g, "run"));
    /* not an operator: a number, a comment */
    CHECK(sym_member_at(d.syn, &d.buf, 0, 5) == (size_t)-1);

    /* globals: this file, the included one, the standard library */
    memset(&g, 0, sizeof g);
    sym_globals(c, docs, 1, &d, got_cb, &g);
    CHECK(has_sym(&g, "Thing") && has_sym(&g, "helper_fn") && has_sym(&g, "Helper") &&
          has_sym(&g, "strlen") && has_sym(&g, "DateTime") && has_sym(&g, "array_map"));

    /* the popup after "$this->": members only, with their signature; a new method is seen */
    {
        Complete cp;
        size_t i;
        int found = 0;

        memset(&cp, 0, sizeof cp);
        y = 22;
        CHECK(complete_open(&cp, docs, 1, &d, y, d.buf.lines[y].len));
        for (i = 0; i < cp.n; i++)
            if (strcmp(cp.v[i].text, "getId") == 0 && cp.v[i].kind == CAND_METHOD &&
                cp.v[i].sig && strcmp(cp.v[i].sig, "getId(): int") == 0)
                found = 1;
        CHECK(found);
        complete_close(&cp);
        buf_insert(&d.buf, 22, d.buf.lines[22].len, "ge", 2);
        CHECK(complete_open(&cp, docs, 1, &d, 22, d.buf.lines[22].len));
        CHECK(cp.n >= 1 && strcmp(cp.v[0].text, "getId") == 0);
        complete_free(&cp);
    }
    sym_cache_free(c);
    doc_release(&d);

    /* Python: indentation scopes, self, inheritance, a function that returns an instance */
    c = sym_cache_new();
    put_file(dir, "a.py",
             "class Base:\n"
             "    def ping(self, n: int) -> int:\n"
             "        return n\n"
             "\n"
             "class Dog(Base):\n"
             "    legs: int = 4\n"
             "    def __init__(self, name):\n"
             "        self.name = name\n"
             "        self.owner = Person()\n"
             "    def bark(self, times=1) -> 'Dog':\n"
             "        self.\n"
             "\n"
             "class Person:\n"
             "    def pet(self) -> Dog: pass\n"
             "\n"
             "def make() -> Dog:\n"
             "    d = make()\n"
             "    d.\n"
             "    d.owner.pet().\n"
             "    s = 'abc'\n"
             "    s.\n");
    snprintf(path, sizeof path, "%s/a.py", dir);
    memset(&dp, 0, sizeof dp);
    CHECK(doc_load(&dp, path, err, sizeof err) == 0);
    docs[0] = &dp;
    x = sym_build(&dp.buf, dp.syn);
    s = find_sym(x, "Dog", RULE_CLASS);
    CHECK(s && strcmp(s->ret, "Base") == 0 && s->y1 == 10);
    CHECK(find_sym(x, "name", RULE_PROP) && find_sym(x, "owner", RULE_PROP));
    s = find_sym(x, "ping", RULE_METHOD);
    CHECK(s && strcmp(s->sig, "n: int") == 0 && strcmp(s->ret, "int") == 0);
    CHECK(find_sym(x, "make", RULE_FUNC) != NULL && find_sym(x, "pet", RULE_METHOD) != NULL);
    sym_index_free(x);
    y = 10;
    sep = sym_member_at(dp.syn, &dp.buf, y, dp.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &dp, y, sep, got_cb, &g) == 1);
    CHECK(has_sym(&g, "bark") && has_sym(&g, "ping") && has_sym(&g, "legs") && has_sym(&g, "name"));
    y = 17;
    sep = sym_member_at(dp.syn, &dp.buf, y, dp.buf.lines[y].len);
    CHECK(sym_type_before(c, docs, 1, &dp, y, sep, t, sizeof t) && strcmp(t, "Dog") == 0);
    y = 18;                                     /* d.owner.pet(). */
    sep = sym_member_at(dp.syn, &dp.buf, y, dp.buf.lines[y].len);
    CHECK(sym_type_before(c, docs, 1, &dp, y, sep, t, sizeof t) && strcmp(t, "Dog") == 0);
    y = 20;                                     /* a str: the standard library */
    sep = sym_member_at(dp.syn, &dp.buf, y, dp.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &dp, y, sep, got_cb, &g) == 0 || has_sym(&g, "upper"));
    memset(&g, 0, sizeof g);
    sym_globals(c, docs, 1, &dp, got_cb, &g);
    CHECK(has_sym(&g, "len") && has_sym(&g, "Dog") && has_sym(&g, "os") && has_sym(&g, "make"));
    sym_cache_free(c);
    doc_release(&dp);

    /* C and JavaScript: braces, a typedef of an anonymous struct, a class with this.x */
    c = sym_cache_new();
    put_file(dir, "a.c",
             "#include \"b.h\"\n"
             "typedef struct {\n"
             "    int w;\n"
             "    char *label;\n"
             "} Box;\n"
             "struct node { struct node *next; };\n"
             "static int area(const Box *b)\n"
             "{\n"
             "    int local = 1;\n"
             "    return b->w * local;\n"
             "}\n"
             "Box *make_box(int w, int h);\n"
             "#define BOX_MAX 10\n"
             "void f(void)\n"
             "{\n"
             "    Box *p = make_box(1, 2);\n"
             "    p->\n"
             "    struct tm t;\n"
             "    t.\n"
             "}\n");
    put_file(dir, "b.h", "int from_header(int x);\n");
    snprintf(path, sizeof path, "%s/a.c", dir);
    memset(&dp, 0, sizeof dp);
    CHECK(doc_load(&dp, path, err, sizeof err) == 0);
    docs[0] = &dp;
    x = sym_build(&dp.buf, dp.syn);
    s = find_sym(x, "Box", RULE_CLASS);
    CHECK(s != NULL);
    CHECK(find_sym(x, "w", RULE_PROP) && find_sym(x, "label", RULE_PROP));
    s = find_sym(x, "area", RULE_FUNC);
    CHECK(s && strcmp(s->sig, "const Box *b") == 0);
    CHECK(find_sym(x, "local", RULE_VAR) == NULL && find_sym(x, "make_box", RULE_FUNC) &&
          find_sym(x, "BOX_MAX", RULE_CONST) && find_sym(x, "next", RULE_PROP));
    CHECK(x->ninc == 1 && strcmp(x->inc[0], "b.h") == 0);
    sym_index_free(x);
    y = 16;
    sep = sym_member_at(dp.syn, &dp.buf, y, dp.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &dp, y, sep, got_cb, &g) == 1 && has_sym(&g, "w") &&
          has_sym(&g, "label"));
    y = 18;
    sep = sym_member_at(dp.syn, &dp.buf, y, dp.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &dp, y, sep, got_cb, &g) == 1 && has_sym(&g, "tm_year"));
    memset(&g, 0, sizeof g);
    sym_globals(c, docs, 1, &dp, got_cb, &g);
    CHECK(has_sym(&g, "printf") && has_sym(&g, "from_header") && has_sym(&g, "area"));
    sym_cache_free(c);
    doc_release(&dp);

    c = sym_cache_new();
    put_file(dir, "a.js",
             "import { util } from './u.js';\n"
             "class Animal {\n"
             "  static count = 0;\n"
             "  constructor(name) {\n"
             "    this.name = name;\n"
             "    this.tags = new Set();\n"
             "  }\n"
             "  speak(times) { return this.name; }\n"
             "}\n"
             "class Cat extends Animal {\n"
             "  purr() {\n"
             "    this.\n"
             "    this.tags.\n"
             "  }\n"
             "}\n"
             "const c = new Cat('x');\n"
             "c.\n"
             "[1, 2].map(x => x).\n"
             "document.getElementById('a').\n");
    put_file(dir, "u.js", "export function util(a) { return a; }\n");
    snprintf(path, sizeof path, "%s/a.js", dir);
    memset(&dp, 0, sizeof dp);
    CHECK(doc_load(&dp, path, err, sizeof err) == 0);
    docs[0] = &dp;
    y = 11;
    sep = sym_member_at(dp.syn, &dp.buf, y, dp.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &dp, y, sep, got_cb, &g) == 1);
    CHECK(has_sym(&g, "purr") && has_sym(&g, "speak") && has_sym(&g, "name") && has_sym(&g, "tags"));
    y = 12;
    sep = sym_member_at(dp.syn, &dp.buf, y, dp.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &dp, y, sep, got_cb, &g) == 1 && has_sym(&g, "add") &&
          has_sym(&g, "size"));
    y = 16;
    sep = sym_member_at(dp.syn, &dp.buf, y, dp.buf.lines[y].len);
    CHECK(sym_type_before(c, docs, 1, &dp, y, sep, t, sizeof t) && strcmp(t, "Cat") == 0);
    y = 18;
    sep = sym_member_at(dp.syn, &dp.buf, y, dp.buf.lines[y].len);
    memset(&g, 0, sizeof g);
    CHECK(sym_members(c, docs, 1, &dp, y, sep, got_cb, &g) == 1 && has_sym(&g, "querySelector"));
    memset(&g, 0, sizeof g);
    sym_globals(c, docs, 1, &dp, got_cb, &g);
    CHECK(has_sym(&g, "util") && has_sym(&g, "Animal") && has_sym(&g, "Math") &&
          has_sym(&g, "setTimeout"));
    sym_cache_free(c);
    doc_release(&dp);

    /* the standard library itself: every list unpacks, classes, members, bases */
    {
        const char *ids[] = { "php", "c", "cpp", "py", "js" };
        size_t i;

        for (i = 0; i < 5; i++) {
            StdLib *l = std_open(ids[i]);

            CHECK(l != NULL);
            std_close(l);
        }
        CHECK(std_open("nope") == NULL);
        {
            StdLib *l = std_open("php");
            size_t bl;
            const char *b;

            CHECK(std_class(l, "DateTime", 8) && !std_class(l, "NoSuchClass", 11));
            b = std_base(l, "InvalidArgumentException", 24, &bl);
            CHECK(b && bl == 14 && memcmp(b, "LogicException", 14) == 0);
            std_close(l);
        }
    }
    rm_rf(dir);
}

int main(void)
{
    sys_init();
    if (!sys_getcwd(start_dir, sizeof start_dir))
        start_dir[0] = '\0';
#ifdef _WIN32
    /* the folders of the tests are \tmp on the current drive: the drive of %TEMP%, a local one
     * (from a network drive the read-only attribute and the time limits of the scans fail) */
    {
        const char *t = getenv("TEMP");

        if (t && t[0])
            _chdir(t);
    }
    _mkdir("/tmp");
#endif
    setenv("LC_ALL", "C", 1);     /* the texts are checked in English whatever the environment says */
    test_utf8();
    test_edit();
    test_files();
    test_encodings();
    test_undo();
    test_undo_saved();
    test_ranges();
    test_clip();
    test_edit_cmds();
    test_autopair();
    test_indent();
    test_snippet_defs();
    test_syntax_lines();
    test_syntax_parse();
    test_mouse();
    test_mouse_borders();
    test_resize_mode();
    test_syntax_detect();
    test_syntax_langs();
    test_syntax_embed();
    test_syntax_detect_langs();
    test_dotenv();
    test_yaml();
    test_json();
    test_md_render();
    test_md_preview_pane();
    test_syntax_incremental();
    test_syntax_brackets();
    test_theme();
    test_fuzzy_picker();
    test_layout();
    test_layout_resize();
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
#ifndef _WIN32
    test_git();                     /* commands: not yet on Windows (port, phase 4) */
#endif
    test_tags();
    test_complete();
    test_symbols();
    test_call_body();
    test_params();
    test_outline();
    test_long_names();
    test_alias_static();
    test_laravel();
    test_scan_steps();
    test_app_dir();
    test_cfgsave();
    test_save_project();
    test_i18n();
    test_c_include();
    test_py_import();
    test_js_import();
    test_c_project_include();
    test_js_project_import();
    test_py_project_import();
    test_php_static_use();
    test_php_require_and_use_function();
    test_sym_cache_pressure();
    test_php_facades();
    test_go_rust();
    test_java();
    test_java_more();
    test_java_builds();
    test_java_settings_and_size();
    test_csharp();
    test_env_suggest();
    test_color_depth();
    test_palette_colors();
    test_config_merge();
    test_snippets();
    test_cursors();
    test_folding();
#ifndef _WIN32
    test_shell();
#else
    test_shell_win();
    (void)test_shell;
    (void)test_run;
    (void)test_filter;
    (void)test_git;
#endif
    test_errors_parse();
    test_ipc();
#ifndef _WIN32
    test_run();
    test_filter();
#endif
    if (failures) {
        fprintf(stderr, "%d test falliti\n", failures);
        return 1;
    }
    puts("tutti i test passati");
    return 0;
}
