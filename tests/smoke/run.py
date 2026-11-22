#!/usr/bin/env python3
"""Smoke tests of xted on a real pseudo-terminal: what the user sees and the files it writes.

Usage: tests/smoke/run.py [path/to/xted] [name ...]   (make smoke)
Every scenario runs in its own temporary folder, with its own configuration and state folders,
so the user's files and sessions are never touched."""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vt import Run, ESC, UP, DOWN, RIGHT, END, ENTER, ctrl, alt  # noqa: E402

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
        # the channel of the single instance is the box's: never the one of a real xted
        # and no graphical session: the copies never reach the clipboard of the desktop
        e = {"XDG_CONFIG_HOME": self.cfg, "XDG_STATE_HOME": self.state,
             "XTED_IPC": os.path.join(self.dir, ".ipc.sock"), "WAYLAND_DISPLAY": "", "DISPLAY": ""}
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

    # symbols of other files: a package of the Go module, a module of the Rust crate
    b = Box()
    b.write("go.mod", "module example.com/app\n")
    b.write("store/store.go", "package store\n\nfunc Open(path string) error {\n\treturn nil\n}\n")
    b.write("cmd/main.go", "package main\n\nfunc main() {\n\t\n}\n")
    r = b.run(["cmd/main.go"])
    r.type(DOWN, DOWN, DOWN, END, "store.Op", 0.4, ENTER, ctrl("s"))
    check("Go: store.Open of the module adds import \"example.com/app/store\"",
          b.read("cmd/main.go").startswith("package main\n\nimport \"example.com/app/store\"\n\nfunc main()"),
          b.read("cmd/main.go"))
    quit_(r, discard=True)
    b.write("Cargo.toml", "[package]\nname = \"app\"\n")
    b.write("src/net.rs", "pub fn fetch(url: &str) -> String {\n    String::new()\n}\n")
    b.write("src/main.rs", "mod net;\n\nfn main() {\n    \n}\n")
    r = b.run(["src/main.rs"])
    r.type(DOWN, DOWN, DOWN, END, "fetc", 0.4, ENTER, ctrl("s"))
    check("Rust: fetch of another module adds use crate::net::fetch; after the mods",
          b.read("src/main.rs").startswith("mod net;\n\nuse crate::net::fetch;\n\nfn main()"), b.read("src/main.rs"))
    quit_(r, discard=True)
    b.close()


@scenario
def java_imports():
    b = Box()
    b.write("pom.xml", "<project/>\n")
    b.write("src/main/java/com/acme/util/TextUtils.java",
            "package com.acme.util;\n\npublic class TextUtils {\n    public static String shout(String s) {\n"
            "        return s;\n    }\n}\n")
    b.write("src/main/java/com/acme/app/Main.java",
            "package com.acme.app;\n\nimport java.util.Map;\n\npublic class Main {\n    void f() {\n        \n    }\n}\n")
    r = b.run(["src/main/java/com/acme/app/Main.java"])
    r.type(DOWN, DOWN, DOWN, DOWN, DOWN, DOWN, END, "Collections.so", 0.4, ENTER, ctrl("s"))
    check("Java: Collections.sort adds import java.util.Collections; in order",
          "import java.util.Collections;\nimport java.util.Map;\n" in b.read("src/main/java/com/acme/app/Main.java"),
          b.read("src/main/java/com/acme/app/Main.java"))
    quit_(r, discard=True)
    b.write("src/main/java/com/acme/app/Main.java",
            "package com.acme.app;\n\nimport java.util.Map;\n\npublic class Main {\n    void f() {\n        \n    }\n}\n")
    r = b.run(["src/main/java/com/acme/app/Main.java"])
    r.type(DOWN, DOWN, DOWN, DOWN, DOWN, DOWN, END, "TextUt", 0.4, ENTER, ctrl("s"))
    check("Java: a class of another package adds its import in a group of its own",
          "import java.util.Map;\n\nimport com.acme.util.TextUtils;\n\npublic class Main" in
          b.read("src/main/java/com/acme/app/Main.java"), b.read("src/main/java/com/acme/app/Main.java"))
    quit_(r, discard=True)
    b.close()


