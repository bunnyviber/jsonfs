#!/bin/sh
# JSONfs - fetch optional build-time dependencies into third_party/.
#
#   winfsp   WinFsp FUSE headers (needed to build the WinFsp backend of the
#            Windows driver; without them the Windows build only has Dokan)
#   zig      a zig toolchain (via pip's "ziglang" wheel) for cross compiling
#            to Linux / macOS / FreeBSD / Windows-ARM64
#
# usage: scripts/fetch-deps.sh [winfsp] [zig]      (default: winfsp)
#
# Nothing fetched here is linked into the binaries: the WinFsp headers only
# describe the ABI of winfsp-*.dll, which is loaded at runtime.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP="$ROOT/third_party"
WINFSP_TAG=${WINFSP_TAG:-v2.0}

fetch() { # url dest
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 -o "$2" "$1"
    else
        wget -q -O "$2" "$1"
    fi
}

dep_winfsp() {
    d="$TP/winfsp/inc/fuse"
    mkdir -p "$d"
    for f in fuse.h fuse_common.h fuse_opt.h winfsp_fuse.h; do
        fetch "https://raw.githubusercontent.com/winfsp/winfsp/$WINFSP_TAG/inc/fuse/$f" "$d/$f"
    done
    fetch "https://raw.githubusercontent.com/winfsp/winfsp/$WINFSP_TAG/License.txt" "$TP/winfsp/License.txt"
    echo "WinFsp $WINFSP_TAG FUSE headers -> $d"
}

dep_zig() {
    if command -v zig >/dev/null 2>&1; then
        echo "zig already available: $(command -v zig) ($(zig version))"
        return
    fi
    python3 -m pip install --quiet --target "$TP/ziglang" "ziglang==${ZIG_VERSION:-0.15.1}"
    mkdir -p "$TP/bin"
    cat >"$TP/bin/zig" <<EOF
#!/bin/sh
exec python3 -m ziglang "\$@"
EOF
    chmod +x "$TP/bin/zig"
    # make "python3 -m ziglang" importable from the wrapper
    sed -i.bak "s|^exec python3|PYTHONPATH=\"$TP/ziglang\${PYTHONPATH:+:\$PYTHONPATH}\" exec python3|" "$TP/bin/zig"
    rm -f "$TP/bin/zig.bak"
    echo "zig -> $TP/bin/zig ($("$TP/bin/zig" version))"
}

[ $# -eq 0 ] && set -- winfsp
for dep in "$@"; do
    case "$dep" in
    winfsp) dep_winfsp ;;
    zig) dep_zig ;;
    all) dep_winfsp; dep_zig ;;
    *) echo "unknown dependency: $dep" >&2; exit 2 ;;
    esac
done
