#ifndef XTED_THEME_H
#define XTED_THEME_H

#include "syntax.h"
#include "util.h"

typedef struct {
    unsigned char r, g, b;
} Rgb;

typedef struct {
    const char *name;
    int has_bg;             /* 0: leave the terminal's own background */
    Rgb fg, bg;
    Rgb cls[SC_COUNT];
    Rgb match;              /* background of search matches */
    Rgb popup, popup_sel;   /* suggestion popup: rows and the highlighted row */
    Rgb git_add, git_mod, git_del;  /* margin marks of the Git indicators */
} Theme;

enum { COLOR_NONE = 0, COLOR_16 = 16, COLOR_256 = 256, COLOR_TRUE = 24 };

/* Number of themes; theme_get wraps around. */
size_t theme_count(void);
const Theme *theme_get(size_t i);
/* Index of the theme with this name (any case), or -1. */
int theme_by_name(const char *name);
/* Adds a theme written as lines of "key value" (name, foreground, background, one line per
 * syntax class, match, popup, popup_sel, git_add, git_mod, git_del); colours are #rrggbb and
 * background may be "none". What is missing comes from the dark theme. Returns 0, or -1 with
 * a message in err. */
int theme_add_user(const char *text, size_t n, char *err, size_t errlen);
/* Adds every file of dir; failures go to report(). Returns how many were added. */
size_t theme_load_dir(const char *dir, void (*report)(const char *msg, void *ctx), void *ctx);
void theme_clear_user(void);

/* From NO_COLOR, COLORTERM and TERM. */
int theme_detect_depth(void);
/* Appends an SGR sequence setting the foreground / background. */
void theme_fg(SBuf *o, int depth, Rgb c);
void theme_bg(SBuf *o, int depth, Rgb c);
/* Colour index of c in the 256-colour palette / the 16 ANSI colours. */
int theme_to_256(Rgb c);
int theme_to_16(Rgb c);

#endif
