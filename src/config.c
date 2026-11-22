#include "sys.h"
#include "config.h"
#include "i18n.h"
#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LangCfg unset(void)
{
    LangCfg l;

    l.tab_width = l.expand_tab = l.trim_trailing_space = l.final_newline = -1;
    l.indent_width = -1;
    l.set = 0;
    l.autosuggest = l.suggest_min = l.autopair = -1;
    return l;
}

void config_init(Config *c)
{
    memset(c, 0, sizeof *c);
    c->base = unset();
    c->clipboard = -1;
    c->git = -1;
    c->mouse = -1;
    c->single_instance = -1;
    c->colors = -1;
    c->glyphs = -1;
    c->keybar = -1;
}

void config_free(Config *c)
{
    size_t i;

    for (i = 0; i < c->nlang; i++)
        free(c->lang[i].name);
    free(c->lang);
    for (i = 0; i < c->nkeys; i++) {
        free(c->keys[i].key);
        free(c->keys[i].cmd);
    }
    free(c->keys);
    memset(c, 0, sizeof *c);
}

static void complain(Config *c, int line, const char *fmt, ...) XTED_PRINTF(3, 4);

static void complain(Config *c, int line, const char *fmt, ...)
{
    va_list ap;
    char msg[120];

    c->nerrors++;
    if (c->first_error[0])
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (line > 0)
        snprintf(c->first_error, sizeof c->first_error, tr("line %d: %s"), line, msg);
    else
        snprintf(c->first_error, sizeof c->first_error, "%s", msg);
}

void config_error(Config *c, int line, const char *msg)
{
    complain(c, line, "%s", msg);
}

static int parse_bool(const char *v)
{
    if (!strcmp(v, "on") || !strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1") ||
        !strcmp(v, "si") || !strcmp(v, "sì"))
        return 1;
    if (!strcmp(v, "off") || !strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "0"))
        return 0;
    return -1;
}

static int parse_int(const char *v, int lo, int hi)
{
    char *end;
    long n;

    if (!*v)
        return -1;
    n = strtol(v, &end, 10);
    if (*end || n < lo || n > hi)
        return -1;
    return (int)n;
}

/* Sets one language-dependent value; returns 0, or -1 if the key is not one of them, -2 if the value is bad. */
static int set_lang_value(LangCfg *l, const char *key, const char *val)
{
    int v;

    if (!strcmp(key, "tab_width")) {
        v = parse_int(val, 1, 16);
        if (v < 0)
            return -2;
        l->tab_width = v;
    } else if (!strcmp(key, "indent_width")) {
        v = parse_int(val, 1, 16);
        if (v < 0)
            return -2;
        l->indent_width = v;
    } else if (!strcmp(key, "expand_tab")) {
        v = parse_bool(val);
        if (v < 0)
            return -2;
        l->expand_tab = v;
    } else if (!strcmp(key, "trim_trailing_space")) {
        v = parse_bool(val);
        if (v < 0)
            return -2;
        l->trim_trailing_space = v;
    } else if (!strcmp(key, "final_newline")) {
        if (!strcmp(val, "keep"))
            l->final_newline = 0;
        else if (!strcmp(val, "always"))
            l->final_newline = 1;
        else
            return -2;
    } else if (!strcmp(key, "autosuggest")) {
        v = parse_bool(val);
        if (v < 0)
            return -2;
        l->autosuggest = v;
    } else if (!strcmp(key, "autopair")) {
        v = parse_bool(val);
        if (v < 0)
            return -2;
        l->autopair = v;
    } else if (!strcmp(key, "suggest_min")) {
        v = parse_int(val, 1, 10);
        if (v < 0)
            return -2;
        l->suggest_min = v;
    } else {
        return -1;
    }
    return 0;
}

static LangSection *section(Config *c, const char *name)
{
    size_t i;

    for (i = 0; i < c->nlang; i++)
        if (!strcmp(c->lang[i].name, name))
            return &c->lang[i];
    if (c->nlang == c->langcap) {
        c->langcap = c->langcap ? c->langcap * 2 : 8;
        c->lang = xrealloc(c->lang, c->langcap * sizeof *c->lang);
    }
    c->lang[c->nlang].name = xstrdup(name);
    c->lang[c->nlang].c = unset();
    return &c->lang[c->nlang++];
}

