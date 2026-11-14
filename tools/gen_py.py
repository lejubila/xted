#!/usr/bin/env python3
"""Writes tools/lib/py.txt (Python standard library symbols) by introspection of this interpreter.

Return types come from annotations when there are any and from the RET table below otherwise.
Usage: tools/gen_py.py > tools/lib/py.txt
"""
import builtins
import importlib
import inspect
import re
import sys

MODULES = ["os", "sys", "re", "json", "math", "random", "time", "datetime", "collections",
           "itertools", "functools", "pathlib", "subprocess", "argparse", "logging", "io",
           "shutil", "string", "glob", "copy", "csv", "tempfile", "threading", "socket",
           "struct", "hashlib", "base64", "urllib.request", "urllib.parse", "sqlite3", "enum",
           "dataclasses", "typing", "contextlib", "operator", "statistics", "textwrap",
           "platform", "signal", "traceback", "uuid", "heapq", "bisect", "queue", "asyncio",
           "unittest", "pickle", "zipfile", "decimal", "fractions", "secrets", "shlex", "pprint"]

# classes (module-qualified or builtin) whose members are listed
CLASSES = ["str", "list", "dict", "set", "frozenset", "tuple", "int", "float", "bytes",
           "bytearray", "bool", "complex", "object", "range", "slice", "memoryview",
           "io.TextIOWrapper", "io.BufferedReader", "io.BytesIO", "io.StringIO",
           "pathlib.Path", "datetime.datetime", "datetime.date", "datetime.time",
           "datetime.timedelta", "re.Pattern", "re.Match", "collections.deque",
           "collections.Counter", "collections.OrderedDict", "collections.defaultdict",
           "subprocess.Popen", "subprocess.CompletedProcess", "argparse.ArgumentParser",
           "argparse.Namespace", "logging.Logger", "threading.Thread", "threading.Lock",
           "threading.Event", "socket.socket", "queue.Queue", "sqlite3.Connection",
           "sqlite3.Cursor", "csv.DictReader", "csv.DictWriter", "random.Random",
           "decimal.Decimal", "fractions.Fraction", "zipfile.ZipFile", "uuid.UUID",
           "BaseException", "Exception", "OSError", "ValueError", "TypeError", "KeyError",
           "IndexError", "RuntimeError", "StopIteration", "FileNotFoundError",
           "ImportError", "AttributeError", "NameError", "NotImplementedError",
           "AssertionError", "LookupError", "ArithmeticError", "ZeroDivisionError",
           "PermissionError", "TimeoutError", "UnicodeError", "KeyboardInterrupt"]

