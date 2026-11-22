#!/bin/sh
# The files of a release in build/release/, from a clean build of both systems:
#   xted-<v>-linux-x86_64            the static binary (musl)
#   xted-<v>-linux-x86_64.tar.gz     with xt, the catalogs and the manual pages (bin/, share/)
#   xted-<v>-windows-x86_64.zip      xted.exe, the catalogs, README and LICENSE
#   install.sh, install.ps1          the installers
#   SHA256SUMS
# The version is src/version.h's. Each binary must stay under the limit of SPEC.md.
#   tools/release.sh            both systems
#   tools/release.sh --no-win   Linux only (no MinGW at hand)
set -eu

LIMIT=614400                        # 600 KB: SPEC.md

cd "$(dirname "$0")/.."
V=$(sed -n 's/^#define XTED_VERSION "\(.*\)"$/\1/p' src/version.h)
[ -n "$V" ] || { echo "release: no XTED_VERSION in src/version.h" >&2; exit 1; }
win=1
[ "${1:-}" = "--no-win" ] && win=0
R=build/release

size_ok() {
    s=$(wc -c < "$1")
    echo "release: $1: $s bytes (limit $LIMIT)"
    [ "$s" -le "$LIMIT" ] || { echo "release: $1 is over the limit" >&2; exit 1; }
}

rm -rf "$R" build/static build/win
mkdir -p "$R"

# ---- Linux ----------------------------------------------------------------
make >/dev/null
file build/static/xted | grep -q "statically linked" ||
    { echo "release: build/static/xted is not static (is musl-gcc installed?)" >&2; exit 1; }
size_ok build/static/xted
cp build/static/xted "$R/xted-$V-linux-x86_64"
S=$R/stage/xted-$V
mkdir -p "$S/bin" "$S/share/xted/lang" "$S/share/man/man1" "$S/share/man/it/man1" \
         "$S/share/applications" "$S/share/icons/hicolor/scalable/apps" "$S/share/xted/desktop"
cp build/static/xted "$S/bin/xted"
ln -s xted "$S/bin/xt"
cp lang/* "$S/share/xted/lang/"
cp man/xted.1 "$S/share/man/man1/"
cp man/it/xted.1 "$S/share/man/it/man1/"
cp LICENSE README.md "$S/"
cp desktop/xted.desktop "$S/share/applications/"
cp desktop/xted.svg "$S/share/icons/hicolor/scalable/apps/"
cp desktop/dolphin-xted.desktop desktop/xted.nemo_action "$S/share/xted/desktop/"
chmod 755 "$S/bin/xted"
chmod 644 "$S"/share/xted/lang/* "$S"/share/man/man1/* "$S"/share/man/it/man1/* \
          "$S"/share/applications/* "$S"/share/icons/hicolor/scalable/apps/* "$S"/share/xted/desktop/*
tar -C "$R/stage" --owner=0 --group=0 -czf "$R/xted-$V-linux-x86_64.tar.gz" "xted-$V"
rm -rf "$R/stage"

# ---- Windows --------------------------------------------------------------
if [ "$win" = 1 ]; then
    make WIN=1 >/dev/null
    size_ok build/win/xted.exe
    python3 - "$V" "$R" <<'EOF'
import sys, zipfile, os
v, r = sys.argv[1], sys.argv[2]
top = "xted-%s/" % v
with zipfile.ZipFile(os.path.join(r, "xted-%s-windows-x86_64.zip" % v), "w", zipfile.ZIP_DEFLATED) as z:
    z.write("build/win/xted.exe", top + "xted.exe")
    for f in sorted(os.listdir("lang")):
        z.write(os.path.join("lang", f), top + "lang/" + f)
    z.write("README.md", top + "README.md")
    z.write("LICENSE", top + "LICENSE")
EOF
fi

# ---- the installers and the sums ------------------------------------------
cp install.sh install.ps1 "$R/"
(cd "$R" && sha256sum -- * > SHA256SUMS)
echo "release: $R"
ls -l "$R"