static void lower(char *s)
{
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

/* Trims blanks at both ends in place; returns the start. */
static char *trim(char *s)
{
    size_t n;

    while (*s == ' ' || *s == '\t')
        s++;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

int config_parse(Config *c, const char *text, size_t n)
{
    size_t pos = 0;
    int lineno = 0;
    enum { TOP, LANG, KEYS } where = TOP;
    LangSection *sec = NULL;

    while (pos < n) {
        size_t len = 0;
        char *line, *eq, *key, *val, *hash;

        while (pos + len < n && text[pos + len] != '\n')
            len++;
        line = xmalloc(len + 1);
        memcpy(line, text + pos, len);
        line[len] = '\0';
        pos += len + 1;
        lineno++;

        /* a '#' starts a comment (values here never contain one, except in [keys]: "#" is not a key) */
        hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        key = trim(line);
        if (!*key) {
            free(line);
            continue;
        }
        if (*key == '[') {
            size_t kl = strlen(key);

            if (key[kl - 1] != ']' || kl < 3) {
                complain(c, lineno, tr("invalid section «%s»"), key);
                where = TOP;
                sec = NULL;
                free(line);
                continue;
            }
            key[kl - 1] = '\0';
            key = trim(key + 1);
            lower(key);
            if (!strcmp(key, "keys")) {
                where = KEYS;
            } else {
                where = LANG;
                sec = section(c, key);
            }
            free(line);
            continue;
        }
        eq = strchr(key, '=');
        if (!eq) {
            complain(c, lineno, tr("missing «=» in «%s»"), key);
            free(line);
            continue;
        }
        *eq = '\0';
        val = trim(eq + 1);
        key = trim(key);
        if (where == KEYS) {
            if (!*val) {
                complain(c, lineno, tr("missing command for «%s»"), key);
            } else {
                if (c->nkeys == c->keycap) {
                    c->keycap = c->keycap ? c->keycap * 2 : 16;
                    c->keys = xrealloc(c->keys, c->keycap * sizeof *c->keys);
                }
                c->keys[c->nkeys].key = xstrdup(key);
                c->keys[c->nkeys].cmd = xstrdup(val);
                c->keys[c->nkeys].line = lineno;
                c->nkeys++;
            }
            free(line);
            continue;
        }
        lower(key);
        {
            int r;
            LangCfg *dst = where == LANG ? &sec->c : &c->base;
            char *lv = xstrdup(val);

            lower(lv);
            r = set_lang_value(dst, key, lv);
            if (r == -1 && where == TOP) {
                if (!strcmp(key, "theme")) {
                    if (!*val || strlen(val) >= sizeof c->theme)
                        complain(c, lineno, tr("invalid theme"));
                    else
                        snprintf(c->theme, sizeof c->theme, "%s", val);
                    r = 0;
                } else if (!strcmp(key, "language")) {
                    size_t k;

                    for (k = 0; lv[k] && (isalnum((unsigned char)lv[k]) || lv[k] == '_' || lv[k] == '-'); k++)
                        ;
                    if (!*lv || lv[k] || k >= sizeof c->language)
                        r = -2;
                    else
                        snprintf(c->language, sizeof c->language, "%s", lv);
                    if (r != -2)
                        r = 0;
                } else if (!strcmp(key, "keybar")) {
                    /* the keys of the context: in the message row, in a row of their own, none */
                    if (!strcmp(lv, "message"))
                        c->keybar = 0;
                    else if (!strcmp(lv, "line"))
                        c->keybar = 1;
                    else if (!strcmp(lv, "off"))
                        c->keybar = 2;
                    else
                        r = -2;
                    if (r != -2)
                        r = 0;
                } else if (!strcmp(key, "glyphs")) {
                    /* the symbols drawn: unicode (▸ ▾ │ ●), windows (► ▼, which every
                     * Windows console font has) or ascii (+ - | *), for fonts that lack them */
                    if (!strcmp(lv, "unicode") || !strcmp(lv, "utf-8") || !strcmp(lv, "utf8"))
                        c->glyphs = 0;
                    else if (!strcmp(lv, "windows") || !strcmp(lv, "wgl4"))
                        c->glyphs = 1;
                    else if (!strcmp(lv, "ascii"))
                        c->glyphs = 2;
                    else
                        r = -2;
                    if (r != -2)
                        r = 0;
                } else if (!strcmp(key, "colors")) {
                    /* how many colours the terminal shows: auto (from TERM and COLORTERM),
                     * truecolor, 256, 16 or none */
                    if (!strcmp(lv, "auto"))
                        c->colors = -1;
                    else if (!strcmp(lv, "truecolor") || !strcmp(lv, "24bit") || !strcmp(lv, "24"))
                        c->colors = 24;
                    else if (!strcmp(lv, "256"))
                        c->colors = 256;
                    else if (!strcmp(lv, "16") || !strcmp(lv, "8"))
                        c->colors = 16;
                    else if (!strcmp(lv, "none") || !strcmp(lv, "off") || !strcmp(lv, "0"))
                        c->colors = 0;
                    else
                        r = -2;
                    if (r != -2)
                        r = 0;
                } else if (!strcmp(key, "clipboard") || !strcmp(key, "git") ||
                           !strcmp(key, "mouse") || !strcmp(key, "single_instance")) {
                    int b = parse_bool(lv);

                    if (b < 0) {
                        r = -2;
                    } else {
                        if (key[0] == 'c')
                            c->clipboard = b;
                        else if (key[0] == 'g')
                            c->git = b;
                        else if (key[0] == 's')
                            c->single_instance = b;
                        else
                            c->mouse = b;
                        r = 0;
                    }
                }
            }
            if (r == -1)
                complain(c, lineno, tr("unknown key «%s»%s"), key,
                         where == LANG ? tr(" in a language section") : "");
            else if (r == -2)
                complain(c, lineno, tr("invalid value «%s» for %s"), val, key);
            free(lv);
        }
        free(line);
    }
    return c->nerrors;
}

char *config_dir(void)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    size_t n;
    char *d;

    if (x && sys_is_abs(x))
        return app_dir(x);
    if (h && h[0]) {
        n = strlen(h) + 16;
        d = xmalloc(n);
        snprintf(d, n, "%s/.config", h);
        {
            char *r = app_dir(d);

            free(d);
            return r;
        }
    }
    return NULL;
}

int config_load_file(Config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    SBuf text = SBUF_INIT;
    char buf[4096];
    size_t n;

    if (!f)
        return 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.len < (1u << 20))
        sbuf_add(&text, buf, n);
    fclose(f);
    config_parse(c, text.p ? text.p : "", text.len);
    sbuf_free(&text);
    return 1;
}