@scenario
def csharp_usings():
    b = Box()
    b.write("Acme.sln", "\n")
    b.write("App/App.csproj", "<Project Sdk=\"Microsoft.NET.Sdk\">\n  <PropertyGroup>\n"
            "    <ImplicitUsings>enable</ImplicitUsings>\n  </PropertyGroup>\n</Project>\n")
    src = "using System.Text;\n\nnamespace Acme.App;\n\npublic class P\n{\n    void F()\n    {\n        \n    }\n}\n"
    b.write("App/P.cs", src)
    r = b.run(["App/P.cs"])
    r.type(DOWN * 8, END, "Regex.IsMat", 0.4, ENTER, ctrl("s"))
    check("C#: Regex.IsMatch adds using System.Text.RegularExpressions; in order",
          b.read("App/P.cs").startswith("using System.Text;\nusing System.Text.RegularExpressions;\n\nnamespace"),
          b.read("App/P.cs"))
    quit_(r, discard=True)
    b.write("App/P.cs", src)
    r = b.run(["App/P.cs"])
    r.type(DOWN * 8, END, "File.ReadAllTe", 0.4, ENTER, ctrl("s"))
    check("C#: File needs no using with the project's implicit usings",
          b.read("App/P.cs").startswith("using System.Text;\n\nnamespace") and "File.ReadAllText(" in b.read("App/P.cs"),
          b.read("App/P.cs"))
    quit_(r, discard=True)
    b.close()


@scenario
def dotenv_file():
    b = Box()
    b.write(".env.local", "# local settings\nAPP_NAME=\"My App\"\nDB_PORT=5432\nexport API_URL=http://x/#a\n")
    r = b.run([".env.local"])
    t = r.screen().text()
    check(".env.local is a Dotenv file", "Dotenv" in t, t[-400:])
    r.type(ctrl("t"), 0.3)
    t = r.screen().text()
    check("Ctrl+T lists its variables", "APP_NAME" in t and "DB_PORT" in t and "API_URL" in t, t[-600:])
    r.type(ESC)
    quit_(r)
    b.write(".git/HEAD", "ref: refs/heads/main\n")
    b.write("config.php", "<?php\n\n")
    r = b.run(["config.php"])
    r.type(DOWN, "$port = env('DB", 0.3, "_", 0.4)
    t = r.screen().text()
    check("env(' suggests the variables of .env with their file", "DB_PORT" in t and ".env.local" in t, t[-600:])
    r.type(ENTER, ctrl("s"))
    check("accepting writes the name", "env('DB_PORT')" in b.read("config.php"), b.read("config.php"))
    quit_(r, discard=True)
    b.close()


@scenario
def palette_colors():
    b = Box()
    b.write("a.c", "int main(void) { return 0; }\n")
    r = b.run(["a.c"])
    r.type(alt("x"), "Colors", 0.3)
    t = r.screen().text()
    check("Alt+X has Colors with what auto found", "Colors" in t and "auto (256)" in t, t[-800:])
    r.type(ENTER, 0.3)
    t = r.screen().text()
    check("Enter switches to truecolor at once", "truecolor" in t and b"\x1b[38;2;" in r.out[-6000:], t[-800:])
    r.type(ESC)
    quit_(r)
    b.close()


@scenario
def alt_arrows_putty():
    b = Box()
    b.write("a.txt", "first\n")
    b.write("b.txt", "second\n")
    r = b.run(["a.txt"])
    # two panes, a.txt and b.txt: Alt+Right and Alt+Left move between them
    r.type(alt("\\"), 0.2, ctrl("p"), "b.txt", 0.5, ENTER, 0.3)

    def active():
        # the pane the cursor is in: left or right of the middle
        return "left" if r.screen().c < r.cols // 2 else "right"

    # PuTTY: ESC before the arrow's own sequence; xterm and KiTTY: CSI 1;3C
    for seq, label in (("\x1b\x1b[C", "ESC ESC [ C"), ("\x1b[1;3C", "CSI 1;3C")):
        before = active()
        r.type(seq, 0.3)
        after = active()
        check("Alt+Right as " + label + " goes to the other document",
              before != after and "[C" not in r.screen().text() and "OC" not in r.screen().text(), after)
    before = active()
    r.type("\x1b\x1b[D", 0.3)
    check("Alt+Left as ESC ESC [ D goes back", active() != before and "[D" not in r.screen().text(), active())
    quit_(r)
    check("no text was typed into the files", b.read("a.txt") == "first\n" and b.read("b.txt") == "second\n",
          b.read("a.txt") + b.read("b.txt"))
    # PuTTY's Ctrl+Right is ESC O C (the cursor keys are in normal mode): a word to the right
    b.write("w.txt", "alpha beta gamma\n")
    r = b.run(["w.txt"])
    c0 = r.screen().c
    r.type("\x1bOC", 0.3)
    c1 = r.screen().c
    r.type(RIGHT, 0.3)
    c2 = r.screen().c
    check("ESC O C is Ctrl+Right (a word), ESC [ C one character", c1 - c0 >= 5 and c2 - c1 == 1, (c0, c1, c2))
    quit_(r)
    b.close()


