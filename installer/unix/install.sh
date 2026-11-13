#!/bin/sh
# JSONfs installer / uninstaller for Linux, macOS and FreeBSD.
#
#   install.sh [install] [--prefix DIR] [--from BINDIR] [--no-mount-helper] [--yes]
#   install.sh uninstall [--prefix DIR]
#   install.sh status    [--prefix DIR]
#
# Release bundles (jsonfs-<version>-<os>-<arch>.run) embed this script; run
# them as  "sh jsonfs-....run [install|uninstall|status] [options]".
#
# What gets installed:
#   PREFIX/bin/jsonfs                 maintenance toolkit
#   PREFIX/bin/jsonfs-mount           mount driver (FUSE)
#   PREFIX/sbin/mkfs.jsonfs           -> jsonfs   (mkfs(8) convention)
#   PREFIX/sbin/fsck.jsonfs           -> jsonfs   (fsck(8) convention)
#   /sbin/mount.jsonfs                -> jsonfs-mount (Linux: "mount -t jsonfs", fstab)
#   PREFIX/sbin/mount_jsonfs          -> jsonfs-mount (FreeBSD / macOS naming)
#   PREFIX/share/doc/jsonfs/*         documentation
#   PREFIX/share/jsonfs/manifest      list of installed files (used to uninstall)
#
# The kernel side is provided by the operating system's FUSE implementation:
#   Linux   libfuse 3 or 2 (package fuse3 / fuse)
#   macOS   macFUSE (https://osxfuse.github.io) or FUSE-T (https://www.fuse-t.org)
#   FreeBSD fusefs kernel module + fusefs-libs (pkg install fusefs-libs3)
set -eu

ACTION=install
PREFIX=/usr/local
FROM=
HELPER=1
YES=0
HERE=$(cd "$(dirname "$0")" && pwd)

while [ $# -gt 0 ]; do
    case "$1" in
    install | uninstall | status) ACTION=$1 ;;
    --prefix) PREFIX=$2; shift ;;
    --prefix=*) PREFIX=${1#--prefix=} ;;
    --from) FROM=$2; shift ;;
    --from=*) FROM=${1#--from=} ;;
    --no-mount-helper) HELPER=0 ;;
    -y | --yes) YES=1 ;;
    -h | --help)
        sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

OS=$(uname -s)
MANIFEST="$PREFIX/share/jsonfs/manifest"
say() { printf '%s\n' "$*"; }
need_root() {
    if [ ! -w "$PREFIX" ] && [ "$(id -u)" -ne 0 ]; then
        say "error: $PREFIX is not writable; re-run with sudo (or use --prefix \$HOME/.local)" >&2
        exit 1
    fi
}

# Locate the binaries: --from, the bundle layout (bin/ next to us), or a build tree.
find_bins() {
    for d in "$FROM" "$HERE/bin" "$HERE/../../build/native/bin" "$HERE/../bin"; do
        [ -n "$d" ] && [ -x "$d/jsonfs" ] && [ -x "$d/jsonfs-mount" ] && { BIN_SRC=$(cd "$d" && pwd); return 0; }
    done
    say "error: cannot find the jsonfs binaries (use --from DIR)" >&2
    exit 1
}

record() { printf '%s\n' "$1" >>"$MANIFEST.new"; }

put() { # src dest mode
    mkdir -p "$(dirname "$2")"
    rm -f "$2"
    cp "$1" "$2"
    chmod "$3" "$2"
    record "$2"
}

link() { # target linkname
    mkdir -p "$(dirname "$2")"
    rm -f "$2"
    ln -s "$1" "$2"
    record "$2"
}

check_fuse() {
    case "$OS" in
    Linux)
        if ! (ldconfig -p 2>/dev/null | grep -qE 'libfuse3\.so\.3|libfuse\.so\.2'); then
            say "note: no libfuse found. Install it to mount images, e.g.:"
            say "        apt install fuse3   |   dnf install fuse3   |   pacman -S fuse3   |   zypper in fuse3"
        fi
        [ -e /dev/fuse ] || say "note: /dev/fuse is missing (modprobe fuse)"
        ldconfig -p 2>/dev/null | grep -q 'libcurl' ||
            say "note: install libcurl (libcurl4 / libcurl) to mount http:// and https:// images"
        ;;
    Darwin)
        if [ ! -e /usr/local/lib/libfuse-t.dylib ] && [ ! -e /usr/local/lib/libfuse.2.dylib ] &&
            [ ! -e /opt/homebrew/lib/libfuse.2.dylib ]; then
            say "note: install macFUSE (https://osxfuse.github.io) or FUSE-T (https://www.fuse-t.org,"
            say "      'brew install macos-fuse-t/homebrew-cask/fuse-t') to mount images."
        fi
        ;;
    FreeBSD)
        kldstat -q -m fusefs 2>/dev/null || kldstat -q -m fuse 2>/dev/null ||
            say "note: load the FUSE module: kldload fusefs && sysrc kld_list+=fusefs"
        if [ ! -e /usr/local/lib/libfuse3.so.3 ] && [ ! -e /usr/local/lib/libfuse.so.2 ]; then
            say "note: install the FUSE library: pkg install fusefs-libs3"
        fi
        say "note: to mount as a regular user: sysctl vfs.usermount=1"
        [ -e /usr/local/lib/libcurl.so.4 ] || say "note: pkg install curl  (needed for http:// and https:// images)"
        ;;
    esac
}

