#include "theme.h"
#include "i18n.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Theme themes[] = {
    {
        "dark", 0, { 212, 212, 212 }, { 0, 0, 0 },
        {
            /* normal */   { 212, 212, 212 },
            /* comment */  { 106, 153, 85 },
            /* string */   { 206, 145, 120 },
            /* keyword */  { 197, 134, 192 },
            /* type */     { 78, 201, 176 },
            /* constant */ { 86, 156, 214 },
            /* number */   { 181, 206, 168 },
            /* preproc */  { 155, 155, 155 },
            /* function */ { 220, 220, 170 },
            /* operator */ { 212, 212, 212 },
            /* variable */ { 156, 220, 254 },
            /* heading */  { 86, 156, 214 },
            /* emphasis */ { 220, 160, 90 },
            /* code */     { 206, 145, 120 },
            /* link */     { 79, 193, 255 },
        },
        { 90, 75, 10 }, { 45, 45, 48 }, { 4, 57, 94 },
        { 60, 170, 60 }, { 200, 160, 40 }, { 210, 70, 70 }
    },
    {
        "light", 1, { 56, 58, 66 }, { 236, 236, 239 },
        {
            /* normal */   { 56, 58, 66 },
            /* comment */  { 110, 130, 100 },
            /* string */   { 163, 21, 21 },
            /* keyword */  { 175, 0, 219 },
            /* type */     { 38, 127, 153 },
            /* constant */ { 0, 0, 255 },
            /* number */   { 9, 134, 88 },
            /* preproc */  { 110, 110, 110 },
            /* function */ { 121, 94, 38 },
            /* operator */ { 56, 58, 66 },
            /* variable */ { 0, 16, 128 },
            /* heading */  { 0, 0, 255 },
            /* emphasis */ { 175, 90, 0 },
            /* code */     { 163, 21, 21 },
            /* link */     { 4, 81, 165 },
        },
        { 255, 236, 150 }, { 222, 222, 228 }, { 173, 214, 255 },
        { 30, 130, 30 }, { 170, 120, 0 }, { 190, 40, 40 }
    },
};

/* Themes read from the user's folder come after the built-in ones. */
static Theme *utheme;
static size_t nutheme, ucap;

#define NBUILTIN (sizeof themes / sizeof themes[0])

size_t theme_count(void)
{
    return NBUILTIN + nutheme;
}

const Theme *theme_get(size_t i)
{
    i %= theme_count();
    return i < NBUILTIN ? &themes[i] : &utheme[i - NBUILTIN];
}