@scenario
def glyphs_ascii():
    b = Box()
    b.write("src/main.c", "int main(void) { return 0; }\n")
    b.write("docs/a.txt", "x\n")
    r = b.run(["src/main.c"])
    r.type(ctrl("b"), 0.4)
    t = r.screen().text()
    check("the file browser marks folders with \u25b8 and draws a \u2502 border (unicode, the default)",
          "\u25b8 docs" in t and "\u2502" in t, t[:600])
    r.type(ESC, alt("x"), "Glyphs", 0.3, ENTER, 0.3, ESC, 0.3)
    t = r.screen().text()
    check("Glyphs: windows draws \u25ba, which every Windows font has", "\u25ba docs" in t and "\u2502" in t
          and "\u25b8" not in t, t[:600])
    r.type(alt("x"), "Glyphs", 0.3, ENTER, 0.3, ESC, 0.3)
    t = r.screen().text()
    check("Glyphs: ascii draws + and | in their place", "+ docs" in t and "|" in t and "\u2502" not in t
          and "\u25ba" not in t, t[:600])
    r.type(ctrl("q"), 0.3)
    b.close()


@scenario
def keys_row():
    b = Box()
    b.write("a.txt", "text\n")
    r = b.run(["a.txt"], cols=120)
    last = lambda: r.screen().lines()[-1]
    check("at start the message row lists the common keys", "Ctrl+S save" in last() and "F1 help" in last()
          and "Alt+X commands" in last() and "Ctrl+F find" in last(), last())
    time.sleep(5.6)                     # longer than a message lasts (r.type(n) stops when quiet)
    r.wait(0.3)
    check("they stay, with no time limit", "Ctrl+S save" in last(), last())
    r.type(ctrl("s"), 0.3)
    check("a message goes on the left, the keys stay on the right", last().startswith("1 lines") and
          "written" in last() and "Ctrl+Q quit" in last(), last())
    time.sleep(5.6)
    r.wait(0.3)
    check("and when it is gone they come back", "Ctrl+S save" in last(), last())
    quit_(r)
    r = b.run(["a.txt"], cols=50)
    check("a narrow terminal shows the first ones that fit", "^S save" in last() and "^G" not in last()
          and len(last().rstrip()) <= 50, last())
    quit_(r)
    b.close()


@scenario
def version_and_credits():
    import subprocess
    out = subprocess.run([EXE, "--version"], capture_output=True, text=True, env=dict(os.environ, LANG="C", LC_ALL=""))
    check("xted --version prints the version and the credits",
          out.returncode == 0 and out.stdout.startswith("xted 0.12.0\nCopyright (C) 2026 David Bigagli\nMIT license"),
          out.stdout)
    b = Box()
    r = b.run([], cols=100)
    t = r.screen().text()
    check("with nothing to open the start screen shows name, version, credits and keys",
          "xted 0.12.0" in t and "a text editor for the terminal" in t and "Copyright (C) 2026 David Bigagli" in t
          and "F1 help" in t, t)
    # the terminal reports the focus (ESC [ I) as soon as xted starts: that is no key
    r.type("\x1b[I", 0.3, "\x1b[O", 0.2, "\x1b[I", 0.3)
    check("the focus events the terminal sends leave the start screen", "xted 0.12.0" in r.screen().text(),
          r.screen().text())
    r.type("x", 0.3)
    t = r.screen().text()
    check("the first key takes it away", "xted 0.12.0" not in t and "Copyright" not in t, t)
    r.type(alt("x"), "About", 0.3, ENTER, 0.3)
    t = r.screen().text()
    check("About xted shows the version and the credits in a page of its own",
          "[About]" in t and "xted 0.12.0" in t and "MIT license" in t, t[:800])
    r.type(ctrl("w"), ctrl("q"), 0.3, "n", 0.3)
    b.write("a.txt", "x\n")
    r = b.run(["a.txt"])
    check("opening a file shows no start screen", "Copyright" not in r.screen().text())
    quit_(r)
    b.close()
    b = Box()                           # no session to restore: the start screen again
    r = b.run([], cols=100, env={"LANG": "it_IT.UTF-8"})
    t = r.screen().text()
    check("the start screen follows the language", "un editor di testo per il terminale" in t
          and "licenza MIT" in t, t)
    r.type(alt("x"), "Informazioni", 0.3, ENTER, 0.3)
    t = r.screen().text()
    check("Informazioni su xted in Italian", "Licenza MIT" in t, t[:800])
    r.type(ctrl("w"), ctrl("q"), 0.3)
    b.close()


