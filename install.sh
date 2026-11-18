#!/bin/sh
# Installs xted from a GitHub release: the static binary, the xt link, the language catalogs and
# the manual pages. Nothing is compiled.
#
#   curl -fsSL https://github.com/lejubila/xted/releases/latest/download/install.sh | sh
#
# Settings, in the environment:
#   PREFIX=/dir         where to install (default: /usr/local for root, ~/.local otherwise)
#   XTED_VERSION=0.9.0  a given release (default: the latest)
#   XTED_URL=url        where the release files are (a mirror; default: GitHub)
# Arguments: --uninstall removes what it installs, --help prints this.
#   curl -fsSL .../install.sh | sh -s -- --uninstall
set -eu

REPO=lejubila/xted

say() { printf 'xted: %s\n' "$*"; }
die() { printf 'xted: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Installs xted (a text editor for the terminal) from a GitHub release.

  sh install.sh [--uninstall]

  PREFIX=/dir         where to install (default: /usr/local for root, ~/.local otherwise)
  XTED_VERSION=0.9.0  a given release (default: the latest)
  XTED_URL=url        where the release files are (a mirror)
EOF
}

uninstall=0
for a in "$@"; do
    case $a in
    --uninstall) uninstall=1 ;;
    -h | --help) usage; exit 0 ;;
    *) die "unknown argument: $a (see --help)" ;;
    esac
done

if [ -z "${PREFIX:-}" ]; then
    if [ "$(id -u)" = 0 ]; then
        PREFIX=/usr/local
    else
        [ -n "${HOME:-}" ] || die "HOME is not set: give the folder with PREFIX=/dir"
        PREFIX=$HOME/.local
    fi
fi
BIN=$PREFIX/bin
LANGDIR=$PREFIX/share/xted/lang
MAN=$PREFIX/share/man

# ---- uninstall ------------------------------------------------------------
if [ "$uninstall" = 1 ]; then
    [ -e "$BIN/xted" ] || die "xted is not installed in $PREFIX (set PREFIX to the folder used)"
    rm -f "$BIN/xted" "$MAN/man1/xted.1" "$MAN/it/man1/xted.1"
    # xt only when it is our link
    if [ -L "$BIN/xt" ] && [ "$(readlink "$BIN/xt")" = xted ]; then
        rm -f "$BIN/xt"
    fi
    rm -f "$LANGDIR"/*
    rmdir "$LANGDIR" "$PREFIX/share/xted" 2>/dev/null || true
    say "removed from $PREFIX (the settings in ~/.config/xted are left)"
    exit 0
fi

# ---- the system -----------------------------------------------------------
[ "$(uname -s)" = Linux ] || die "only Linux has a ready binary (this is $(uname -s)): build it from the source with make"
case $(uname -m) in
x86_64 | amd64) ARCH=x86_64 ;;
*) die "only x86_64 has a ready binary (this is $(uname -m)): build it from the source with make" ;;
esac

if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL "$1" -o "$2"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -q -O "$2" "$1"; }
else
    die "curl or wget is needed to download"
fi
if command -v sha256sum >/dev/null 2>&1; then
    sum() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    sum() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    die "sha256sum is needed to check the download"
fi

if [ -n "${XTED_URL:-}" ]; then
    base=$XTED_URL
elif [ -n "${XTED_VERSION:-}" ]; then
    base=https://github.com/$REPO/releases/download/v${XTED_VERSION#v}
else
    base=https://github.com/$REPO/releases/latest/download
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/xted.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM

# ---- download and check ---------------------------------------------------
# SHA256SUMS names the archive, so it also says which version "latest" is
fetch "$base/SHA256SUMS" "$tmp/SHA256SUMS" || die "cannot download $base/SHA256SUMS"
line=$(grep "linux-$ARCH\.tar\.gz\$" "$tmp/SHA256SUMS" | head -n 1)
[ -n "$line" ] || die "the release has no archive for linux-$ARCH"
want=${line%% *}
name=${line##* }
name=${name#\*}
say "downloading $name"
fetch "$base/$name" "$tmp/$name" || die "cannot download $base/$name"
[ "$(sum "$tmp/$name")" = "$want" ] || die "$name does not match its SHA256SUMS: not installed"

tar -xzf "$tmp/$name" -C "$tmp" || die "cannot unpack $name"
src=
for d in "$tmp"/xted-*/; do
    [ -f "$d/bin/xted" ] && src=${d%/}
done
[ -n "$src" ] || die "$name has no bin/xted"

# ---- install --------------------------------------------------------------
if ! mkdir -p "$BIN" 2>/dev/null || [ ! -w "$BIN" ]; then
    die "cannot write in $BIN: run it as root (curl ... | sudo sh) or choose another PREFIX"
fi
mkdir -p "$LANGDIR" "$MAN/man1" "$MAN/it/man1"

# through a new file and a rename: an xted that is running keeps its own binary
cp "$src/bin/xted" "$BIN/.xted.new"
chmod 755 "$BIN/.xted.new"
mv -f "$BIN/.xted.new" "$BIN/xted"
also=" (also as xt)"
if [ -e "$BIN/xt" ] && ! { [ -L "$BIN/xt" ] && [ "$(readlink "$BIN/xt")" = xted ]; }; then
    say "$BIN/xt exists and is not a link to xted: left as it is"
    also=
else
    ln -sf xted "$BIN/xt"
fi
for f in "$src"/share/xted/lang/*; do
    [ -f "$f" ] && cp "$f" "$LANGDIR/" && chmod 644 "$LANGDIR/${f##*/}"
done
[ -f "$src/share/man/man1/xted.1" ] && cp "$src/share/man/man1/xted.1" "$MAN/man1/"
[ -f "$src/share/man/it/man1/xted.1" ] && cp "$src/share/man/it/man1/xted.1" "$MAN/it/man1/"

say "installed $("$BIN/xted" --version | head -n 1) in $BIN$also"
case ":${PATH:-}:" in
*":$BIN:"*) ;;
*)
    say "$BIN is not in PATH: add this line to ~/.profile (or ~/.bashrc), then open a new terminal:"
    printf '\n    export PATH="%s:$PATH"\n\n' "$BIN"
    ;;
esac
