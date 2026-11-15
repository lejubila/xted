#!/usr/bin/env python3
"""Compares the texts passed to tr() and N_() in src/*.c with the catalog lang/<code> (default: it).

Prints the texts that have no translation and the entries the code no longer uses. Exits with 1
if there is any difference. Usage: tools/i18n_check.py [code]"""
import glob
import re
import sys

code = sys.argv[1] if len(sys.argv) > 1 else "it"

def c_unescape(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            c = s[i + 1]
            if c == "x":
                j = i + 2
                bs = bytearray()
                while True:
                    bs.append(int(s[j:j + 2], 16))
                    if s[j + 2:j + 4] != "\\x":
                        j += 2
                        break
                    j += 4
                out.append(bs.decode("utf-8"))
                i = j
                continue
            out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}.get(c, c))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)

# tr( "a" "b" ): adjacent literals are one text
call = re.compile(r'\b(?:tr|N_)\(\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)\)')
lit = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
used = {}
for f in sorted(glob.glob("src/*.c")):
    for m in call.finditer(open(f, encoding="utf-8").read()):
        text = c_unescape("".join(lit.findall(m.group(1))))
        used.setdefault(text, f)

def unescape_cat(s):
    return re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), s)

have = {}
for line in open("lang/" + code, encoding="utf-8").read().split("\n"):
    if not line or line[0] in "#@" or "\t" not in line:
        continue
    k, _, v = line.partition("\t")
    have[unescape_cat(k)] = unescape_cat(v)

missing = [t for t in used if t not in have]
unused = [t for t in have if t not in used]
for t in missing:
    print("missing in lang/%s (%s): %r" % (code, used[t], t))
for t in unused:
    print("not used by the code: %r" % t)
print("%d texts in the code, %d in the catalog" % (len(used), len(have)))
sys.exit(1 if missing or unused else 0)