@scenario
def yaml_file():
    b = Box()
    b.write(".github/workflows/ci.yml", "name: CI\non: [push]\njobs:\n  build:\n    runs-on: ubuntu-latest\n"
            "    steps:\n      - uses: actions/checkout@v4\n      - run: |\n          make\n          make test\n")
    r = b.run([".github/workflows/ci.yml"])
    t = r.screen().text()
    check("a .yml file is YAML", "YAML" in t, t[-300:])
    r.type(ctrl("t"), 0.3)
    t = r.screen().text()
    check("Ctrl+T lists its keys, dashes included", "runs-on" in t and "steps" in t and "jobs" in t, t[-700:])
    r.type(ESC)
    quit_(r)
    b.close()


@scenario
def json_file():
    b = Box()
    b.write("package.json", '{"name": "app", "version": "1.0.0", "scripts": {"test": "make test"}}\n')
    r = b.run(["package.json"])
    r.type(alt("x"), "Format JSON", 0.3, ENTER, 0.3, ctrl("s"), 0.3)
    check("Format JSON writes one key a line, indented",
          b.read("package.json").startswith('{\n  "name": "app",\n  "version": "1.0.0",\n  "scripts": {\n'),
          b.read("package.json"))
    r.type(ctrl("t"), 0.3)
    t = r.screen().text()
    check("Ctrl+T lists the keys", "version" in t and "scripts" in t and "test" in t, t[-600:])
    r.type(ESC, ctrl("g"), "2", ENTER, END, ",", 0.2, ctrl("s"), 0.3)
    last = r.screen().lines()[-1]
    check("saving an invalid JSON says where the error is", "JSON: line 2, column 17" in last, last)
    r.type(ctrl("g"), "1", ENTER, alt("x"), "Check JSON", 0.3, ENTER, 0.3)
    t = r.screen().text()
    check("Check JSON moves the cursor to the error", "Ln 2, Col 17" in t, t[-300:])
    quit_(r, discard=True)
    b.close()


@scenario
def markdown_preview():
    b = Box()
    b.write("notes.md", "# Notes\n\nSome **bold** text.\n\n- first\n- second\n")
    b.write("a.txt", "plain\n")
    r = b.run(["notes.md"], cols=120)
    r.type(alt("u"), 0.4)
    t = r.screen().text()
    rows = r.screen().lines()
    check("Alt+U opens the rendered preview beside the document",
          "[Preview] notes.md" in t and "\u2022 first" in t and "Some bold text." in t, t[:900])
    check("the source keeps the cursor", r.screen().c < 60, r.screen().c)
    r.type(DOWN, DOWN, END, " More", 0.4)
    check("the preview follows the edits", "Some bold text. More" in r.screen().text(), r.screen().text()[:900])
    r.type(alt("u"), 0.3)
    check("Alt+U again closes it", "[Preview]" not in r.screen().text())
    r.type(alt("u"), 0.3)
    quit_(r, discard=True)
    r = b.run([], cols=120)
    t = r.screen().text()
    check("the session brings the preview back as a preview", "[Preview] notes.md" in t and "\u2022 first" in t,
          t[:600])
    quit_(r)
    r = b.run(["a.txt"])
    r.type(alt("u"), 0.3)
    check("a file that is not Markdown has no preview", "Not a Markdown document" in r.screen().text())
    quit_(r)
    b.close()


def click_label(r, label):
    """Clicks (press and release) on the button of the message row that shows label."""
    row = r.screen().lines()[-1]
    x = row.find(label)
    if x < 0:
        return False
    y = r.rows
    r.type("\x1b[<0;%d;%dM" % (x + 2, y), 0.05, "\x1b[<0;%d;%dm" % (x + 2, y), 0.3)
    return True


