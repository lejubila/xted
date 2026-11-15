#ifndef XTED_CONFIG_H
#define XTED_CONFIG_H

#include <stddef.h>

/* The user's settings file (~/.config/xted/config): "key = value" lines, "#" comments,
 * "[language]" sections that override the language-dependent settings, and a "[keys]"
 * section of "key = command" lines. Parsing never fails: a bad line is skipped and
 * counted, and the first problem is kept as a message with its line number. -1 means
 * "not set" in every field below. */

typedef struct {
    int tab_width;              /* 1..16: how wide a tab character is drawn */
    int expand_tab;             /* Tab inserts spaces */
    int indent_width;           /* 1..16: spaces of one indentation level (0: tab_width) */
    int trim_trailing_space;    /* on save */
    int final_newline;          /* on save: 0 keep as it was, 1 always end with a newline */
    int autosuggest;
    int autopair;               /* typing an opening bracket or quote adds the closing one */
    int suggest_min;            /* letters typed before the popup shows, 1..10 */
    unsigned set;               /* CFG_*: given by the user, not a default (config_lang) */
} LangCfg;

enum { CFG_TAB_WIDTH = 1, CFG_EXPAND_TAB = 2, CFG_INDENT_WIDTH = 4, CFG_TRIM = 8, CFG_FINAL_NL = 16,
       CFG_AUTOSUGGEST = 32, CFG_AUTOPAIR = 64, CFG_SUGGEST_MIN = 128 };

typedef struct {
    char *name;                 /* language name, lower case */
    LangCfg c;
} LangSection;

typedef struct {
    char *key, *cmd;            /* "ctrl+s" = "save" */
    int line;
} KeyLine;

typedef struct {
    LangCfg base;               /* settings outside any section, over the defaults */
    char theme[64];             /* "" when not set */
    int clipboard;              /* copies also go to the system clipboard (OSC 52) */
    int git;                    /* Git indicators in the margin */
    int mouse;                  /* the terminal reports clicks and the wheel */
    LangSection *lang;
    size_t nlang, langcap;
    KeyLine *keys;
    size_t nkeys, keycap;
    int nerrors;
    char first_error[160];      /* "riga 3: ..." */
} Config;

void config_init(Config *c);
void config_free(Config *c);
/* Parses text; returns the number of bad lines (also in c->nerrors). */
int config_parse(Config *c, const char *text, size_t n);
/* Records a problem found while applying the settings (counted like a bad line). */
void config_error(Config *c, int line, const char *msg);
/* $XDG_CONFIG_HOME/xted or ~/.config/xted (malloc'ed), NULL if neither is known. An older
 * folder called "ked" is used when it is the only one that exists. */
char *config_dir(void);
/* Reads dir/config if it exists. Returns 1 if a file was read. */
int config_load(Config *c, const char *dir);
/* The same for any file (a project's .xted). */
int config_load_file(Config *c, const char *path);
/* The settings that apply to a language (any case; NULL = plain text): the defaults, then the
 * top-level values, then the language's own section. */
LangCfg config_lang(const Config *c, const char *lang);
/* Copies into d the values that s has set explicitly (s->set), over what d has. */
void config_merge(LangCfg *d, const LangCfg *s);

#endif
