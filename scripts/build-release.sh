#!/bin/sh
# JSONfs release pipeline: builds every (or the selected) target and
# packages it.
#
# usage: scripts/build-release.sh [options] [all | unix | windows | TARGET...]
#
# options:
#   --test             run the native test-suite first
#   --bundle-drivers   embed the WinFsp MSI and the Dokan 0.6.0 installer in the
#                      Windows setup programs (downloaded into third_party/)
#   --jobs N           parallel make jobs (default: number of CPUs)
#
# Outputs (dist/):
#   jsonfs-VER-<os>-<arch>.tar.gz          binaries + install.sh + docs
#   jsonfs-VER-<os>-<arch>.run             self-extracting installer/uninstaller
#   jsonfs-VER-windows-<arch>.zip          portable Windows binaries
#   jsonfs-VER-windows-<arch>-setup.exe    NSIS installer/uninstaller (needs makensis)
#   SHA256SUMS
#
# Toolchains: zig (>= 0.15) for Linux/macOS/FreeBSD/Windows-ARM64, MinGW-w64
# for Windows x86/x64 (XP compatible), makensis for the Windows installers.
# "scripts/fetch-deps.sh all" installs zig and the WinFsp headers locally.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
VERSION=$(sed -n 's/^VERSION *:= *//p' Makefile)
DIST="$ROOT/dist"
UNIX_TARGETS="linux-x86_64 linux-i686 linux-aarch64 linux-armv7 macos-x86_64 macos-arm64 freebsd-x86_64 freebsd-aarch64"
WIN_TARGETS="windows-x86 windows-x64 windows-arm64"
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
TEST=0
BUNDLE=0
TARGETS=""

while [ $# -gt 0 ]; do
    case "$1" in
    --test) TEST=1 ;;
    --bundle-drivers) BUNDLE=1 ;;
    --jobs) JOBS=$2; shift ;;
    all) TARGETS="$TARGETS $UNIX_TARGETS $WIN_TARGETS" ;;
    unix) TARGETS="$TARGETS $UNIX_TARGETS" ;;
    windows) TARGETS="$TARGETS $WIN_TARGETS" ;;
    -h | --help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) TARGETS="$TARGETS $1" ;;
    esac
    shift
done
[ -n "$TARGETS" ] || TARGETS="$UNIX_TARGETS $WIN_TARGETS"

log() { printf '\n==> %s\n' "$*"; }

# ---- toolchains -------------------------------------------------------------
ZIG=${ZIG:-}
if [ -z "$ZIG" ]; then
    if [ -x "$ROOT/third_party/bin/zig" ]; then
        ZIG="$ROOT/third_party/bin/zig"
    elif command -v zig >/dev/null 2>&1; then
        ZIG=zig
    fi
fi
needs_zig=0
for t in $TARGETS; do
    case "$t" in windows-x86 | windows-x64) ;; *) needs_zig=1 ;; esac
done
if [ "$needs_zig" = 1 ] && [ -z "$ZIG" ]; then
    log "zig not found, fetching it (scripts/fetch-deps.sh zig)"
    sh scripts/fetch-deps.sh zig
    ZIG="$ROOT/third_party/bin/zig"
fi
if [ ! -f third_party/winfsp/inc/fuse/fuse.h ]; then
    log "fetching WinFsp headers"
    sh scripts/fetch-deps.sh winfsp || echo "warning: building Windows drivers without the WinFsp backend"
fi

WINFSP_MSI=""
DOKAN_EXE=""
if [ "$BUNDLE" = 1 ]; then
    mkdir -p third_party/drivers
    WINFSP_MSI="$ROOT/third_party/drivers/winfsp.msi"
    DOKAN_EXE="$ROOT/third_party/drivers/DokanInstall_0.6.0.exe"
    [ -f "$WINFSP_MSI" ] || curl -fsSL -o "$WINFSP_MSI" \
        "https://github.com/winfsp/winfsp/releases/download/v2.0/winfsp-2.0.23075.msi"
    [ -f "$DOKAN_EXE" ] || curl -fsSL -o "$DOKAN_EXE" \
        "https://github.com/dokan-dev/dokany/releases/download/0.6.0/DokanInstall_0.6.0.exe"
fi

if [ "$TEST" = 1 ]; then
    log "native build + tests"
    make -j"$JOBS" TARGET=native
    make TARGET=native test
fi

rm -rf "$DIST"
mkdir -p "$DIST/stage"

zipdir() { # dir zipfile
    if command -v zip >/dev/null 2>&1; then
        (cd "$(dirname "$1")" && zip -qr "$2" "$(basename "$1")")
    else
        (cd "$(dirname "$1")" && python3 -m zipfile -c "$2" "$(basename "$1")")
    fi
}

for t in $TARGETS; do
    log "building $t"
    make -j"$JOBS" TARGET="$t" ZIG="${ZIG:-zig}" OPT="-O2" LDFLAGS="-s" all
    name="jsonfs-$VERSION-$t"
    st="$DIST/stage/$name"
    mkdir -p "$st/bin" "$st/docs"
    cp README.md LICENSE "$st/"
    cp docs/FORMAT.md docs/USAGE.md docs/BUILDING.md "$st/docs/"
    case "$t" in
    windows-*)
        cp "build/$t/bin/jsonfs.exe" "build/$t/bin/jsonfs-mount.exe" "$st/bin/"
        zipdir "$st" "$DIST/$name.zip"
        if command -v makensis >/dev/null 2>&1; then
            arch=${t#windows-}
            set -- -V2 -DVERSION="$VERSION" -DARCH="$arch" -DBINDIR="$ROOT/build/$t/bin" -DSRCROOT="$ROOT" \
                -DOUTFILE="$DIST/$name-setup.exe"
            [ -n "$WINFSP_MSI" ] && set -- "$@" -DWINFSP_MSI="$WINFSP_MSI"
            [ -n "$DOKAN_EXE" ] && [ "$arch" != arm64 ] && set -- "$@" -DDOKAN_EXE="$DOKAN_EXE"
            makensis "$@" installer/windows/jsonfs.nsi
        else
            echo "warning: makensis not found, skipping $name-setup.exe"
        fi
        ;;
    *)
        cp "build/$t/bin/jsonfs" "build/$t/bin/jsonfs-mount" "$st/bin/"
        cp installer/unix/install.sh "$st/install.sh"
        chmod 755 "$st/install.sh" "$st/bin/"*
        (cd "$DIST/stage" && tar cf - "$name" | gzip -9 >"$DIST/$name.tar.gz")
        sed -e "s/@VERSION@/$VERSION/g" -e "s/@TARGET@/$t/g" installer/unix/selfextract.sh >"$DIST/$name.run"
        (cd "$st" && tar cf - .) | gzip -9 >>"$DIST/$name.run"
        chmod 755 "$DIST/$name.run"
        ;;
    esac
done
rm -rf "$DIST/stage"

log "checksums"
(cd "$DIST" && if command -v sha256sum >/dev/null 2>&1; then sha256sum -- *; else shasum -a 256 -- *; fi) >"$DIST/SHA256SUMS.tmp"
mv "$DIST/SHA256SUMS.tmp" "$DIST/SHA256SUMS"
ls -l "$DIST"
