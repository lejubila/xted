#ifndef XTED_LAUNCH_H
#define XTED_LAUNCH_H

#include "ipc.h"

/* Starts a terminal emulator with xted and the files in it (xted --desktop, when no xted took
 * them): term is the "terminal =" of the configuration ("" for none: $TERMINAL, then the ones
 * known). It does not return when a terminal starts; -1 when none could. */
int launch_in_terminal(const IpcFile *f, size_t n, const char *term);

#endif
