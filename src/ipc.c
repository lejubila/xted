/* What the channels of ipc_unix.c and ipc_win.c have in common: the request and the queue. */

#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC "xted 1\n"

static IpcFile *queue;
static size_t nqueue, capqueue;

void ipc_request(SBuf *b, const IpcFile *f, size_t n)
{
    size_t i;

    sbuf_adds(b, MAGIC);
    for (i = 0; i < n; i++)
        if (f[i].path && !strpbrk(f[i].path, "\t\n"))
            sbuf_addf(b, "%s\t%zu\t%zu\n", f[i].path, f[i].line, f[i].col);
    sbuf_add(b, "\n", 1);
}

int ipc_complete(const char *s, size_t n)
{
    return n >= sizeof MAGIC && memcmp(s, MAGIC, sizeof MAGIC - 1) == 0 && s[n - 1] == '\n' &&
           s[n - 2] == '\n';
}

size_t ipc_queue(const char *s, size_t n)
{
    size_t i = sizeof MAGIC - 1, added = 0;

    if (!ipc_complete(s, n))
        return 0;
    while (i < n && s[i] != '\n') {
        const char *line = s + i, *nl = memchr(line, '\n', n - i), *t1, *t2;
        size_t len = (size_t)(nl - line);

        t1 = memchr(line, '\t', len);
        t2 = t1 ? memchr(t1 + 1, '\t', (size_t)(nl - t1 - 1)) : NULL;
        if (t1 && t2 && t1 > line) {
            if (nqueue == capqueue) {
                capqueue = capqueue ? capqueue * 2 : 8;
                queue = xrealloc(queue, capqueue * sizeof *queue);
            }
            queue[nqueue].path = xstrndup(line, (size_t)(t1 - line));
            queue[nqueue].line = (size_t)strtoul(t1 + 1, NULL, 10);
            queue[nqueue].col = (size_t)strtoul(t2 + 1, NULL, 10);
            nqueue++;
            added++;
        }
        i = (size_t)(nl - s) + 1;
    }
    return added;
}

IpcFile *ipc_take(size_t *n)
{
    IpcFile *q = queue;

    *n = nqueue;
    queue = NULL;
    nqueue = capqueue = 0;
    return *n ? q : (free(q), (IpcFile *)NULL);
}