@scenario
def context_keys():
    b = Box()
    b.write("a.txt", "text here\nthe end\n")
    b.write("b.txt", "other\n")
    r = b.run(["a.txt", "b.txt"], cols=120)
    last = lambda: r.screen().lines()[-1]
    check("editing: the keys of editing", "Ctrl+S save" in last() and "Ctrl+F find" in last(), last())
    click_label(r, "Ctrl+F find")
    check("a click on Ctrl+F find opens the search", "Find:" in last(), last())
    check("a prompt with no hints of its own has Enter and Esc as buttons", "Esc" in last(), last())
    r.type(ESC, 0.2)
    r.type("\x1b[1;2C" * 4, 0.2)               # Shift+Right: a selection
    check("a selection: copy, cut, comment", "Ctrl+C copy" in last() and "Ctrl+X cut" in last(), last())
    click_label(r, "Ctrl+C copy")
    r.type(ctrl("e"), 0.3)
    check("a list: Enter, the arrows, Esc", "Enter choose" in last() and "Esc close" in last(), last())
    click_label(r, "Esc close")
    check("a click on Esc close closes the list", "Enter choose" not in last(), last())
    r.type(DOWN, END, ENTER, "te", 0.4)
    check("the suggestions: Tab accept", "Tab accept" in last(), last())
    click_label(r, "Tab accept")
    r.type(" ", ctrl("v"), 0.2, ctrl("s"), 0.3)
    check("Tab accept completed the word, Ctrl+C copy copied", "\ntext text\n" in b.read("a.txt"), b.read("a.txt"))
    r.type("x", ctrl("q"), 0.3)
    check("a question: its answers as buttons", "y yes" in last() and "n no" in last() and "Esc cancel" in last(), last())
    click_label(r, "n no")
    r.finish()
    check("a click on n no answers it: xted quits without saving", r.status == 0 and "textx" not in b.read("a.txt"),
          b.read("a.txt"))
    b.close()


@scenario
def context_keys_more():
    b = Box()
    b.write("n.md", "# T\n")
    b.write("d.json", "{}\n")
    b.write("m.c", "int main(void)\n{\n    return 0;\n}\n")
    b.write("w.txt", "one two one two\n")
    last = lambda: r.screen().lines()[-1]
    for name, want in (("n.md", "Alt+U preview"), ("d.json", "format"), ("m.c", "Ctrl+T symbols")):
        r = b.run([name], cols=140)
        check(name + ": the keys of its language", want in last(), last())
        if name == "d.json":
            # a command with no key: the button runs it
            click_label(r, "format")
            check("a click on format (no key) runs Format JSON", "JSON: 1 lines" in last(), last())
        quit_(r)
    r = b.run(["m.c"], cols=140)
    r.type(alt("\\"), 0.2, alt("e"), 0.2)
    check("resize mode: the arrows and Esc", "Esc done" in last(), last())
    r.type(ESC, 0.2, alt("q"), 0.2, ctrl("b"), 0.4)
    check("the file browser: open, close folder, back to the text", "Enter open" in last() and
          "Esc back to the text" in last(), last())
    click_label(r, "Esc back to the text")
    check("a click on it gives the keys back to the text", "Ctrl+S save" in last(), last())
    r.type(ctrl("b"), 0.2, ctrl("b"), 0.2)
    r.type(DOWN, END, ENTER, "for", 0.3, "\t", 0.3)
    check("a snippet: Tab next field, Esc", "Tab next field" in last(), last())
    quit_(r, discard=True)
    r = b.run(["w.txt"], cols=140)
    r.type(alt("w"), 0.2, ctrl("d"), 0.3)
    check("several cursors: next occurrence, Esc one cursor", "Esc one cursor" in last(), last())
    r.type(ESC, ESC, 0.2, ctrl("f"), 0.3)
    check("the search bar: its keys", "Alt+C" in last(), last())
    before = r.out
    click_label(r, "Alt+C")
    check("a click on Alt+C turns the option on (shown bold)", len(r.out) > len(before) and "Alt+C" in last(), last())
    quit_(r)
    b.close()


