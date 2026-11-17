#include "json.h"
#include "i18n.h"

#include <string.h>

#define JSON_DEPTH 512

typedef struct {
    const char *s;
    size_t n, i;
    int lenient, comments, depth;
    SBuf *o;                /* NULL: only check */
    const char *ind;        /* NULL: compact */
    JsonErr *e;
} P;

const char *json_err_text(int code)
{
    switch (code) {
    case JE_END: return N_("the text ends too soon");
    case JE_CHAR: return N_("unexpected character");
    case JE_STRING: return N_("string not closed");
    case JE_ESCAPE: return N_("invalid escape in a string");
    case JE_CONTROL: return N_("control character in a string");
    case JE_NUMBER: return N_("invalid number");
    case JE_KEY: return N_("a key in double quotes was expected");
    case JE_COLON: return N_("':' was expected after the key");
    case JE_COMMA: return N_("a comma is missing");
    case JE_TRAIL: return N_("comma before the closing bracket");
    case JE_EXTRA: return N_("text after the end of the value");
    case JE_DEPTH: return N_("nested too deep");
    case JE_COMMENT: return N_("comment not closed");
    default: return "";
    }
}

static int fail(P *p, int code, size_t at)
{
    size_t k;

    if (p->e->code)
        return -1;
    p->e->code = code;
    p->e->line = p->e->col = 0;
    for (k = 0; k < at && k < p->n; k++) {
        if (p->s[k] == '\n') {
            p->e->line++;
            p->e->col = 0;
        } else {
            p->e->col++;
        }
    }
    return -1;
}

static void put(P *p, const char *s, size_t n)
{
    if (p->o)
        sbuf_add(p->o, s, n);
}

static void newline(P *p, int depth)
{
    int k;

    if (!p->o || !p->ind)
        return;
    sbuf_adds(p->o, "\n");
    for (k = 0; k < depth; k++)
        sbuf_adds(p->o, p->ind);
}

/* Blanks and comments. */
static int skip(P *p)
{
    while (p->i < p->n) {
        char c = p->s[p->i];

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->i++;
        } else if (c == '/' && p->i + 1 < p->n && p->s[p->i + 1] == '/') {
            p->comments = 1;
            while (p->i < p->n && p->s[p->i] != '\n')
                p->i++;
        } else if (c == '/' && p->i + 1 < p->n && p->s[p->i + 1] == '*') {
            size_t at = p->i;

            p->comments = 1;
            for (p->i += 2; p->i + 1 < p->n && !(p->s[p->i] == '*' && p->s[p->i + 1] == '/'); p->i++)
                ;
            if (p->i + 1 >= p->n)
                return fail(p, JE_COMMENT, at);
            p->i += 2;
        } else {
            break;
        }
    }
    return 0;
}

