#ifndef KED_SESSION_H
#define KED_SESSION_H

#include <stddef.h>

/* Files that were open in a folder and where the cursor was in each. */
typedef struct {
    char *path;             /* absolute */
    size_t y, x, rowoff;
} SessionEntry;

typedef struct {
    SessionEntry *e;
    size_t n, cap;
    size_t active;          /* index of the file that was shown */
    char *layout;           /* panes, as written by the editor (NULL: none) */
    size_t activewin;       /* which pane had the focus, in reading order */
    int sidebar;            /* the file browser was shown */
} Session;

/* File that holds the session of the current folder, under $XDG_STATE_HOME
 * (or ~/.local/state)/ked/sessions/. malloc'ed; NULL if there is no home. */
char *session_file(void);
void session_add(Session *s, const char *path, size_t y, size_t x, size_t rowoff);
/* Writes (creating folders) or, with no entries, removes the file. 0 on success. */
int session_save(const char *file, const Session *s);
/* 0 on success; -1 if the file is missing or malformed. */
int session_load(const char *file, Session *s);
void session_free(Session *s);

#endif
