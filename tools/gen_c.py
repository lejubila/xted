#!/usr/bin/env python3
"""Writes tools/lib/c.txt (C standard and POSIX library symbols) from the system headers.

Needs universal-ctags. Usage: tools/gen_c.py > tools/lib/c.txt
"""
import json
import os
import re
import subprocess
import sys

HEADERS = """stdio stdlib string strings ctype math time errno signal setjmp stdint limits float assert
locale wchar wctype inttypes stdbool stddef unistd fcntl dirent pthread semaphore sys/types
sys/stat sys/socket netinet/in arpa/inet netdb sys/wait sys/time sys/mman poll sys/select
termios regex dlfcn glob fnmatch libgen utime sched syslog sys/ioctl sys/resource sys/utsname
pwd grp getopt sys/epoll sys/uio sys/un net/if ifaddrs fenv complex""".split()

WORDS = {"const", "struct", "union", "enum", "extern", "static", "inline", "volatile", "register"}


def tags(path):
    r = subprocess.run(["ctags", "-f", "-", "--output-format=json", "--language-force=C", "--c-kinds=+p",
                        "--fields=+Snt",
                        "-I", "__THROW,__THROWNL,__nonnull+,__wur,__attribute_malloc__,__attribute_pure__,"
                        "__attribute_const__,__attr_dealloc+,__attribute_warn_unused_result__,"
                        "__attribute_deprecated__,__REDIRECT+,__REDIRECT_NTH+,__BEGIN_DECLS,__END_DECLS,"
                        "__fortified_attr_access+,__attr_access+,__attribute_nonnull__+,__glibc_unlikely+,"
                        "__COLD,__attribute_returns_twice__,__attribute_alloc_size__+,__attribute_format_arg__+,"
                        "__attribute_format_strfmon__+,__attribute_noinline__,__attribute_used__,__attribute_maybe_unused__,"
                        "__nonnull,__restrict,__restrict_arr,__inline,__extension__,__attribute__+,"
                        "__attr_dealloc_free,__attribute_artificial__,__ptr_t,__THROW_",
                        path], capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        try:
            t = json.loads(line)
        except ValueError:
            continue
        if t.get("_type") != "tag":
            continue
        kind = {"macro": "d", "function": "f", "prototype": "p", "typedef": "t", "struct": "s",
                "union": "u", "enumerator": "e", "member": "m"}.get(t.get("kind"), "")
        extra = {"typeref": t.get("typeref", "")}
        if "typename:" not in extra["typeref"] and extra["typeref"]:
            extra["typeref"] = extra["typeref"]
        if "signature" in t:
            extra["signature"] = t["signature"]
        if t.get("scope"):
            extra["scope"] = t["scope"]
        out.append((t["name"], kind, extra))
    return out


def clean_type(t):
    t = re.sub(r"^typename:", "", t)
    t = re.sub(r"\b(__restrict|restrict|extern|static|inline|const|struct|union|enum|volatile)\b", " ", t)
    t = re.sub(r"\s+", " ", t).strip()
    t = t.replace(" *", " *").replace("* ", "*")
    return t


def clean_params(sig):
    sig = sig.strip()
    if sig.startswith("("):
        sig = sig[1:]
    if sig.endswith(")"):
        sig = sig[:-1]
    sig = re.sub(r"\b__restrict_arr\b|\b__restrict\b|\brestrict\b", "", sig)
    sig = re.sub(r"\b__(\w+)", r"\1", sig)               # glibc names its parameters __x
    sig = re.sub(r"\s+", " ", sig).replace(" ,", ",").replace(",", ", ").replace("  ", " ")
    sig = re.sub(r"\*\s+", "*", sig).strip()
    sig = sig.replace("* ,", "*,")
    if sig == "void":
        return ""
    return sig[:90]


def main():
    funcs, macros, types, members, anon = {}, {}, set(), {}, {}
    typedef_of = {}
    paths = []
    for h in HEADERS:
        for base in ("/usr/include/", "/usr/include/x86_64-linux-gnu/"):
            if os.path.exists(base + h + ".h"):
                paths.append(base + h + ".h")
    for x in ("types/struct_tm", "types/struct_timeval", "types/struct_timespec", "struct_stat",
              "dirent", "termios-struct", "types/struct_itimerspec", "socket", "types/struct_iovec",
              "types/struct_rusage", "sockaddr"):
        p = "/usr/include/x86_64-linux-gnu/bits/%s.h" % x
        if os.path.exists(p):
            paths.append(p)
    for path in paths:
        for name, kind, ex in tags(path):
            if name.startswith("_") or "." in name or name.endswith("64"):
                continue
            if kind == "p" or kind == "f":
                if name not in funcs:
                    funcs[name] = (clean_params(ex.get("signature", "()")), clean_type(ex.get("typeref", "")))
            elif kind == "d":
                if re.fullmatch(r"[A-Z][A-Z0-9_]+", name):
                    macros.setdefault(name, "")
                elif re.fullmatch(r"[a-z]\w*", name) and "signature" in ex:
                    funcs.setdefault(name, (clean_params(ex["signature"]), ""))
            elif kind in ("s", "u"):
                types.add(name)
            elif kind == "t":
                types.add(name)
                tr = ex.get("typeref", "")
                m = re.match(r"(?:struct|union):(\w+)", tr)
                if m:
                    typedef_of[m.group(1)] = name
            elif kind == "e":
                macros.setdefault(name, "")
            elif kind == "m":
                sc = ex.get("scope", "")
                if sc:
                    members.setdefault(sc, []).append((name, clean_type(ex.get("typeref", ""))))
    out = ["#"]
    for n in sorted(funcs):
        p, r = funcs[n]
        out.append("%s(%s)>%s" % (n, p, r))
    for n in sorted(macros):
        out.append("%s:" % n)
    emitted = set()
    for sc in sorted(members):
        name = typedef_of.get(sc, sc)
        if name.startswith("__anon") or name.startswith("_"):
            continue
        if name in emitted:
            continue
        emitted.add(name)
        out.append("#" + name)
        for m, t in members[sc]:
            if not m.startswith("_"):
                out.append("%s:%s" % (m, t))
    for t in sorted(types):
        if t not in emitted and not t.startswith("_"):
            out.append("#" + t)
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
