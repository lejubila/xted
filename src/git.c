#include "sys.h"
#include "git.h"
#include "shell.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define GIT_TIMEOUT_MS 1500
#define GIT_MAX_OUTPUT (8u << 20)

static size_t num(const char *s, size_t n, size_t *i, int *ok)
{
    size_t v = 0, k = 0;

    while (*i < n && s[*i] >= '0' && s[*i] <= '9' && k < 9) {
        v = v * 10 + (size_t)(s[*i] - '0');
        (*i)++;
        k++;
    }
    *ok = k > 0;
    return v;
}

/* "-a[,b] +c[,d] @@" at s[i]; returns 1 and the four numbers. */
static int hunk(const char *s, size_t n, size_t i, size_t *b, size_t *c, size_t *d)
{
    int ok;
    size_t a;

    if (i >= n || s[i] != '-')
        return 0;
    i++;
    a = num(s, n, &i, &ok);
    (void)a;
    if (!ok)
        return 0;
    *b = 1;
    if (i < n && s[i] == ',') {
        i++;
        *b = num(s, n, &i, &ok);
        if (!ok)
            return 0;
    }
    if (i + 1 >= n || s[i] != ' ' || s[i + 1] != '+')
        return 0;
    i += 2;
    *c = num(s, n, &i, &ok);
    if (!ok)
        return 0;
    *d = 1;
    if (i < n && s[i] == ',') {
        i++;
        *d = num(s, n, &i, &ok);
        if (!ok)
            return 0;
    }
    return 1;
}

void git_parse_diff(const char *diff, size_t n, uint8_t *marks, size_t nlines)
{
    size_t pos = 0;

    while (pos < n) {
        size_t len = 0, b, c, d, k;

        while (pos + len < n && diff[pos + len] != '\n')
            len++;
        if (len > 3 && memcmp(diff + pos, "@@ ", 3) == 0 && hunk(diff + pos, len, 3, &b, &c, &d)) {
            if (d == 0) {
                size_t at = c > 0 ? c - 1 : 0;

                if (at < nlines && marks[at] == GIT_NONE)
                    marks[at] = GIT_DEL;
            } else {
                size_t nm = b < d ? b : d;

                for (k = 0; k < d; k++) {
                    size_t at = c - 1 + k;

                    if (c == 0 || at >= nlines)
                        break;
                    marks[at] = k < nm ? GIT_MOD : GIT_ADD;
                }
            }
        }
        pos += len + 1;
    }
}

int git_file_marks(const char *path, size_t nlines, uint8_t **marks)
{
    char *copy = xstrdup(path), *slash = strrchr(copy, '/');
    const char *dir = ".", *base = copy;
    const char *ls[] = { "git", "-C", NULL, "ls-files", "--error-unmatch", "--", NULL, NULL };
    const char *diff[] = { "git", "-C", NULL, "-c", "core.quotepath=off", "diff", "--no-color",
                           "--no-ext-diff", "-U0", "HEAD", "--", NULL, NULL };
    SBuf out = SBUF_INIT;
    int st = 0, r, result = 0;

    if (slash) {
        *slash = '\0';
        dir = slash == copy ? "/" : copy;
        base = slash + 1;
    }
    /* reading must not take the index lock behind the user's back */
    sys_setenv("GIT_OPTIONAL_LOCKS", "0");
    ls[2] = dir;
    ls[6] = base;
    r = shell_capture(ls, NULL, NULL, 0, NULL, NULL, GIT_TIMEOUT_MS, 1 << 16, &st);
    if (r != 0 || (WIFEXITED(st) && WEXITSTATUS(st) == 127)) {
        result = -1;
        goto done;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        goto done;                          /* not tracked, or not a repository */
    diff[2] = dir;
    diff[11] = base;
    r = shell_capture(diff, NULL, NULL, 0, &out, NULL, GIT_TIMEOUT_MS, GIT_MAX_OUTPUT, &st);
    if (r == 0 && WIFEXITED(st) && WEXITSTATUS(st) != 0) {
        /* no commit yet: compare with the index instead */
        const char *idx[] = { "git", "-C", dir, "-c", "core.quotepath=off", "diff", "--no-color",
                              "--no-ext-diff", "-U0", "--", base, NULL };

        out.len = 0;
        r = shell_capture(idx, NULL, NULL, 0, &out, NULL, GIT_TIMEOUT_MS, GIT_MAX_OUTPUT, &st);
    }
    if (r != 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        result = -1;
        goto done;
    }
    *marks = xmalloc(nlines ? nlines : 1);
    memset(*marks, 0, nlines ? nlines : 1);
    git_parse_diff(out.p ? out.p : "", out.len, *marks, nlines);
    result = 1;
done:
    sbuf_free(&out);
    free(copy);
    return result;
}