@scenario
def keybar_options():
    b = Box()
    b.write("a.txt", "one\ntwo\n")
    b.write(".cfg/xted/config", "keybar = line\n")
    r = b.run(["a.txt"], cols=120)
    lines = lambda: r.screen().lines()
    check("keybar = line: the keys in a row of their own above the messages", "Ctrl+S save" in lines()[-2]
          and "Ctrl+S" not in lines()[-1], "\n".join(lines()[-3:]))
    r.type(ctrl("s"), 0.3)
    check("a message leaves them in their row", "written" in lines()[-1] and "Ctrl+S save" in lines()[-2],
          "\n".join(lines()[-2:]))
    row = lines()[-2]
    x = row.find("Ctrl+F find")
    r.type("\x1b[<0;%d;%dM" % (x + 2, r.rows - 1), 0.3)
    check("a click on their row works too, and holds the button down until let go",
          "Find:" in lines()[-1] and b"\x1b[7m" in r.out, lines()[-1])
    r.type("\x1b[<0;%d;%dm" % (x + 2, r.rows - 1), 0.2, ESC, 0.2)
    r.type(alt("x"), "key bar", 0.3, ENTER, 0.3)
    check("Key bar in the palette: off hides them", "off" in lines()[-1] and
          "Ctrl+S save" not in "\n".join(lines()[-2:]), "\n".join(lines()[-2:]))
    r.type(ENTER, 0.3, ESC, 0.3)
    check("and again: back in the message row", "Ctrl+Q quit" in lines()[-1], lines()[-1])
    quit_(r)
    b.write(".cfg/xted/config", "")
    r = b.run(["a.txt"], cols=60)
    check("a narrow terminal: short names (^S, M-x) to show more keys", "^S save" in lines()[-1] and
          "M-x commands" in lines()[-1], lines()[-1])
    quit_(r)
    b.close()


@scenario
def new_file():
    b = Box()
    b.write("a.txt", "uno\n")
    b.write("sub/b.txt", "due\n")
    r = b.run(["a.txt"], cols=110)
    last = lambda: r.screen().lines()[-1]
    text = lambda: r.screen().text()
    r.type(ctrl("n"), 0.3)
    check("Ctrl+N: a new empty buffer, no name asked", "New buffer" in last() and "[Untitled]" in text(),
          last())
    r.type("ciao", ctrl("s"), 0.3)
    check("Ctrl+S on it asks the name", "Save as:" in last(), last())
    r.type("sub/nuovo.txt", ENTER, 0.3)
    check("and writes it", b.read("sub/nuovo.txt") == "ciao\n" or b.read("sub/nuovo.txt") == "ciao",
          repr(b.read("sub/nuovo.txt")))
    # the file browser: a click selects the folder sub
    r.type(ctrl("b"), 0.3)
    rows = r.screen().lines()
    y = next(i for i, l in enumerate(rows) if " sub" in l[:25])
    x = rows[y].index("sub")
    r.type("\x1b[<0;%d;%dM" % (x + 2, y + 1), 0.05, "\x1b[<0;%d;%dm" % (x + 2, y + 1), 0.3)
    r.type(ctrl("n"), 0.3)
    check("Ctrl+N in the browser asks a name in the selected folder", "New file: sub/" in last(), last())
    r.type("x.txt", ENTER, 0.4)
    check("the file is made empty and opened", os.path.exists(b.path("sub/x.txt")) and
          os.path.getsize(b.path("sub/x.txt")) == 0 and "created" in last(), last())
    r.type(ctrl("b"), 0.2, "\x1b[1~", 0.1, "\x1b[18~", 0.3)          # Home, F7
    check("F7: a new folder", "New folder:" in last(), last())
    r.type("nuova", ENTER, 0.4)
    check("made and shown", os.path.isdir(b.path("nuova")) and "nuova" in text(), last())
    r.type("\x1b[12~", 0.3)                                         # F2 on it
    check("F2 renames: the name to change", "Rename to: nuova" in last(), last())
    r.type("2", ENTER, 0.4)
    check("renamed", os.path.isdir(b.path("nuova2")) and not os.path.exists(b.path("nuova")), last())
    r.type("\x1b[3~", 0.3)                                          # Del
    check("Del asks first", "Delete the folder" in last(), last())
    r.type("y", 0.4)
    check("an empty folder is deleted", not os.path.exists(b.path("nuova2")) and "deleted" in last(), last())
    b.write("esterno.txt", "x\n")
    r.type("\x1b[I", 0.4)                                           # the window comes back
    check("a file made by another program shows when the window comes back", "esterno.txt" in text(),
          text()[:500])
    r.type(ESC, ctrl("q"), 0.3, "n", 0.3)
    r.finish()
    b.close()