STR = "str"
RET = {
    "str": {k: STR for k in "capitalize casefold center expandtabs format format_map join ljust lower lstrip replace rjust rstrip strip swapcase title translate upper zfill removeprefix removesuffix".split()},
    "list": {"copy": "list"},
    "dict": {"copy": "dict"},
    "set": {k: "set" for k in "copy union intersection difference symmetric_difference".split()},
    "bytes": {"decode": "str"},
    "datetime": {"now": "datetime", "today": "datetime", "utcnow": "datetime", "fromtimestamp": "datetime",
                 "strptime": "datetime", "fromisoformat": "datetime", "replace": "datetime",
                 "strftime": "str", "isoformat": "str", "date": "date", "time": "time",
                 "timestamp": "float", "weekday": "int", "astimezone": "datetime"},
    "date": {"today": "date", "strftime": "str", "isoformat": "str", "replace": "date", "fromisoformat": "date"},
    "timedelta": {"total_seconds": "float"},
    "Path": {"resolve": "Path", "joinpath": "Path", "with_suffix": "Path", "with_name": "Path",
             "absolute": "Path", "expanduser": "Path", "home": "Path", "cwd": "Path",
             "read_text": "str", "read_bytes": "bytes", "exists": "bool", "is_file": "bool",
             "is_dir": "bool", "stat": "stat_result", "open": "TextIOWrapper", "relative_to": "Path"},
    "Pattern": {"match": "Match", "search": "Match", "fullmatch": "Match", "sub": "str",
                "findall": "list", "split": "list"},
    "Match": {"group": "str", "start": "int", "end": "int", "span": "tuple", "groups": "tuple"},
    "TextIOWrapper": {"read": "str", "readline": "str", "readlines": "list"},
    "Popen": {"communicate": "tuple", "poll": "int", "wait": "int"},
    "ArgumentParser": {"parse_args": "Namespace", "add_argument": "Action"},
    "Logger": {},
    "socket": {"accept": "tuple", "makefile": "TextIOWrapper"},
    "Counter": {"most_common": "list", "copy": "Counter"},
    "Connection": {"cursor": "Cursor", "execute": "Cursor"},
    "Cursor": {"fetchall": "list", "fetchone": "tuple", "execute": "Cursor"},
    "Decimal": {},
    "UUID": {},
}
# module-level functions: (module, name) -> return type
MODRET = {
    ("os", "getcwd"): "str", ("os", "listdir"): "list", ("os", "getenv"): "str",
    ("os", "getpid"): "int", ("os", "stat"): "stat_result", ("os", "walk"): "tuple",
    ("os", "popen"): "TextIOWrapper", ("os", "urandom"): "bytes",
    ("re", "compile"): "Pattern", ("re", "match"): "Match", ("re", "search"): "Match",
    ("re", "fullmatch"): "Match", ("re", "sub"): "str", ("re", "findall"): "list",
    ("re", "split"): "list", ("re", "escape"): "str",
    ("json", "dumps"): "str", ("json", "load"): "dict", ("json", "loads"): "dict",
    ("math", "sqrt"): "float", ("math", "floor"): "int", ("math", "ceil"): "int",
    ("random", "random"): "float", ("random", "randint"): "int", ("random", "uniform"): "float",
    ("random", "choice"): "object", ("time", "time"): "float", ("time", "sleep"): "None",
    ("time", "strftime"): "str", ("time", "monotonic"): "float", ("time", "perf_counter"): "float",
    ("subprocess", "run"): "CompletedProcess", ("subprocess", "check_output"): "bytes",
    ("subprocess", "Popen"): "Popen", ("logging", "getLogger"): "Logger",
    ("argparse", "ArgumentParser"): "ArgumentParser", ("pathlib", "Path"): "Path",
    ("shutil", "which"): "str", ("glob", "glob"): "list", ("hashlib", "sha256"): "object",
    ("io", "open"): "TextIOWrapper", ("socket", "socket"): "socket",
    ("sqlite3", "connect"): "Connection", ("uuid", "uuid4"): "UUID",
    ("datetime", "datetime"): "datetime", ("collections", "Counter"): "Counter",
    ("collections", "deque"): "deque", ("collections", "defaultdict"): "defaultdict",
    ("collections", "OrderedDict"): "OrderedDict", ("threading", "Thread"): "Thread",
    ("threading", "Lock"): "Lock", ("threading", "Event"): "Event",
    ("tempfile", "mkdtemp"): "str", ("platform", "system"): "str", ("shlex", "split"): "list",
    ("textwrap", "dedent"): "str", ("base64", "b64encode"): "bytes",
    ("queue", "Queue"): "Queue", ("csv", "DictReader"): "DictReader",
    ("csv", "DictWriter"): "DictWriter", ("zipfile", "ZipFile"): "ZipFile",
}
BUILTIN_RET = {"open": "TextIOWrapper", "len": "int", "str": "str", "int": "int",
               "float": "float", "list": "list", "dict": "dict", "set": "set",
               "tuple": "tuple", "sorted": "list", "input": "str", "repr": "str",
               "range": "range", "bool": "bool", "abs": "int", "sum": "int",
               "isinstance": "bool", "hasattr": "bool", "callable": "bool", "round": "int",
               "ord": "int", "chr": "str", "format": "str", "hex": "str", "bin": "str",
               "oct": "str", "id": "int", "hash": "int", "bytes": "bytes",
               "bytearray": "bytearray", "frozenset": "frozenset", "enumerate": "enumerate",
               "zip": "zip", "map": "map", "filter": "filter", "reversed": "reversed"}


def clean_sig(obj):
    try:
        sig = str(inspect.signature(obj))
    except (TypeError, ValueError):
        txt = getattr(obj, "__text_signature__", None)
        if not txt:
            return ""
        sig = txt
    sig = sig[1:-1] if sig.startswith("(") and sig.endswith(")") else sig
    sig = re.sub(r"\$\w+,?\s*|\bself,?\s*", "", sig)
    sig = re.sub(r",?\s*/(?=,|$)", "", sig)
    sig = re.sub(r"\s*->.*$", "", sig)
    sig = re.sub(r":\s*[A-Za-z_][\w\.\[\], |]*(?=[,=)]|$)", "", sig)      # drop annotations
    sig = re.sub(r"\s+", " ", sig).strip(", ")
    if len(sig) > 70:
        sig = sig[:67].rsplit(",", 1)[0] + ", ..."
    return sig.replace("(", "(").replace("<", "").replace(">", "")


def ret_of(obj, hint):
    if hint:
        return hint
    try:
        r = inspect.signature(obj).return_annotation
    except (TypeError, ValueError):
        return ""
    if r is inspect.Signature.empty:
        return ""
    name = getattr(r, "__name__", "")
    return name if re.fullmatch(r"[A-Za-z_]\w*", name or "") and name not in ("NoneType",) else ""


