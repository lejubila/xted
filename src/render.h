#ifndef KED_RENDER_H
#define KED_RENDER_H

struct Editor;

/* Remembers what each screen row currently shows, so that a frame only
 * rewrites the rows that changed. */
typedef struct Render Render;

Render *render_create(void);
void render_free(Render *r);
/* Forget the screen contents: the next frame clears and redraws all. */
void render_invalidate(Render *r);
/* Draws the editor state with a single write(); no-op if nothing changed. */
void render_frame(Render *r, const struct Editor *e);

#endif
