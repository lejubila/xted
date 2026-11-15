#!/usr/bin/env python3
"""Adds the import lines to the Python list: reads tools/lib/py.txt (or what gen_py.py wrote) on
standard input and writes it again with

  - "@module" before each module entry of the global section ("os:" ...): accepting such a name in
    the editor adds "import os";
  - "@module" as the line after the start of a class that a module defines ("#Path"): accepting
    it adds "from pathlib import Path".

Idempotent. Usage: tools/gen_py.py | tools/py_modules.py > tools/lib/py.txt
       tools/py_modules.py < tools/lib/py.txt > /tmp/py.txt
"""
import re
import sys

# the modules that are plain names (urllib.request and urllib.parse are not: they are dotted)
MODULES = """os sys re json math random time datetime collections itertools functools pathlib subprocess
argparse logging io shutil string glob copy csv tempfile threading socket struct hashlib base64 sqlite3
enum dataclasses typing contextlib operator statistics textwrap platform signal traceback uuid heapq
bisect queue asyncio unittest pickle zipfile decimal fractions secrets shlex pprint""".split()

# class -> the module that defines it (the builtin ones need no import)
CLASS_MODULE = {
    "TextIOWrapper": "io", "BufferedReader": "io", "BytesIO": "io", "StringIO": "io",
    "Path": "pathlib", "datetime": "datetime", "date": "datetime", "time": "datetime",
    "timedelta": "datetime", "Pattern": "re", "Match": "re", "deque": "collections",
    "Counter": "collections", "OrderedDict": "collections", "defaultdict": "collections",
    "Popen": "subprocess", "CompletedProcess": "subprocess", "ArgumentParser": "argparse",
    "Namespace": "argparse", "Logger": "logging", "Thread": "threading", "Lock": "threading",
    "Event": "threading", "socket": "socket", "Queue": "queue", "Connection": "sqlite3",
    "Cursor": "sqlite3", "DictReader": "csv", "DictWriter": "csv", "Random": "random",
    "Decimal": "decimal", "Fraction": "fractions", "ZipFile": "zipfile", "UUID": "uuid",
}


def main():
    lines = sys.stdin.read().split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    out = []
    in_global = True        # the global section comes first
    classes = True          # then the classes, then (from "#os") the sections of the modules
    for l in lines:
        if l.startswith("@"):
            continue                          # rewritten below
        if l == "#":
            out.append(l)                     # the start of the global section
            continue
        if l.startswith("#"):
            in_global = False
            name = l[1:].split(":")[0]
            if name == "os":
                classes = False
            out.append(l)
            if classes and name in CLASS_MODULE:
                out.append("@" + CLASS_MODULE[name])
            continue
        m = re.match(r"(\w+):$", l)
        if in_global and m and m.group(1) in MODULES:
            out.append("@" + m.group(1))
        out.append(l)
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
