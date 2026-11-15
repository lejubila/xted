#!/usr/bin/env python3
"""Writes tools/lib/c.txt (C standard and POSIX library symbols) from the system headers.

Each symbol belongs to the header that declares it: the global section is written as groups, a line
"@stdio.h" starting the symbols of that header, and a type section starts with such a line too
(the editor adds `#include <stdio.h>` when a symbol is accepted and its header is not there).

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

# Declared by several headers: the one a program should include for it.
CANON = {"size_t": "stddef.h", "NULL": "stddef.h", "wchar_t": "stddef.h", "ptrdiff_t": "stddef.h",
         "ssize_t": "sys/types.h", "off_t": "sys/types.h", "pid_t": "sys/types.h", "uid_t": "sys/types.h",
         "gid_t": "sys/types.h", "mode_t": "sys/types.h", "time_t": "time.h", "clock_t": "time.h",
         "FILE": "stdio.h", "va_list": "stdarg.h", "sigset_t": "signal.h", "fd_set": "sys/select.h",
         "socklen_t": "sys/socket.h", "struct timespec": "time.h", "locale_t": "locale.h",
         "intptr_t": "stdint.h", "uintptr_t": "stdint.h", "wint_t": "wchar.h", "pthread_t": "pthread.h",
         "dev_t": "sys/types.h", "ino_t": "sys/types.h", "nlink_t": "sys/types.h", "blksize_t": "sys/types.h",
         "blkcnt_t": "sys/types.h", "id_t": "sys/types.h", "useconds_t": "sys/types.h",
         "suseconds_t": "sys/types.h", "timer_t": "time.h", "clockid_t": "time.h"}
# The files of bits/ and their public header.
BITS = {"types/struct_tm": "time.h", "types/struct_timeval": "sys/time.h", "types/struct_timespec": "time.h",
        "struct_stat": "sys/stat.h", "dirent": "dirent.h", "termios-struct": "termios.h",
        "types/struct_itimerspec": "time.h", "socket": "sys/socket.h", "types/struct_iovec": "sys/uio.h",
        "types/struct_rusage": "sys/resource.h", "sockaddr": "sys/socket.h"}

# Symbols that the system headers declare with macros ctags cannot read (math and ctype functions,
# FILE, stdin, fixed-size integers, errno, O_* flags, signals): kept by hand, per header.
CURATED = {
    "math.h": """sin(double x)>double cos(double x)>double tan(double x)>double asin(double x)>double
        acos(double x)>double atan(double x)>double atan2(double y, double x)>double sinh(double x)>double
        cosh(double x)>double tanh(double x)>double exp(double x)>double log(double x)>double
        log10(double x)>double log2(double x)>double pow(double x, double y)>double sqrt(double x)>double
        cbrt(double x)>double fabs(double x)>double floor(double x)>double ceil(double x)>double
        round(double x)>double trunc(double x)>double fmod(double x, double y)>double
        hypot(double x, double y)>double fmin(double x, double y)>double fmax(double x, double y)>double
        lround(double x)>long INFINITY: NAN: HUGE_VAL:""",
    "ctype.h": """isalnum(int c)>int isalpha(int c)>int iscntrl(int c)>int isdigit(int c)>int isgraph(int c)>int
        islower(int c)>int isprint(int c)>int ispunct(int c)>int isspace(int c)>int isupper(int c)>int
        isxdigit(int c)>int""",
    "stdio.h": """#FILE stdin:FILE * stdout:FILE * stderr:FILE * BUFSIZ: getchar()>int putchar(int c)>int
        fgets(char *s, int size, FILE *stream)>char * fgetc(FILE *stream)>int fputc(int c, FILE *stream)>int
        fread(void *ptr, size_t size, size_t nmemb, FILE *stream)>size_t
        fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)>size_t fclose(FILE *stream)>int
        fflush(FILE *stream)>int fseek(FILE *stream, long offset, int whence)>int ftell(FILE *stream)>long
        rewind(FILE *stream)>void""",
    "stdint.h": """#int8_t #int16_t #int32_t #int64_t #uint8_t #uint16_t #uint32_t #uint64_t #intptr_t #uintptr_t
        #intmax_t #uintmax_t INT32_MAX: INT64_MAX: UINT8_MAX: UINT16_MAX: UINT32_MAX: UINT64_MAX:""",
    "stdbool.h": "#bool true: false:",
    "stddef.h": "#size_t #ptrdiff_t #wchar_t NULL: offsetof(type, member)>size_t",
    "errno.h": """errno:int EPERM: ENOENT: EINTR: EIO: EBADF: EAGAIN: ENOMEM: EACCES: EEXIST: ENOTDIR: EISDIR:
        EINVAL: ENOSPC: EPIPE: ERANGE: ETIMEDOUT:""",
    "signal.h": """SIGHUP: SIGINT: SIGQUIT: SIGABRT: SIGKILL: SIGSEGV: SIGPIPE: SIGALRM: SIGTERM: SIGCHLD: SIGUSR1:
        SIGUSR2: SIG_DFL: SIG_IGN:""",
    "fcntl.h": """O_RDONLY: O_WRONLY: O_RDWR: O_CREAT: O_EXCL: O_TRUNC: O_APPEND: O_NONBLOCK: O_CLOEXEC:
        F_GETFL: F_SETFL: F_GETFD: F_SETFD: FD_CLOEXEC:""",
    "time.h": "CLOCKS_PER_SEC: CLOCK_REALTIME: CLOCK_MONOTONIC:",
    "stdarg.h": """va_start(ap, last)>void va_arg(ap, type)>type va_end(ap)>void va_copy(dest, src)>void""",
    "stdlib.h": "RAND_MAX:",
}

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
    hdrs = {}                       # name -> headers that declare it, in order
    paths = []
    for h in HEADERS:
        for base in ("/usr/include/", "/usr/include/x86_64-linux-gnu/"):
            if os.path.exists(base + h + ".h"):
                paths.append((base + h + ".h", h + ".h"))
    for x in ("types/struct_tm", "types/struct_timeval", "types/struct_timespec", "struct_stat",
              "dirent", "termios-struct", "types/struct_itimerspec", "socket", "types/struct_iovec",
              "types/struct_rusage", "sockaddr"):
        p = "/usr/include/x86_64-linux-gnu/bits/%s.h" % x
        if os.path.exists(p):
            paths.append((p, BITS[x]))
    for path, header in paths:
        for name, kind, ex in tags(path):
            if name.startswith("_") or "." in name or name.endswith("64"):
                continue
            hl = hdrs.setdefault(name, [])
            if header not in hl:
                hl.append(header)
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
    variables = {}
    for h, text in CURATED.items():
        # entries are separated by blanks, but a function's parameters and a variable's type hold some
        for tok in re.findall(r"#\w+|\w+\([^)]*\)>[A-Za-z_]\w*(?: \*)?|\w+:[A-Za-z_]*(?: \*)?|\w+\([^)]*\)", text):
            if tok.startswith("#"):
                name = tok[1:]
                types.add(name)
            elif "(" in tok:
                name, rest = tok.split("(", 1)
                params, _, ret = rest.partition(")")
                ret = ret.lstrip(">")
                funcs.setdefault(name, (clean_params("(" + params + ")"), ret.strip()))
            else:
                name, _, ty = tok.partition(":")
                if re.fullmatch(r"[A-Z][A-Z0-9_]+|true|false", name):
                    macros.setdefault(name, "")
                else:
                    variables[name] = ty.strip()
            hdrs[name] = [h]

    def header(n, sc=None):
        h = hdrs.get(n) or (hdrs.get(sc) if sc else None)
        if n in CANON:
            return CANON[n]
        return h[0] if h else ""

    out = []
    groups = {}
    for n in funcs:
        groups.setdefault(header(n), ([], []))[0].append(n)
    for n in macros:
        groups.setdefault(header(n), ([], []))[1].append(n)
    for n in variables:
        groups.setdefault(header(n), ([], []))[1].append(n)
    for h in sorted(groups):
        out.append("@" + h if h else "#")
        fs, ms = groups[h]
        for n in sorted(fs):
            p, r = funcs[n]
            out.append("%s(%s)>%s" % (n, p, r))
        for n in sorted(ms):
            out.append("%s:%s" % (n, variables.get(n, "")))
    # the global section starts with "#": the group of symbols without a header comes first
    if not out[0].startswith("#"):
        out.insert(0, "#")
    emitted = set()
    for sc in sorted(members):
        name = typedef_of.get(sc, sc)
        if name.startswith("__anon") or name.startswith("_"):
            continue
        if name in emitted:
            continue
        emitted.add(name)
        out.append("#" + name)
        h = header(name, sc)
        if h:
            out.append("@" + h)
        for m, t in members[sc]:
            if not m.startswith("_"):
                out.append("%s:%s" % (m, t))
    for t in sorted(types):
        if t not in emitted and not t.startswith("_"):
            out.append("#" + t)
            if header(t):
                out.append("@" + header(t))
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
