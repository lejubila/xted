#!/usr/bin/env python3
"""Smoke tests of xted on a real pseudo-terminal: what the user sees and the files it writes.

Usage: tests/smoke/run.py [path/to/xted] [name ...]   (make smoke)
Every scenario runs in its own temporary folder, with its own configuration and state folders,
so the user's files and sessions are never touched."""
import os
import shutil
import sys
import tempfile
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vt import Run, ESC, UP, DOWN, END, ENTER, ctrl, alt  # noqa: E402

EXE = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].isidentifier() else "./xted")
ONLY = [a for a in sys.argv[1:] if a.isidentifier()]
SCENARIOS = []
failures = 0


def scenario(f):
    SCENARIOS.append(f)
    return f


def check(name, ok, detail=""):
    global failures
    print(("ok   " if ok else "FAIL ") + name)
    if not ok:
        failures += 1
        if detail:
            print("     " + detail.replace("\n", "\n     "))


class Box:
    """A temporary project folder with its own configuration and state."""

    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="xted-smoke-")
        self.cfg = os.path.join(self.dir, ".cfg")
        self.state = os.path.join(self.dir, ".state")
        os.makedirs(os.path.join(self.cfg, "xted"))
        os.makedirs(self.state)

    def path(self, name):
        return os.path.join(self.dir, name)

    def write(self, name, text):
        p = self.path(name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(text)
        return p

    def read(self, name):
        with open(self.path(name)) as f:
            return f.read()

    def run(self, args, rows=24, cols=80, env=None):
        e = {"XDG_CONFIG_HOME": self.cfg, "XDG_STATE_HOME": self.state}
        if env:
            e.update(env)
        return Run(EXE, args, rows, cols, e, cwd=self.dir)

    def close(self):
        shutil.rmtree(self.dir, ignore_errors=True)


def quit_(r, discard=False):
    r.type(ESC, ctrl("q"))
    if discard:
        r.type("n")
    return r.finish()


@scenario
def files_and_quit():
    b = Box()
    b.write("old.txt", "old\n")
    r = b.run(["old.txt"])
    t = r.screen().text()
    check("the hint of the keys is shown", "Ctrl+S" in t and "Alt+X" in t, t[-200:])
    quit_(r)
    r = b.run(["notes.txt"])
    t = r.screen().text()
    check("a new file opens with its name in the status bar", "notes.txt" in t and "New file" in t)
    r.type("hello world", ctrl("s"))
    check("Ctrl+S writes the file", os.path.exists(b.path("notes.txt")) and b.read("notes.txt").startswith("hello world"))
    r.type("!", ctrl("q"))
    check("quitting with changes asks", "unsaved" in r.screen().text().lower())
    r.type("n")
    st = r.finish()
    check("answering no quits without saving", st == 0 and b.read("notes.txt").startswith("hello world") and
          "!" not in b.read("notes.txt"), "exit %r" % st)
    b.close()


@scenario
def palette():
    b = Box()
    b.write("a.py", "x = 1\n")
    r = b.run(["a.py"])
    r.type(alt("x"))
    t = r.screen().text()
    check("Alt+X opens the palette in sections", "Actions · File" in t and "Save" in t and "Ctrl+S" in t)
    r.type("toggle auto-p")
    t = r.screen().text()
    check("a setting shows its value", "Toggle auto-pairs" in t and " on" in t)
    r.type(ENTER)
    t = r.screen().text()
    check("Enter on a setting changes it and the palette stays open", "Command" in t and " off" in t, t[-400:])
    r.type(ESC)
    check("Esc closes the palette", "Command ›" not in r.screen().text())
    r.type(alt("x"))
    t = r.screen().text()
    check("the palette opens again on the setting used last", "Toggle auto-pairs" in t)
    quit_(r)
    b.close()


@scenario
def italian():
    b = Box()
    b.write("a.txt", "x\n")
    r = b.run(["a.txt"], cols=140, env={"LANG": "it_IT.UTF-8"})
    r.type(alt("x"))
    t = r.screen().text()
    check("with LANG=it the palette is in Italian", "Azioni · File" in t and "Salva" in t, t[-300:])
    r.type("lingua")
    t = r.screen().text()
    check("the row of the language names its catalog file", "Italiano" in t and "lang/it" in t, t[-300:])
    quit_(r)
    b.close()


@scenario
def symbols_and_buffers():
    b = Box()
    b.write("m.c", "#include <stdio.h>\n\nstatic int add(int a, int b)\n{\n    return a + b;\n}\n\n"
                   "int main(void)\n{\n    return add(1, 2);\n}\n")
    b.write("n.txt", "other\n")
    r = b.run(["m.c", "n.txt"])
    r.type(ctrl("e"))
    t = r.screen().text()
    check("Ctrl+E lists the buffers", "Buffer" in t and "m.c" in t and "n.txt" in t)
    r.type(ESC, ctrl("t"))
    t = r.screen().text()
    check("Ctrl+T lists the functions with their parameters and lines",
          "Symbols" in t and "add" in t and "(int a, int b)" in t and "main" in t, t[-500:])
    r.type("mai", ENTER)
    check("choosing a symbol moves the cursor to it", "Ln 8" in r.screen().text())
    quit_(r)
    b.close()


@scenario
def search_bar():
    b = Box()
    b.write("s.txt", "alpha beta\nbeta gamma\n")
    r = b.run(["s.txt"], cols=100)
    r.type(ctrl("f"), "beta")
    t = r.screen().text()
    check("the search bar counts the matches", "1/2" in t, t[-200:])
    check("the search bar shows the keys it takes", "Alt+C" in t and "Alt+W" in t and "Esc" in t)
    r.type(ESC)
    quit_(r)
    b.close()


@scenario
def light_theme():
    b = Box()
    b.write(".cfg/xted/config", "theme = light\n")
    b.write("a.txt", "x\n")
    r = b.run(["a.txt"], env={"COLORTERM": "truecolor"})
    r.type(alt("x"))
    raw = r.out
    check("the bars use the theme's text color as background", b"48;2;56;58;66" in raw)
    check("the cursor gets the theme's color (OSC 12)", b"\x1b]12;#383a42" in raw)
    quit_(r)
    b.close()


@scenario
def save_settings():
    b = Box()
    b.write("a.py", "x = 1\n")
    r = b.run(["a.py"])
    r.type(alt("x"), "indentation: 2", ENTER, ESC)
    r.type(alt("x"), "save settings", ENTER)
    t = r.screen().text()
    check("Save settings asks where", "Save where" in t and "User" in t and "Project" in t, t[-500:])
    r.type(ENTER)
    cfg = b.path(".cfg/xted/config")
    check("the user's file gets the indentation", os.path.exists(cfg) and "indent_width = 2" in open(cfg).read())
    quit_(r)
    b.close()


@scenario
def imports():
    b = Box()
    b.write("a.c", "int main(void)\n{\n    \n}\n")
    r = b.run(["a.c"])
    r.type(DOWN, DOWN, END, "strle", 0.3, ENTER, ctrl("s"))
    check("C: choosing strlen adds #include <string.h>", b.read("a.c").startswith("#include <string.h>\n"), b.read("a.c"))
    r.type(ctrl("z"), ctrl("s"))
    check("one Undo takes back the include and the call (the typed word stays)",
          b.read("a.c") == "int main(void)\n{\n    strle\n}\n", b.read("a.c"))
    quit_(r, discard=True)
    b.write("p.py", "def f():\n    \n")
    r = b.run(["p.py"])
    r.type(DOWN, END, "os.getc", 0.3, ENTER, ctrl("s"))
    check("Python: os.getcwd adds import os", b.read("p.py").startswith("import os\n"), b.read("p.py"))
    quit_(r, discard=True)
    b.write("w.js", "const path = require('path');\n\nfunction f() {\n    \n}\n")
    r = b.run(["w.js"])
    r.type(DOWN, DOWN, DOWN, END, "fs.readF", 0.3, ENTER, ctrl("s"))
    check("JavaScript: fs.readFile adds the require in the file's style",
          "const fs = require('fs');" in b.read("w.js"), b.read("w.js"))
    quit_(r, discard=True)
    b.close()


@scenario
def go_and_rust():
    b = Box()
    b.write("main.go", "package main\n\nimport \"os\"\n\nfunc main() {\n\t\n}\n")
    r = b.run(["main.go"])
    r.type(DOWN, DOWN, DOWN, DOWN, DOWN, END, "fmt.Printl", 0.3, ENTER, ctrl("s"))
    check("Go: fmt.Println turns the import into a block with \"fmt\"",
          b.read("main.go").startswith("package main\n\nimport (\n\t\"fmt\"\n\t\"os\"\n)\n"), b.read("main.go"))
    quit_(r, discard=True)
    b.write("main.rs", "fn main() {\n    \n}\n")
    r = b.run(["main.rs"])
    r.type(DOWN, END, "HashMap::ne", 0.3, ENTER, ctrl("s"))
    check("Rust: HashMap::new adds use std::collections::HashMap;",
          b.read("main.rs").startswith("use std::collections::HashMap;\n\nfn main()"), b.read("main.rs"))
    quit_(r, discard=True)
    b.close()


@scenario
def help_page():
    b = Box()
    b.write("a.txt", "text\n")
    r = b.run(["a.txt"], rows=40)
    r.type("\x1bOP")
    t = r.screen().text()
    check("F1 opens the help page with the commands and their keys",
          "xted: help" in t and "Ctrl+S" in t and "Save" in t and "[Help]" in t, t[:600])
    r.type("x")
    check("the help page cannot be changed", "read-only" in r.screen().text())
    r.type(ctrl("w"))
    t = r.screen().text()
    check("Ctrl+W closes it and the file is back", "[Help]" not in t and "text" in t)
    quit_(r)
    r = b.run(["a.txt"], rows=40, env={"LANG": "it_IT.UTF-8"})
    r.type("\x1bOP")
    t = r.screen().text()
    check("the help page follows the language", "xted: guida" in t and "Salva" in t, t[:400])
    quit_(r)
    b.close()


@scenario
def panes_and_quick_open():
    b = Box()
    b.write("one.txt", "1\n")
    b.write("sub/two.txt", "2\n")
    r = b.run(["one.txt"])
    r.type(alt("\\"))
    check("Alt+\\ splits the pane", "│" in r.screen().text())
    r.type(ctrl("p"), "two", 0.5)
    t = r.screen().text()
    check("Ctrl+P finds the files of the project", "sub/two.txt" in t, t[-300:])
    r.type(ENTER)
    check("choosing one opens it", "two.txt" in r.screen().lines()[0])
    quit_(r)
    b.close()


def main():
    if not os.access(EXE, os.X_OK):
        print("no executable: " + EXE)
        return 2
    for f in SCENARIOS:
        if ONLY and f.__name__ not in ONLY:
            continue
        print("== " + f.__name__)
        try:
            f()
        except Exception:
            check(f.__name__ + " ran to the end", False, traceback.format_exc())
    print("%s" % ("smoke: all passed" if not failures else "smoke: %d failed" % failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