def resolve(spec):
    if "." in spec:
        mod, _, cls = spec.rpartition(".")
        return getattr(importlib.import_module(mod), cls), cls
    return getattr(builtins, spec), spec


def emit_class(out, obj, name, base_hint):
    base = ""
    for b in obj.__mro__[1:2]:
        if b is not object and b.__name__ != name:
            base = b.__name__
    out.append("#%s%s" % (name, ":" + base if base else ""))
    hints = RET.get(name, {})
    seen = set()
    for attr in sorted(vars(obj)):
        if attr.startswith("_") and attr not in ("__init__",):
            continue
        if attr == "__init__":
            continue
        val = vars(obj)[attr]
        if attr in seen:
            continue
        seen.add(attr)
        if isinstance(val, (staticmethod, classmethod)):
            val = val.__func__
        if isinstance(val, property) or inspect.isgetsetdescriptor(val) or inspect.ismemberdescriptor(val):
            out.append("%s:%s" % (attr, hints.get(attr, "")))
        elif callable(val):
            out.append("%s(%s)>%s" % (attr, clean_sig(val), ret_of(val, hints.get(attr, ""))))
        else:
            out.append("%s:%s" % (attr, type(val).__name__ if not attr.isupper() else ""))
    # the constructor's signature is that of the class itself: not listed as a member


def main():
    out = []
    out.append("#")
    for name in sorted(vars(builtins)):
        if name.startswith("_"):
            continue
        val = getattr(builtins, name)
        if inspect.isclass(val):
            continue          # classes are sections; their constructor call is the name
        if callable(val):
            out.append("%s(%s)>%s" % (name, clean_sig(val), BUILTIN_RET.get(name, "")))
        else:
            out.append("%s:" % name)
    for spec in ("print",):
        pass
    for name in ("int", "str", "float", "list", "dict", "set", "tuple", "bool", "bytes", "bytearray",
                 "frozenset", "range", "enumerate", "zip", "map", "filter", "reversed", "object",
                 "complex", "slice", "memoryview", "property", "staticmethod", "classmethod", "super", "type"):
        out.append("%s(%s)>%s" % (name, "", name))
    for mod in MODULES:
        out.append("%s:" % mod.split(".")[-1])          # a module is a class to complete after "import"
    secs = []
    for spec in CLASSES:
        try:
            obj, name = resolve(spec)
        except (ImportError, AttributeError):
            continue
        sec = []
        emit_class(sec, obj, name, None)
        secs.append(sec)
    for mod in MODULES:
        try:
            m = importlib.import_module(mod)
        except ImportError:
            continue
        mname = mod.split(".")[-1]
        sec = ["#" + mname]
        for attr in sorted(vars(m)):
            if attr.startswith("_"):
                continue
            val = getattr(m, attr)
            if inspect.ismodule(val):
                if attr == "path" and mod == "os":
                    sec.append("path:path")
                continue
            if inspect.isclass(val):
                sec.append("%s(%s)>%s" % (attr, "", MODRET.get((mname, attr), attr)))
            elif callable(val):
                sec.append("%s(%s)>%s" % (attr, clean_sig(val), ret_of(val, MODRET.get((mname, attr), ""))))
            elif attr in ("sep", "linesep", "environ", "argv", "path", "stdin", "stdout", "stderr",
                          "version", "platform", "maxsize", "name", "curdir", "pardir", "pi", "e",
                          "inf", "nan", "tau", "ascii_letters", "ascii_lowercase",
                          "ascii_uppercase", "digits", "punctuation", "whitespace", "hexdigits",
                          "printable", "PIPE", "STDOUT", "DEVNULL", "DEBUG", "INFO", "WARNING",
                          "ERROR", "CRITICAL", "IGNORECASE", "MULTILINE", "DOTALL", "VERBOSE"):
                sec.append("%s:%s" % (attr, "" if attr.isupper() else type(val).__name__))
        secs.append(sec)
    # os.path as its own section
    import posixpath
    sec = ["#path"]
    for attr in sorted(vars(posixpath)):
        if attr.startswith("_") or not callable(getattr(posixpath, attr)):
            continue
        h = {"join": "str", "dirname": "str", "basename": "str", "abspath": "str", "exists": "bool",
             "isfile": "bool", "isdir": "bool", "splitext": "tuple", "expanduser": "str",
             "realpath": "str", "getsize": "int"}.get(attr, "")
        sec.append("%s(%s)>%s" % (attr, clean_sig(getattr(posixpath, attr)), h))
    secs.append(sec)
    # exceptions as plain classes need only a header; the generic ones above carry a few members
    for sec in secs:
        out.extend(sec)
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