static int hexd(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int string(P *p)
{
    size_t b = p->i;

    for (p->i++; p->i < p->n; p->i++) {
        unsigned char c = (unsigned char)p->s[p->i];

        if (c == '"') {
            p->i++;
            put(p, p->s + b, p->i - b);
            return 0;
        }
        if (c == '\n')
            break;
        if (c < 0x20)
            return fail(p, JE_CONTROL, p->i);
        if (c == '\\') {
            char x = p->i + 1 < p->n ? p->s[p->i + 1] : 0;

            if (x == 'u') {
                int k;

                for (k = 2; k < 6; k++)
                    if (p->i + (size_t)k >= p->n || !hexd(p->s[p->i + (size_t)k]))
                        return fail(p, JE_ESCAPE, p->i);
                p->i += 5;
            } else if (x && strchr("\"\\/bfnrt", x)) {
                p->i++;
            } else {
                return fail(p, JE_ESCAPE, p->i);
            }
        }
    }
    return fail(p, JE_STRING, b);
}

static int digits(P *p)
{
    size_t b = p->i;

    while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
        p->i++;
    return p->i > b;
}

static int number(P *p)
{
    size_t b = p->i;

    if (p->s[p->i] == '-')
        p->i++;
    if (p->i < p->n && p->s[p->i] == '0') {
        p->i++;
        if (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9')
            return fail(p, JE_NUMBER, b);   /* 01: no leading zeros */
    } else if (!digits(p))
        return fail(p, JE_NUMBER, b);
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++;
        if (!digits(p))
            return fail(p, JE_NUMBER, b);
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-'))
            p->i++;
        if (!digits(p))
            return fail(p, JE_NUMBER, b);
    }
    if (p->i < p->n && (p->s[p->i] == '.' || p->s[p->i] == '_' ||
                        (p->s[p->i] >= 'a' && p->s[p->i] <= 'z') || (p->s[p->i] >= 'A' && p->s[p->i] <= 'Z')))
        return fail(p, JE_NUMBER, b);
    put(p, p->s + b, p->i - b);
    return 0;
}

static int value(P *p);

/* An object or an array, from its opening bracket. */
static int container(P *p)
{
    char open = p->s[p->i], close = open == '{' ? '}' : ']';

    if (++p->depth > JSON_DEPTH)
        return fail(p, JE_DEPTH, p->i);
    put(p, &open, 1);
    p->i++;
    if (skip(p) < 0)
        return -1;
    if (p->i < p->n && p->s[p->i] == close) {
        p->i++;
        put(p, &close, 1);
        p->depth--;
        return 0;
    }
    for (;;) {
        if (skip(p) < 0)
            return -1;
        if (p->i >= p->n)
            return fail(p, JE_END, p->i);
        newline(p, p->depth);
        if (open == '{') {
            if (p->s[p->i] != '"')
                return fail(p, JE_KEY, p->i);
            if (string(p) < 0 || skip(p) < 0)
                return -1;
            if (p->i >= p->n || p->s[p->i] != ':')
                return fail(p, p->i >= p->n ? JE_END : JE_COLON, p->i);
            p->i++;
            put(p, p->ind ? ": " : ":", p->ind ? 2 : 1);
            if (skip(p) < 0)
                return -1;
        }
        if (value(p) < 0 || skip(p) < 0)
            return -1;
        if (p->i >= p->n)
            return fail(p, JE_END, p->i);
        if (p->s[p->i] == ',') {
            size_t comma = p->i;

            p->i++;
            if (skip(p) < 0)
                return -1;
            if (p->i < p->n && p->s[p->i] == close) {
                if (!p->lenient)
                    return fail(p, JE_TRAIL, comma);
                p->i++;
                newline(p, p->depth - 1);
                put(p, &close, 1);
                p->depth--;
                return 0;
            }
            put(p, ",", 1);
            continue;
        }
        if (p->s[p->i] == close) {
            p->i++;
            newline(p, p->depth - 1);
            put(p, &close, 1);
            p->depth--;
            return 0;
        }
        return fail(p, JE_COMMA, p->i);
    }
}

static int value(P *p)
{
    static const char *const lit[] = { "true", "false", "null" };
    size_t k;
    char c;

    if (p->i >= p->n)
        return fail(p, JE_END, p->i);
    c = p->s[p->i];
    if (c == '{' || c == '[')
        return container(p);
    if (c == '"')
        return string(p);
    if (c == '-' || (c >= '0' && c <= '9'))
        return number(p);
    for (k = 0; k < 3; k++) {
        size_t l = strlen(lit[k]);

        if (p->i + l <= p->n && memcmp(p->s + p->i, lit[k], l) == 0) {
            put(p, lit[k], l);
            p->i += l;
            return 0;
        }
    }
    return fail(p, JE_CHAR, p->i);
}

static int run(P *p)
{
    if (skip(p) < 0)
        return -1;
    if (p->i >= p->n)
        return fail(p, JE_END, p->i);
    if (value(p) < 0 || skip(p) < 0)
        return -1;
    if (p->i < p->n)
        return fail(p, JE_EXTRA, p->i);
    return 0;
}

int json_check(const char *s, size_t n, int lenient, JsonErr *e)
{
    P p;

    memset(&p, 0, sizeof p);
    memset(e, 0, sizeof *e);
    p.s = s;
    p.n = n;
    p.lenient = lenient;
    p.e = e;
    return run(&p);
}

int json_format(const char *s, size_t n, int lenient, const char *indent, SBuf *out, JsonErr *e)
{
    P p;

    memset(&p, 0, sizeof p);
    memset(e, 0, sizeof *e);
    p.s = s;
    p.n = n;
    p.lenient = lenient;
    p.e = e;
    /* a first pass: errors and comments are told before anything is written */
    if (run(&p) < 0)
        return -1;
    if (p.comments)
        return 1;
    memset(&p, 0, sizeof p);
    p.s = s;
    p.n = n;
    p.lenient = lenient;
    p.e = e;
    p.o = out;
    p.ind = indent;
    return run(&p);
}
