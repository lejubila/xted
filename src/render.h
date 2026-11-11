#ifndef KED_RENDER_H
#define KED_RENDER_H

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

#endif
