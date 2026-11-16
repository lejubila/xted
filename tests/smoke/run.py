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