@scenario
def single_instance():
    b = Box()
    b.write("a.txt", "uno\n")
    b.write("b.txt", "due\nseconda riga\n")
    b.write("sub/c.txt", "tre\n")
    r = b.run(["a.txt"], cols=100)
    env = {"XDG_CONFIG_HOME": b.cfg, "XDG_STATE_HOME": b.state, "XTED_IPC": b.path(".ipc.sock")}
    out = subprocess.run([EXE, "--remote", "b.txt:2", "sub/c.txt"], cwd=b.dir, env=dict(os.environ, **env),
                         capture_output=True, timeout=10)
    check("xted --remote hands the files to the open xted and ends at once", out.returncode == 0, out)
    r.type("", 0.5)
    t = r.screen().text()
    check("they are opened there as buffers", "b.txt" in t and "c.txt" in t and
          "2 files opened from the desktop" in t, t[-300:])
    r.type(ctrl("e"), 0.3)
    check("all of them in the list of buffers", "a.txt" in r.screen().text() and
          "b.txt" in r.screen().text(), r.screen().text()[-400:])
    r.type(ESC, 0.2)
    # with a prompt open the files wait for it to be answered
    r.type(ctrl("o"), 0.2)
    subprocess.run([EXE, "--remote", "a.txt"], cwd=b.dir, env=dict(os.environ, **env), timeout=10)
    r.type("", 0.4)
    check("not while a prompt waits", "Open file:" in r.screen().lines()[-1], r.screen().lines()[-1])
    r.type(ESC, 0.4)
    check("then they open", "a.txt» opened from the desktop" in r.screen().lines()[-1], r.screen().lines()[-1])
    quit_(r)
    # none listening: --remote opens its own xted
    r = b.run(["--remote", "b.txt"], cols=100)
    check("with no xted open, --remote starts as usual", "due" in r.screen().text(), r.screen().text()[:200])
    quit_(r)
    # single_instance = off: it does not listen
    b.write(".cfg/xted/config", "single_instance = off\n")
    r = b.run(["a.txt"], cols=100)
    out = subprocess.run([EXE, "--remote", "b.txt"], cwd=b.dir, env=dict(os.environ, **env), capture_output=True,
                         timeout=10, input=b"")
    check("single_instance = off: no one takes them", "b.txt" not in r.screen().text(), r.screen().text()[:200])
    quit_(r)
    b.close()


@scenario
def utf16_file():
    b = Box()
    text = "[.ShellClassInfo]\r\nLocalizedResourceName=Città\r\n"
    with open(b.path("desktop.ini"), "wb") as f:
        f.write(b"\xff\xfe" + text.encode("utf-16-le"))
    r = b.run(["desktop.ini"], cols=100)
    t = r.screen().text()
    check("a UTF-16 file is shown as text", "LocalizedResourceName=Città" in t and "^@" not in t, t[:300])
    check("the status bar says its encoding", "UTF-16 LE" in t and "CRLF" in t, t[-300:])
    r.type(END, "!", ctrl("s"), 0.3)
    data = open(b.path("desktop.ini"), "rb").read()
    check("saved back in UTF-16 with its BOM", data == b"\xff\xfe" + "[.ShellClassInfo]!\r\nLocalizedResourceName=Città\r\n".encode("utf-16-le"),
          data[:60])
    quit_(r)
    b.close()


