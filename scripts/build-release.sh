#!/bin/sh
# JSONfs release pipeline: builds every (or the selected) target and
# packages it.
#
# usage: scripts/build-release.sh [options] [all | unix | windows | TARGET...]
#
# options:
#   --test             run the native test-suite first
#   --bundle-drivers   additionally build Windows setup programs that embed the
#                      WinFsp 2.0 MSI and the Dokan 0.6.0 installer (downloaded
#                      into third_party/drivers and checked against pinned
#                      SHA-256 sums), plus the drivers' source archives
#   --jobs N           parallel make jobs (default: number of CPUs)
#
# Outputs (dist/):
#   jsonfs-VER-<os>-<arch>.tar.gz          binaries + install.sh + docs
#   jsonfs-VER-<os>-<arch>.run             self-extracting installer/uninstaller
#   jsonfs-VER-windows-<arch>.zip          portable Windows binaries
#   jsonfs-VER-windows-<arch>-setup.exe    NSIS installer/uninstaller (needs makensis)
#   jsonfs-VER-windows-<arch>-setup-with-drivers.exe   same, with WinFsp/Dokan inside (--bundle-drivers)
#   winfsp-2.0-source.tar.gz, dokan-0.6.0-source.tar.gz  sources of the bundled drivers (--bundle-drivers)
#   SHA256SUMS
# docs/PACKAGES.md describes every file.
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

# Driver packages embedded in the "-with-drivers" Windows installers.  The
# versions are pinned and verified: a changed upstream file aborts the build.
WINFSP_URL="https://github.com/winfsp/winfsp/releases/download/v2.0/winfsp-2.0.23075.msi"
WINFSP_SHA256="6324dc81194a6a08f97b6aeca303cf5c2325c53ede153bae9fc4378f0838c101"
WINFSP_SRC="https://github.com/winfsp/winfsp/archive/refs/tags/v2.0.tar.gz"
DOKAN_URL="https://github.com/dokan-dev/dokany/releases/download/0.6.0/DokanInstall_0.6.0.exe"
DOKAN_SHA256="a92ebb9f5baa83aa33a186d329eed2a2d063448432584f5bd88fd53bd44c0b3b"
DOKAN_SRC="https://github.com/dokan-dev/dokany/archive/refs/tags/0.6.0.tar.gz"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

fetch_pinned() { # url sha256 dest
    if [ ! -f "$3" ] || [ "$(sha256_of "$3")" != "$2" ]; then
        curl -fsSL --retry 3 -o "$3.part" "$1"
        mv "$3.part" "$3"
    fi
    if [ "$(sha256_of "$3")" != "$2" ]; then
        echo "error: $3 does not match the pinned SHA-256 ($2)" >&2
        exit 1
    fi
}

WINFSP_MSI=""
DOKAN_EXE=""
if [ "$BUNDLE" = 1 ]; then
    log "fetching driver installers"
    mkdir -p third_party/drivers
    WINFSP_MSI="$ROOT/third_party/drivers/winfsp-2.0.23075.msi"
    DOKAN_EXE="$ROOT/third_party/drivers/DokanInstall_0.6.0.exe"
    fetch_pinned "$WINFSP_URL" "$WINFSP_SHA256" "$WINFSP_MSI"
    fetch_pinned "$DOKAN_URL" "$DOKAN_SHA256" "$DOKAN_EXE"
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
    cp README.md LICENSE THIRD_PARTY_NOTICES.md "$st/"
    cp docs/FORMAT.md docs/USAGE.md docs/BUILDING.md docs/PACKAGES.md "$st/docs/"
    case "$t" in
    windows-*)
        cp "build/$t/bin/jsonfs.exe" "build/$t/bin/jsonfs-mount.exe" "$st/bin/"
        zipdir "$st" "$DIST/$name.zip"
        if command -v makensis >/dev/null 2>&1; then
            arch=${t#windows-}
            # without drivers: the installer points to the driver download pages
            makensis -V2 -DVERSION="$VERSION" -DARCH="$arch" -DBINDIR="$ROOT/build/$t/bin" -DSRCROOT="$ROOT" \
                -DOUTFILE="$DIST/$name-setup.exe" installer/windows/jsonfs.nsi
            if [ "$BUNDLE" = 1 ]; then
                # with drivers: WinFsp (Windows 7+, all architectures) and
                # Dokan 0.6 (XP/Vista, x86/x64 only) are installed when missing
                set -- -V2 -DVERSION="$VERSION" -DARCH="$arch" -DBINDIR="$ROOT/build/$t/bin" -DSRCROOT="$ROOT" \
                    -DOUTFILE="$DIST/$name-setup-with-drivers.exe" -DWINFSP_MSI="$WINFSP_MSI"
                [ "$arch" != arm64 ] && set -- "$@" -DDOKAN_EXE="$DOKAN_EXE"
                makensis "$@" installer/windows/jsonfs.nsi
            fi
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

if [ "$BUNDLE" = 1 ]; then
    # Corresponding source of the redistributed drivers (Dokan is LGPL; WinFsp
    # is GPLv3 with a FLOSS exception) published next to the installers.
    log "driver source archives"
    if ! curl -fsSL --retry 3 -o "$DIST/winfsp-2.0-source.tar.gz" "$WINFSP_SRC" ||
        ! curl -fsSL --retry 3 -o "$DIST/dokan-0.6.0-source.tar.gz" "$DOKAN_SRC"; then
        echo "error: cannot download the driver sources (needed when redistributing the drivers)" >&2
        exit 1
    fi
fi

log "checksums"
(cd "$DIST" && if command -v sha256sum >/dev/null 2>&1; then sha256sum -- *; else shasum -a 256 -- *; fi) >"$DIST/SHA256SUMS.tmp"
mv "$DIST/SHA256SUMS.tmp" "$DIST/SHA256SUMS"
ls -l "$DIST"