do_install() {
    find_bins
    if ! "$BIN_SRC/jsonfs" version >/dev/null 2>&1; then
        say "error: $BIN_SRC/jsonfs does not run on this system (wrong OS/architecture bundle?)" >&2
        exit 1
    fi
    need_root
    if [ -f "$MANIFEST" ]; then
        say "removing previous installation first"
        do_uninstall quiet
    fi
    mkdir -p "$(dirname "$MANIFEST")"
    : >"$MANIFEST.new"
    put "$BIN_SRC/jsonfs" "$PREFIX/bin/jsonfs" 755
    put "$BIN_SRC/jsonfs-mount" "$PREFIX/bin/jsonfs-mount" 755
    link "$PREFIX/bin/jsonfs" "$PREFIX/sbin/mkfs.jsonfs"
    link "$PREFIX/bin/jsonfs" "$PREFIX/sbin/fsck.jsonfs"
    link "$PREFIX/bin/jsonfs-mount" "$PREFIX/sbin/mount_jsonfs"
    if [ "$HELPER" = 1 ] && [ "$OS" = Linux ]; then
        if [ -w /sbin ] || [ "$(id -u)" -eq 0 ]; then
            link "$PREFIX/bin/jsonfs-mount" /sbin/mount.jsonfs
        else
            say "note: not root, skipping /sbin/mount.jsonfs (mount -t jsonfs / fstab support)"
        fi
    fi
    for doc in README.md LICENSE docs/FORMAT.md docs/BUILDING.md docs/USAGE.md; do
        for base in "$HERE" "$HERE/../.."; do
            if [ -f "$base/$doc" ]; then
                put "$base/$doc" "$PREFIX/share/doc/jsonfs/$(basename "$doc")" 644
                break
            fi
        done
    done
    # keep a copy of this script so "jsonfs-uninstall" works without the bundle
    put "$0" "$PREFIX/share/jsonfs/install.sh" 755
    mkdir -p "$PREFIX/bin"
    cat >"$PREFIX/bin/jsonfs-uninstall" <<EOF
#!/bin/sh
exec sh "$PREFIX/share/jsonfs/install.sh" uninstall --prefix "$PREFIX" "\$@"
EOF
    chmod 755 "$PREFIX/bin/jsonfs-uninstall"
    record "$PREFIX/bin/jsonfs-uninstall"
    record "$MANIFEST"
    mv "$MANIFEST.new" "$MANIFEST"
    say "JSONfs $("$PREFIX/bin/jsonfs" version | awk '{print $2}') installed into $PREFIX"
    say "  create an image:  jsonfs mkfs my.json"
    say "  mount it:         jsonfs-mount my.json /mnt/point"
    say "  uninstall:        jsonfs-uninstall"
    check_fuse
}

do_uninstall() {
    if [ ! -f "$MANIFEST" ]; then
        [ "${1:-}" = quiet ] && return 0
        say "JSONfs is not installed in $PREFIX (no $MANIFEST)" >&2
        exit 1
    fi
    need_root
    # refuse while images are mounted through this installation
    if mount 2>/dev/null | grep -q 'jsonfs'; then
        say "warning: JSONfs filesystems are still mounted:"
        mount | grep jsonfs | sed 's/^/  /'
        if [ "$YES" != 1 ] && [ "${1:-}" != quiet ]; then
            say "unmount them first (or pass --yes to uninstall anyway)" >&2
            exit 1
        fi
    fi
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        if [ -L "$f" ] || [ -f "$f" ]; then rm -f "$f"; fi
    done <"$MANIFEST"
    rm -f "$MANIFEST"
    for d in "$PREFIX/share/doc/jsonfs" "$PREFIX/share/jsonfs" "$PREFIX/sbin"; do
        rmdir "$d" 2>/dev/null || true
    done
    [ "${1:-}" = quiet ] || say "JSONfs removed from $PREFIX"
}

do_status() {
    if [ -f "$MANIFEST" ]; then
        say "JSONfs is installed in $PREFIX:"
        sed 's/^/  /' "$MANIFEST"
        [ -x "$PREFIX/bin/jsonfs" ] && "$PREFIX/bin/jsonfs" version
    else
        say "JSONfs is not installed in $PREFIX"
        exit 1
    fi
    check_fuse
}

case "$ACTION" in
install) do_install ;;
uninstall) do_uninstall ;;
status) do_status ;;
esac
