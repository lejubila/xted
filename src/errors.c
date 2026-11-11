#include "errors.h"

#include <ctype.h>
#include <string.h>

/* Reads digits at s[*i...]; returns 0 if there are none. */
static int number(const char *s, size_t len, size_t *i, size_t *out)
{
    size_t v = 0, n = 0;

    while (*i < len && isdigit((unsigned char)s[*i]) && n < 9) {
        v = v * 10 + (size_t)(s[*i] - '0');
        (*i)++;
        n++;
    }
    *out = v;
    return n > 0;
}

static int path_char(char c)
{
    return c != ' ' && c != '\t' && c != ':' && c != '(' && c != '"' && c != '\'' &&
           c != '<' && c != '>' && c != '=' && c != '[' && c != ']' && c != ',';
}

/* A path has a slash or a dot with something after it, so "12:30" and "error:3" do not count. */
static int looks_like_path(const char *s, size_t n)
{
    size_t i;
    int slash = 0, dot = 0;

    if (n == 0)
        return 0;
    for (i = 0; i < n; i++) {
        if (s[i] == '/')
            slash = 1;
        if (s[i] == '.' && i > 0 && i + 1 < n)
            dot = 1;
    }
    return slash || dot;
}

static int python_form(const char *s, size_t len, size_t *po, size_t *pl, size_t *ln)
{
    static const char key[] = "File \"";
    size_t i = 0, start, end;

    while (i < len && (s[i] == ' ' || s[i] == '\t'))
        i++;
    if (len - i < sizeof key || memcmp(s + i, key, sizeof key - 1) != 0)
        return 0;
    start = i + sizeof key - 1;
    end = start;
    while (end < len && s[end] != '"')
        end++;
    if (end >= len || end == start)
        return 0;
    i = end + 1;
    if (len - i < 7 || memcmp(s + i, ", line ", 7) != 0)
        return 0;
    i += 7;
    if (!number(s, len, &i, ln))
        return 0;
    *po = start;
    *pl = end - start;
    return 1;
}

int err_parse(const char *line, size_t len, size_t *path_off, size_t *path_len, size_t *lineno,
              size_t *col)
{
    size_t i = 0;

    *col = 0;
    if (python_form(line, len, path_off, path_len, lineno))
        return 1;
    while (i < len) {
        size_t start, j, ln, cl = 0;

        /* a token starts the line or follows a blank or an opening bracket */
        if (!path_char(line[i]) || (i > 0 && path_char(line[i - 1]))) {
            i++;
            continue;
        }
        start = i;
        j = i;
        while (j < len && path_char(line[j]))
            j++;
        if (looks_like_path(line + start, j - start) && j < len) {
            size_t k = j;

            if (line[k] == ':') {
                k++;
                if (number(line, len, &k, &ln)) {
                    if (k < len && line[k] == ':') {
                        size_t m = k + 1;

                        if (number(line, len, &m, &cl))
                            k = m;
                    }
                    if (k >= len || line[k] == ':' || line[k] == ' ' || line[k] == '\t') {
                        *path_off = start;
                        *path_len = j - start;
                        *lineno = ln;
                        *col = cl;
                        return 1;
                    }
                }
            } else if (line[k] == '(') {
                k++;
                if (number(line, len, &k, &ln)) {
                    if (k < len && line[k] == ',') {
                        k++;
                        number(line, len, &k, &cl);
                    }
                    if (k < len && line[k] == ')') {
                        *path_off = start;
                        *path_len = j - start;
                        *lineno = ln;
                        *col = cl;
                        return 1;
                    }
                }
            }
        }
        i = j > i ? j : i + 1;
    }
    return 0;
}