static int same_name(const char *a, const char *b)
{
    while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

int theme_by_name(const char *name)
{
    size_t i;

    /* the names the themes had before the texts became English */
    if (same_name("scuro", name))
        name = "dark";
    else if (same_name("chiaro", name))
        name = "light";
    for (i = theme_count(); i-- > 0;)
        if (same_name(theme_get(i)->name, name))
            return (int)i;
    return -1;
}

/* TERM is one of the names in list (blank separated), or starts with one followed by "-". */
static int term_is(const char *t, const char *list)
{
    size_t tl = strlen(t);

    while (*list) {
        size_t n = strcspn(list, " ");

        if (n && tl >= n && memcmp(t, list, n) == 0 && (tl == n || t[n] == '-'))
            return 1;
        list += n;
        while (*list == ' ')
            list++;
    }
    return 0;
}

int theme_detect_depth(void)
{
    const char *nc = getenv("NO_COLOR"), *ct = getenv("COLORTERM"), *t = getenv("TERM");
    const char *wt = getenv("WT_SESSION");
    size_t tl;

    if (nc && nc[0])
        return COLOR_NONE;
    if (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit")))
        return COLOR_TRUE;
#ifdef _WIN32
    /* no TERM on Windows: Windows Terminal and the console of Windows 10 show any colour */
    if (!t || !t[0])
        return COLOR_TRUE;
#endif
    if (!t || !t[0] || strcmp(t, "dumb") == 0)
        return COLOR_NONE;
    tl = strlen(t);
    /* terminals known to show any colour (Windows Terminal says so with WT_SESSION) */
    if ((tl > 7 && strcmp(t + tl - 7, "-direct") == 0) || (wt && wt[0]) ||
        term_is(t, "xterm-kitty xterm-ghostty alacritty foot wezterm contour"))
        return COLOR_TRUE;
    if (strstr(t, "256"))
        return COLOR_256;
    /* the xterm family shows 256 colours even when TERM does not say it: PuTTY and KiTTY send
     * "xterm" over SSH, and COLORTERM does not travel with it */
    if (term_is(t, "xterm putty screen tmux rxvt konsole gnome vte st kitty mintty iterm"))
        return COLOR_256;
    return COLOR_16;
}

static int sq(int a)
{
    return a * a;
}

static int level(int v)
{
    static const int steps[6] = { 0, 95, 135, 175, 215, 255 };
    int i, best = 0;

    for (i = 1; i < 6; i++)
        if (abs(v - steps[i]) < abs(v - steps[best]))
            best = i;
    return best;
}

int theme_to_256(Rgb c)
{
    static const int steps[6] = { 0, 95, 135, 175, 215, 255 };
    int ri = level(c.r), gi = level(c.g), bi = level(c.b);
    int cube = 16 + 36 * ri + 6 * gi + bi;
    int avg = (c.r + c.g + c.b) / 3;
    int gi2 = (avg - 8 + 5) / 10, gray, dc, dg;

    if (gi2 < 0)
        gi2 = 0;
    if (gi2 > 23)
        gi2 = 23;
    gray = 8 + 10 * gi2;
    dc = sq(c.r - steps[ri]) + sq(c.g - steps[gi]) + sq(c.b - steps[bi]);
    dg = sq(c.r - gray) + sq(c.g - gray) + sq(c.b - gray);
    return dg < dc ? 232 + gi2 : cube;
}

int theme_to_16(Rgb c)
{
    /* xterm's palette in SGR order: 0-7 normal, 8-15 bright */
    static const Rgb pal[16] = {
        { 0, 0, 0 }, { 205, 0, 0 }, { 0, 205, 0 }, { 205, 205, 0 },
        { 0, 0, 238 }, { 205, 0, 205 }, { 0, 205, 205 }, { 229, 229, 229 },
        { 127, 127, 127 }, { 255, 0, 0 }, { 0, 255, 0 }, { 255, 255, 0 },
        { 92, 92, 255 }, { 255, 0, 255 }, { 0, 255, 255 }, { 255, 255, 255 },
    };
    int i, best = 0, bd = 1 << 30;

    for (i = 0; i < 16; i++) {
        int d = sq(c.r - pal[i].r) + sq(c.g - pal[i].g) + sq(c.b - pal[i].b);

        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static void color(SBuf *o, int depth, Rgb c, int base)
{
    switch (depth) {
    case COLOR_TRUE:
        sbuf_addf(o, "\x1b[%d;2;%d;%d;%dm", base + 8, c.r, c.g, c.b);
        break;
    case COLOR_256:
        sbuf_addf(o, "\x1b[%d;5;%dm", base + 8, theme_to_256(c));
        break;
    case COLOR_16: {
        int i = theme_to_16(c);

        sbuf_addf(o, "\x1b[%dm", (i < 8 ? base : base + 60) + (i & 7));
        break;
    }
    default:
        break;
    }
}

void theme_fg(SBuf *o, int depth, Rgb c)
{
    color(o, depth, c, 30);
}

void theme_bg(SBuf *o, int depth, Rgb c)
{
    color(o, depth, c, 40);
}

/* ---- themes from files ------------------------------------------------------ */

static int hex(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int parse_color(const char *s, Rgb *out)
{
    int i, v[6];

    if (*s == '#')
        s++;
    if (strlen(s) != 6)
        return -1;
    for (i = 0; i < 6; i++) {
        v[i] = hex((unsigned char)s[i]);
        if (v[i] < 0)
            return -1;
    }
    out->r = (unsigned char)(v[0] * 16 + v[1]);
    out->g = (unsigned char)(v[2] * 16 + v[3]);
    out->b = (unsigned char)(v[4] * 16 + v[5]);
    return 0;
}

static const char *const class_keys[SC_COUNT] = {
    "normal", "comment", "string", "keyword", "type", "constant", "number", "preproc",
    "function", "operator", "variable", "heading", "emphasis", "code", "link"
};

int theme_add_user(const char *text, size_t n, char *err, size_t errlen)
{
    Theme t = themes[0];
    char *name = NULL;
    size_t pos = 0;
    int lineno = 0;

    while (pos < n) {
        size_t len = 0, i;
        char line[200], *key, *val, *sp;
        Rgb c;

        while (pos + len < n && text[pos + len] != '\n')
            len++;
        snprintf(line, sizeof line, "%.*s", (int)(len < sizeof line - 1 ? len : sizeof line - 1),
                 text + pos);
        pos += len + 1;
        lineno++;
        key = line;
        while (*key == ' ' || *key == '\t')
            key++;
        if (!*key || *key == '#')
            continue;
        sp = key;
        while (*sp && *sp != ' ' && *sp != '\t')
            sp++;
        if (*sp)
            *sp++ = '\0';
        val = sp;
        while (*val == ' ' || *val == '\t')
            val++;
        for (i = strlen(val); i > 0 && (val[i - 1] == ' ' || val[i - 1] == '\t' || val[i - 1] == '\r'); i--)
            val[i - 1] = '\0';
        if (strcmp(key, "name") == 0) {
            if (!*val) {
                snprintf(err, errlen, tr("line %d: missing name"), lineno);
                free(name);
                return -1;
            }
            free(name);
            name = xstrdup(val);
            continue;
        }
        if (strcmp(key, "background") == 0 && strcmp(val, "none") == 0) {
            t.has_bg = 0;
            continue;
        }
        if (parse_color(val, &c) < 0) {
            snprintf(err, errlen, tr("line %d: invalid colour «%s» (#rrggbb needed)"), lineno, val);
            free(name);
            return -1;
        }
        if (strcmp(key, "foreground") == 0) {
            t.fg = c;
            t.cls[SC_NORMAL] = c;
        } else if (strcmp(key, "background") == 0) {
            t.bg = c;
            t.has_bg = 1;
        } else if (strcmp(key, "match") == 0) {
            t.match = c;
        } else if (strcmp(key, "popup") == 0) {
            t.popup = c;
        } else if (strcmp(key, "popup_sel") == 0) {
            t.popup_sel = c;
        } else if (strcmp(key, "git_add") == 0) {
            t.git_add = c;
        } else if (strcmp(key, "git_mod") == 0) {
            t.git_mod = c;
        } else if (strcmp(key, "git_del") == 0) {
            t.git_del = c;
        } else {
            for (i = 0; i < SC_COUNT; i++)
                if (strcmp(key, class_keys[i]) == 0)
                    break;
            if (i == SC_COUNT) {
                snprintf(err, errlen, tr("line %d: unknown key «%s»"), lineno, key);
                free(name);
                return -1;
            }
            t.cls[i] = c;
        }
    }
    if (!name) {
        snprintf(err, errlen, tr("missing «name» line"));
        return -1;
    }
    t.name = name;
    if (nutheme == ucap) {
        ucap = ucap ? ucap * 2 : 4;
        utheme = xrealloc(utheme, ucap * sizeof *utheme);
    }
    utheme[nutheme++] = t;
    return 0;
}

void theme_clear_user(void)
{
    size_t i;

    for (i = 0; i < nutheme; i++)
        free((char *)utheme[i].name);
    free(utheme);
    utheme = NULL;
    nutheme = ucap = 0;
}

static int str_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

size_t theme_load_dir(const char *dir, void (*report)(const char *msg, void *ctx), void *ctx)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char **names = NULL;
    size_t n = 0, cap = 0, i, added = 0;

    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            names = xrealloc(names, cap * sizeof *names);
        }
        names[n++] = xstrdup(de->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, str_cmp);
    for (i = 0; i < n; i++) {
        size_t pn = strlen(dir) + strlen(names[i]) + 2;
        char *path = xmalloc(pn), err[160], msg[300];
        FILE *f;
        SBuf text = SBUF_INIT;
        char buf[2048];
        size_t got;

        snprintf(path, pn, "%s/%s", dir, names[i]);
        f = fopen(path, "r");
        if (f) {
            while ((got = fread(buf, 1, sizeof buf, f)) > 0 && text.len < (64u << 10))
                sbuf_add(&text, buf, got);
            fclose(f);
            if (theme_add_user(text.p ? text.p : "", text.len, err, sizeof err) == 0) {
                added++;
            } else if (report) {
                snprintf(msg, sizeof msg, "%s: %s", names[i], err);
                report(msg, ctx);
            }
        }
        sbuf_free(&text);
        free(path);
        free(names[i]);
    }
    free(names);
    return added;
}
