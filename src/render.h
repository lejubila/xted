#ifndef XTED_RENDER_H
#define XTED_RENDER_H

#include "term.h"
#include <stddef.h>

struct Editor;

/* Remembers what each screen row currently shows, so that a frame only
 * rewrites the rows that changed. */
typedef struct Render Render;

Render *render_create(void);
void render_free(Render *r);
/* Forget the screen contents: the next frame clears and redraws all. */
void render_invalidate(Render *r);
/* The tab under screen column x of the tab bar, or (size_t)-1. */
size_t render_tab_at(const struct Editor *e, int x);
/* Rows taken by the picker, query line included. */
int render_picker_height(const struct Editor *e);
/* Draws the editor state with a single write(); no-op if nothing changed. */
void render_frame(Render *r, const struct Editor *e);

/* The key a click at (x, y) on the message row's buttons stands for. Returns 0 if none. */
int render_hint_key(int x, int y, Key *k);
/* The button at (x, y) is held down (drawn in reverse) when on, none when off. */
void render_hint_press(int x, int y, int on);

#endif