int config_load(Config *c, const char *dir)
{
    size_t n = strlen(dir) + 8;
    char *path = xmalloc(n);
    int r;

    snprintf(path, n, "%s/config", dir);
    r = config_load_file(c, path);
    free(path);
    return r;
}

static void overlay(LangCfg *dst, const LangCfg *src)
{
    if (src->tab_width >= 0) {
        dst->tab_width = src->tab_width;
        dst->set |= CFG_TAB_WIDTH;
    }
    if (src->expand_tab >= 0) {
        dst->expand_tab = src->expand_tab;
        dst->set |= CFG_EXPAND_TAB;
    }
    if (src->indent_width >= 0) {
        dst->indent_width = src->indent_width;
        dst->set |= CFG_INDENT_WIDTH;
    }
    if (src->trim_trailing_space >= 0) {
        dst->trim_trailing_space = src->trim_trailing_space;
        dst->set |= CFG_TRIM;
    }
    if (src->final_newline >= 0) {
        dst->final_newline = src->final_newline;
        dst->set |= CFG_FINAL_NL;
    }
    if (src->autosuggest >= 0) {
        dst->autosuggest = src->autosuggest;
        dst->set |= CFG_AUTOSUGGEST;
    }
    if (src->autopair >= 0) {
        dst->autopair = src->autopair;
        dst->set |= CFG_AUTOPAIR;
    }
    if (src->suggest_min >= 0) {
        dst->suggest_min = src->suggest_min;
        dst->set |= CFG_SUGGEST_MIN;
    }
}

LangCfg config_lang(const Config *c, const char *lang)
{
    LangCfg out;
    char name[64];
    size_t i;

    out.tab_width = 8;
    out.expand_tab = 0;
    out.trim_trailing_space = 0;
    out.final_newline = 0;
    out.autosuggest = 1;
    out.autopair = 1;
    out.indent_width = 0;
    out.set = 0;
    out.suggest_min = 2;
    /* data files are indented with two spaces (YAML forbids tabs, JSON's tools write so): a
     * default, under the configuration, .editorconfig and the file's own style */
    if (lang && (strcmp(lang, "JSON") == 0 || strcmp(lang, "YAML") == 0)) {
        out.expand_tab = 1;
        out.indent_width = 2;
    }
    overlay(&out, &c->base);
    if (lang) {
        snprintf(name, sizeof name, "%s", lang);
        lower(name);
        for (i = 0; i < c->nlang; i++)
            if (!strcmp(c->lang[i].name, name))
                overlay(&out, &c->lang[i].c);
    }
    return out;
}

void config_merge(LangCfg *d, const LangCfg *s)
{
    if (s->set & CFG_TAB_WIDTH)
        d->tab_width = s->tab_width;
    if (s->set & CFG_EXPAND_TAB)
        d->expand_tab = s->expand_tab;
    if (s->set & CFG_INDENT_WIDTH)
        d->indent_width = s->indent_width;
    if (s->set & CFG_TRIM)
        d->trim_trailing_space = s->trim_trailing_space;
    if (s->set & CFG_FINAL_NL)
        d->final_newline = s->final_newline;
    if (s->set & CFG_AUTOSUGGEST)
        d->autosuggest = s->autosuggest;
    if (s->set & CFG_AUTOPAIR)
        d->autopair = s->autopair;
    if (s->set & CFG_SUGGEST_MIN)
        d->suggest_min = s->suggest_min;
    d->set |= s->set;
}
