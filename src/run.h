#ifndef KED_RUN_H
#define KED_RUN_H

#include "editor.h"

/* Running a shell command in the background and showing its output in a
 * "[Output]" pane (Editor.run). The pipe is watched by the event loop
 * (term_watch), so the editor stays usable while the command runs. */

#define RUN_MAX_OUTPUT (5u << 20)

/* Alt+R: asks for a command (the last one is offered) and runs it. */
void run_prompt(Editor *e);
/* F5: runs the last command again. */
void run_again(Editor *e);
/* Shift+F5: asks the command to end. */
void run_stop(Editor *e);
/* Runs cmd now. */
void run_command(Editor *e, const char *cmd);

/* F8 / Shift+F8: opens the next / previous error of the output at its file and line. */
void run_next_error(Editor *e);
void run_prev_error(Editor *e);
/* Enter in the output pane: opens the error on the cursor line. Returns 0 if the key was not
 * for the output (the cursor is not in it). */
int run_enter(Editor *e);

/* Event loop hooks: the descriptor to watch (-1 for none), how long to wait at most
 * (-1: forever; a finished pipe whose process has not been collected yet needs a poll),
 * and the handler to call when the pipe is readable or that time has passed. */
int run_fd(const Editor *e);
int run_timeout(const Editor *e);
void run_pump(Editor *e);

/* Is d the output document? Called before d is closed / when the editor ends. */
int run_is_output(const Editor *e, const Doc *d);
void run_doc_removed(Editor *e, Doc *d);
/* Kills the command, if any (the editor is going away). */
void run_kill(Editor *e);
/* Also closes the output pane (before the session is saved). */
void run_shutdown(Editor *e);

/* Appends text to the output document as the command's stdout would (tests). */
void run_append(Editor *e, const char *s, size_t n);

#endif
