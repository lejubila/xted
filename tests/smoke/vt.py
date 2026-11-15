"""A small terminal emulator for the smoke tests: runs xted on a pseudo-terminal, feeds it keys
and reads back the screen it drew (text and cursor), plus the raw bytes for checks on colors.

Only what xted writes is understood: cursor moves, erase of line and screen, SGR (ignored),
private modes (ignored) and OSC strings (skipped)."""
import fcntl
import os
import pty
import select
import signal
import struct
import termios
import time
import unicodedata


class Screen:
    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.cells = [[" "] * cols for _ in range(rows)]
        self.r = self.c = 0

    def _put(self, ch):
        w = 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
        if unicodedata.combining(ch):
            return
        if self.c + w > self.cols or self.r >= self.rows:
            return
        self.cells[self.r][self.c] = ch
        if w == 2 and self.c + 1 < self.cols:
            self.cells[self.r][self.c + 1] = ""
        self.c += w

    def feed(self, data):
        text = data.decode("utf-8", "replace")
        i, n = 0, len(text)
        while i < n:
            ch = text[i]
            if ch == "\x1b":
                if i + 1 < n and text[i + 1] == "[":
                    j = i + 2
                    while j < n and not ("@" <= text[j] <= "~"):
                        j += 1
                    if j >= n:
                        break
                    self._csi(text[i + 2:j], text[j])
                    i = j + 1
                    continue
                if i + 1 < n and text[i + 1] == "]":
                    j = i + 2
                    while j < n and text[j] != "\x07" and not (text[j] == "\x1b" and j + 1 < n and text[j + 1] == "\\"):
                        j += 1
                    i = j + (1 if j < n and text[j] == "\x07" else 2)
                    continue
                i += 3 if i + 1 < n and text[i + 1] in "()" else 2
                continue
            if ch == "\r":
                self.c = 0
            elif ch == "\n":
                self.r = min(self.r + 1, self.rows - 1)
            elif ch == "\b":
                self.c = max(self.c - 1, 0)
            elif ch >= " ":
                self._put(ch)
            i += 1

    def _csi(self, params, final):
        priv = params.startswith("?")
        nums = [int(p) if p.isdigit() else 0 for p in params.lstrip("?").split(";")] if params else []
        a = nums[0] if nums else 0
        if priv:
            return
        if final in "Hf":
            r = (nums[0] if len(nums) > 0 and nums[0] else 1) - 1
            c = (nums[1] if len(nums) > 1 and nums[1] else 1) - 1
            self.r, self.c = min(r, self.rows - 1), min(c, self.cols - 1)
        elif final == "K":
            row = self.cells[self.r]
            if a == 0:
                for k in range(self.c, self.cols):
                    row[k] = " "
            elif a == 1:
                for k in range(0, self.c + 1):
                    row[k] = " "
            else:
                self.cells[self.r] = [" "] * self.cols
        elif final == "J":
            if a == 2 or a == 3:
                self.cells = [[" "] * self.cols for _ in range(self.rows)]
            elif a == 0:
                for k in range(self.c, self.cols):
                    self.cells[self.r][k] = " "
                for r in range(self.r + 1, self.rows):
                    self.cells[r] = [" "] * self.cols
        elif final == "A":
            self.r = max(self.r - max(a, 1), 0)
        elif final == "B":
            self.r = min(self.r + max(a, 1), self.rows - 1)
        elif final == "C":
            self.c = min(self.c + max(a, 1), self.cols - 1)
        elif final == "D":
            self.c = max(self.c - max(a, 1), 0)

    def lines(self):
        return ["".join(row).rstrip() for row in self.cells]

    def text(self):
        return "\n".join(self.lines())


class Run:
    """One session of the editor; keys go in with type(), the screen comes back with screen()."""

    def __init__(self, exe, args, rows=24, cols=80, env=None, cwd=None):
        self.rows, self.cols = rows, cols
        self.out = b""
        pid, fd = pty.fork()
        if pid == 0:
            try:
                fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
                if cwd:
                    os.chdir(cwd)
                e = dict(os.environ)
                e.update({"TERM": "xterm-256color", "LANG": "C", "LC_ALL": "", "COLORTERM": ""})
                if env:
                    e.update(env)
                os.execve(exe, [exe] + list(args), e)
            finally:
                os._exit(127)
        self.pid, self.fd = pid, fd
        self.status = None
        self.reaped = False
        # keys sent before the editor takes the terminal (raw mode, alternate screen) would be
        # echoed by the tty instead of read: wait for its first frame
        end = time.time() + 10
        while time.time() < end and b"\x1b[?1049h" not in self.out:
            self._read(0.1)
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            if pid:                         # it ended at once (a wrong argument, no terminal)
                self.status = os.waitstatus_to_exitcode(st)
                self.reaped = True
                break
        self.wait(0.5)

    def _read(self, timeout):
        try:
            while select.select([self.fd], [], [], timeout)[0]:
                d = os.read(self.fd, 65536)
                if not d:
                    break
                self.out += d
                timeout = min(timeout, 0.15)
        except OSError:
            pass

    def wait(self, seconds=0.3):
        """Reads what the editor writes until it has been quiet for a while (at most seconds)."""
        end = time.time() + seconds
        while time.time() < end:
            before = len(self.out)
            self._read(0.15)
            if len(self.out) == before:
                break

    def type(self, *keys, delay=0.06):
        for k in keys:
            if isinstance(k, (int, float)):
                self.wait(k)
                continue
            try:
                os.write(self.fd, k.encode() if isinstance(k, str) else k)
            except OSError:
                break
            time.sleep(delay)
            self._read(0.05)
        self.wait(0.4)

    def screen(self):
        s = Screen(self.rows, self.cols)
        s.feed(self.out)
        return s

    def mark(self):
        return len(self.out)

    def since(self, mark):
        return self.out[mark:]

    def finish(self, timeout=3.0):
        """Waits for the editor to end (after the keys that quit it); kills it if it does not."""
        end = time.time() + timeout
        while not self.reaped and time.time() < end:
            self._read(0.1)
            pid, st = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.status = os.waitstatus_to_exitcode(st)
                break
        else:
            if not self.reaped:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
                self.status = None
        try:
            os.close(self.fd)
        except OSError:
            pass
        return self.status


# keys
ESC = "\x1b"
UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"
HOME, END, PGUP, PGDN = "\x1b[H", "\x1b[F", "\x1b[5~", "\x1b[6~"
ENTER, TAB, BS = "\r", "\t", "\x7f"


def ctrl(c):
    return chr(ord(c.lower()) - 96)


def alt(c):
    return "\x1b" + c
