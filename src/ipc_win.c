/* The channel of ipc.h on Windows: a named pipe of the user and of the session,
 * \\.\pipe\xted-<user>-<session>, local only. The listening xted waits on its connection with
 * an event (term_watch_server), together with the console. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEND_TIMEOUT_MS 2000
#define SERVE_TIMEOUT_MS 1000
#define MAX_REQUEST (256u << 10)

static HANDLE pipe_h = INVALID_HANDLE_VALUE, conn_ev;
static OVERLAPPED conn_ov;

static void pipe_name(char *out, size_t n)
{
    const char *x = getenv("XTED_IPC"), *u = getenv("USERNAME");
    DWORD session = 0;
    char user[64];
    size_t i, k = 0;

    if (x && x[0]) {
        snprintf(out, n, "\\\\.\\pipe\\%s", x);
        return;
    }
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    for (i = 0; u && u[i] && k + 1 < sizeof user; i++)
        user[k++] = (u[i] == '\\' || u[i] == '/') ? '_' : u[i];
    user[k] = '\0';
    snprintf(out, n, "\\\\.\\pipe\\xted-%s-%lu", user, (unsigned long)session);
}

/* An overlapped read or write on h, waiting up to ms; the bytes moved, -1 on failure. */
static long io(HANDLE h, int wr, void *buf, DWORD n, DWORD ms)
{
    OVERLAPPED ov;
    DWORD done = 0;
    BOOL r;

    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent)
        return -1;
    r = wr ? WriteFile(h, buf, n, NULL, &ov) : ReadFile(h, buf, n, NULL, &ov);
    if (!r && GetLastError() == ERROR_IO_PENDING &&
        WaitForSingleObject(ov.hEvent, ms) != WAIT_OBJECT_0) {
        CancelIo(h);
        GetOverlappedResult(h, &ov, &done, TRUE);
        CloseHandle(ov.hEvent);
        return -1;
    }
    r = GetOverlappedResult(h, &ov, &done, FALSE);
    CloseHandle(ov.hEvent);
    return r ? (long)done : -1;
}

int ipc_send(const IpcFile *f, size_t n)
{
    char name[300], ans[8];
    SBuf req = SBUF_INIT;
    HANDLE h;
    long got = 0, r;
    size_t sent = 0;
    int ok = 0;

    pipe_name(name, sizeof name);
    h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeA(name, 1000))
        h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ipc_request(&req, f, n);
    while (sent < req.len) {
        r = io(h, 1, req.p + sent, (DWORD)(req.len - sent), SEND_TIMEOUT_MS);
        if (r <= 0)
            break;
        sent += (size_t)r;
    }
    if (sent == req.len) {
        while (got < 3) {
            r = io(h, 0, ans + got, (DWORD)(sizeof ans - 1 - (size_t)got), SEND_TIMEOUT_MS);
            if (r <= 0)
                break;
            got += r;
        }
        ok = got >= 3 && memcmp(ans, "ok\n", 3) == 0;
    }
    sbuf_free(&req);
    CloseHandle(h);
    return ok;
}

/* Waits for the next client, without blocking: the event is set when one connects. */
static void await_client(void)
{
    memset(&conn_ov, 0, sizeof conn_ov);
    conn_ov.hEvent = conn_ev;
    ResetEvent(conn_ev);
    if (!ConnectNamedPipe(pipe_h, &conn_ov)) {
        DWORD e = GetLastError();

        if (e == ERROR_PIPE_CONNECTED)
            SetEvent(conn_ev);              /* it came before the call */
        else if (e != ERROR_IO_PENDING)
            SetEvent(conn_ev);              /* broken: ipc_serve sees it and starts again */
    }
}

intptr_t ipc_listen(void)
{
    char name[300];

    if (pipe_h != INVALID_HANDLE_VALUE)
        return (intptr_t)conn_ev;
    pipe_name(name, sizeof name);
    /* the first instance only: if the pipe is there, another xted listens */
    pipe_h = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                              1, 4096, 4096, 0, NULL);
    if (pipe_h == INVALID_HANDLE_VALUE)
        return -1;
    conn_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!conn_ev) {
        CloseHandle(pipe_h);
        pipe_h = INVALID_HANDLE_VALUE;
        return -1;
    }
    await_client();
    return (intptr_t)conn_ev;
}

void ipc_serve(void)
{
    SBuf b = SBUF_INIT;
    char buf[4096];
    DWORD done;
    ULONGLONG end = GetTickCount64() + SERVE_TIMEOUT_MS;
    long r;

    if (pipe_h == INVALID_HANDLE_VALUE || WaitForSingleObject(conn_ev, 0) != WAIT_OBJECT_0)
        return;
    if (GetOverlappedResult(pipe_h, &conn_ov, &done, FALSE) || GetLastError() == ERROR_PIPE_CONNECTED) {
        while (b.len < MAX_REQUEST && !ipc_complete(b.p, b.len) && GetTickCount64() < end) {
            r = io(pipe_h, 0, buf, sizeof buf, (DWORD)(end - GetTickCount64()));
            if (r <= 0)
                break;
            sbuf_add(&b, buf, (size_t)r);
        }
        if (b.p && ipc_complete(b.p, b.len)) {
            ipc_queue(b.p, b.len);
            io(pipe_h, 1, "ok\n", 3, SERVE_TIMEOUT_MS);
            FlushFileBuffers(pipe_h);
        }
    }
    sbuf_free(&b);
    DisconnectNamedPipe(pipe_h);
    await_client();
}

void ipc_close(void)
{
    if (pipe_h == INVALID_HANDLE_VALUE)
        return;
    CloseHandle(pipe_h);
    CloseHandle(conn_ev);
    pipe_h = INVALID_HANDLE_VALUE;
}