@scenario
def desktop_launcher():
    b = Box()
    b.write("a.txt", "uno\n")
    b.write("sub/b.txt", "due\nseconda\nterza\n")
    # fake terminals: they write how they were called; PATH has only them (never a real one)
    fake = b.path("fake")
    os.makedirs(fake)
    for name in ("konsole", "xdg-terminal-exec", "myterm"):
        with open(os.path.join(fake, name), "w") as f:
            f.write("#!/bin/sh\n{ echo \"$(basename \"$0\")\"; for a in \"$@\"; do echo \"$a\"; done; pwd; } > %s\n"
                    % b.path("called." + name))
        os.chmod(os.path.join(fake, name), 0o755)
    env = dict(os.environ, XDG_CONFIG_HOME=b.cfg, XDG_STATE_HOME=b.state, XTED_IPC=b.path(".ipc.sock"),
               PATH=fake, XDG_CURRENT_DESKTOP="KDE")
    env.pop("TERMINAL", None)
    run = lambda *args: subprocess.run([EXE, "--desktop"] + list(args), cwd=b.dir, env=env,
                                       stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
    called = lambda n: open(b.path("called." + n)).read().split("\n") if os.path.exists(b.path("called." + n)) else None

    # an xted open: the files go to it, no terminal is started
    r = b.run(["a.txt"], cols=100)
    out = run("sub/b.txt:2")
    r.type("", 0.5)
    check("--desktop hands the files to the open xted", out.returncode == 0 and "b.txt" in r.screen().text() and
          called("xdg-terminal-exec") is None, (out, r.screen().text()[-200:]))
    quit_(r)

    # none open: a terminal with xted in it, in the folder of the first file
    out = run("sub/b.txt:3")
    c = called("xdg-terminal-exec")
    check("none open: the terminal is started (xdg-terminal-exec first)", c is not None and c[1] == EXE and
          c[2] == b.path("sub/b.txt") + ":3:1", (out, c))
    check("in the folder of the file", c is not None and os.path.realpath(c[3]) == os.path.realpath(b.path("sub")), c)
    os.remove(os.path.join(fake, "xdg-terminal-exec"))
    out = run("a.txt")
    c = called("konsole")
    check("then the terminal of the desktop (KDE: konsole -e)", c is not None and c[1] == "-e" and c[2] == EXE, c)
    b.write(".cfg/xted/config", "terminal = myterm --flag\n")
    out = run()
    c = called("myterm")
    check("terminal = in the configuration wins, with its words", c is not None and c[1] == "--flag" and c[2] == EXE and
          len(c) == 5, c)
    b.write(".cfg/xted/config", "")
    for name in ("konsole", "myterm"):
        os.remove(os.path.join(fake, name))
    out = run("a.txt")
    check("no terminal at all: said, and an error", out.returncode == 1 and b"terminal" in out.stderr, out)
    b.close()


@scenario
def desktop_clipboard():
    b = Box()
    b.write("a.txt", "uno\ndue\n")
    # fake wl-copy and wl-paste: the clipboard is a file of the box
    fake, clip = b.path("fake"), b.path("clip")
    os.makedirs(fake)
    for name, body in (("wl-copy", "/bin/cat > %s" % clip), ("wl-paste", "[ -f %s ] && exec /bin/cat %s; exit 1" % (clip, clip))):
        with open(os.path.join(fake, name), "w") as f:
            f.write("#!/bin/sh\n" + body + "\n")
        os.chmod(os.path.join(fake, name), 0o755)
    r = b.run(["a.txt"], cols=100, env={"WAYLAND_DISPLAY": "wayland-test", "PATH": fake, "SSH_CONNECTION": "",
                                         "SSH_TTY": ""})
    r.type(ctrl("c"), 0.5)
    check("Ctrl+C: the line goes to the desktop's clipboard (wl-copy)", os.path.exists(clip) and
          open(clip).read() == "uno\n", open(clip).read() if os.path.exists(clip) else None)
    with open(clip, "w") as f:
        f.write("dal desktop\r\n")
    r.type(DOWN, END, ctrl("v"), ctrl("s"), 0.5)
    check("Ctrl+V: what another program copied (CR LF as LF)", b.read("a.txt") == "uno\ndue" + "dal desktop\n" + "\n",
          repr(b.read("a.txt")))
    quit_(r)
    # a graphical session without the programs: OSC 52, and a hint once
    os.remove(os.path.join(fake, "wl-copy"))
    r = b.run(["a.txt"], cols=140, env={"WAYLAND_DISPLAY": "wayland-test", "PATH": fake})
    r.type(ctrl("c"), 0.5)
    check("no wl-copy: the copy says what to install", "wl-clipboard" in r.screen().lines()[-1] and
          b"\x1b]52;c;" in r.out, r.screen().lines()[-1])
    r.type(ctrl("c"), 0.5)
    check("only once", "wl-clipboard" not in r.screen().lines()[-1], r.screen().lines()[-1])
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
          "xted 0.12.0: help" in t and "Copyright (C) 2026 David Bigagli, MIT license" in t and "Ctrl+S" in t and "Save" in t and "[Help]" in t, t[:600])
    r.type("x")
    check("the help page cannot be changed", "read-only" in r.screen().text())
    r.type(ctrl("w"))
    t = r.screen().text()
    check("Ctrl+W closes it and the file is back", "[Help]" not in t and "text" in t)
    quit_(r)
    r = b.run(["a.txt"], rows=40, env={"LANG": "it_IT.UTF-8"})
    r.type("\x1bOP")
    t = r.screen().text()
    check("the help page follows the language", "xted 0.12.0: guida" in t and "Salva" in t, t[:400])
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
