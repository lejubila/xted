#ifndef KED_THEME_H
#define KED_THEME_H

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
} Theme;

enum { COLOR_NONE = 0, COLOR_16 = 16, COLOR_256 = 256, COLOR_TRUE = 24 };

/* Number of themes; theme_get wraps around. */
size_t theme_count(void);
const Theme *theme_get(size_t i);

/* From NO_COLOR, COLORTERM and TERM. */
int theme_detect_depth(void);
/* Appends an SGR sequence setting the foreground / background. */
void theme_fg(SBuf *o, int depth, Rgb c);
void theme_bg(SBuf *o, int depth, Rgb c);
/* Colour index of c in the 256-colour palette / the 16 ANSI colours. */
int theme_to_256(Rgb c);
int theme_to_16(Rgb c);

#endif
