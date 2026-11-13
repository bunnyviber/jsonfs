#!/bin/sh
# JSONfs archive conversion tests (zip / tar / tar.gz built in, more via libarchive).
# usage: tests/archive_test.sh /path/to/jsonfs
# Interoperability checks use whatever is installed: GNU tar, bsdtar, unzip,
# zip, 7z and python3; missing tools skip their checks.
set -u
J=${1:-build/native/bin/jsonfs}
T=$(mktemp -d "${TMPDIR:-/tmp}/jsonfs-ar.XXXXXX")
trap 'rm -rf "$T"' EXIT INT TERM
pass=0
fail=0
LC_ALL=C.UTF-8
export LC_ALL

ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }
check() { d=$1; shift; if "$@" >"$T/out" 2>&1; then ok; else bad "$d"; sed 's/^/    /' "$T/out" | head -20 >&2; fi; }
check_fails() { d=$1; shift; if "$@" >"$T/out" 2>&1; then bad "$d (unexpected success)"; else ok; fi; }
expect() { if [ "$2" = "$3" ]; then ok; else bad "$1: expected [$2] got [$3]"; fi; }
have() { command -v "$1" >/dev/null 2>&1; }
# compare trees; $3 = "nofifo" ignores special files (zip/7z cannot store them)
same_tree() {
    # diff cannot compare FIFOs ("is a fifo while ... is a fifo"): check their type separately
    out=$(diff -r --no-dereference "$1" "$2" 2>&1 | grep -v "is a fifo while file .* is a fifo")
    if [ "${3:-}" = nofifo ]; then
        out=$(printf '%s\n' "$out" | grep -v "^Only in $1: fifo$")
    elif [ -p "$1/fifo" ] && [ ! -p "$2/fifo" ]; then
        echo "fifo missing in $2"
        return 1
    fi
    if printf '%s' "$out" | grep -q .; then
        printf '%s\n' "$out"
        return 1
    fi
    return 0
}
has_libarchive() { "$J" archive-formats | grep -q "libarchive (available)"; }

cd "$T" || exit 1
L=$(printf 'n%.0s' $(seq 1 120))
mkdir -p src/dir/sub "src/ünï cödé" src/empty "src/long/$L/$L"
printf 'hello\n' >src/a.txt
: >src/zero.txt
head -c 300000 /dev/urandom >src/dir/rand.bin
seq 1 30000 >src/dir/sub/seq.txt
ln -s ../a.txt src/dir/link
ln src/a.txt src/hard.txt
printf 'u\n' >"src/ünï cödé/f.txt"
printf 'deep\n' >"src/long/$L/$L/$L.txt"
mkfifo src/fifo 2>/dev/null || true
chmod 750 src/dir
chmod 600 src/zero.txt
touch -d "2010-05-06 07:08:09" src/a.txt
"$J" pack src img.json >/dev/null || bad "pack source tree"

# ---- our archives, extracted by other tools --------------------------------------
for f in out.tar out.tar.gz out.zip; do
    check "write $f" "$J" to-archive img.json "$f"
done
check_fails "refuse to overwrite" "$J" to-archive img.json out.zip
check "overwrite with -f" "$J" to-archive -f img.json out.zip
if have tar && tar --version 2>/dev/null | grep -q GNU; then
    mkdir g1 g2
    check "GNU tar extracts our tar" tar -xf out.tar -C g1
    check "tar content identical" same_tree src g1
    expect "hard link preserved" "$(stat -c %i g1/a.txt)" "$(stat -c %i g1/hard.txt)"
    expect "mtime preserved" "2010-05-06 07:08:09" "$(date -d "@$(stat -c %Y g1/a.txt)" '+%Y-%m-%d %H:%M:%S')"
    expect "mode preserved" "750" "$(stat -c %a g1/dir)"
    check "GNU tar extracts our tar.gz" tar -xzf out.tar.gz -C g2
    check "tar.gz content identical" same_tree src g2
    check "gzip integrity" gzip -t out.tar.gz
fi
if have python3; then
    check "python reads our zip" python3 -c "
import zipfile, sys
z = zipfile.ZipFile('out.zip')
assert z.testzip() is None
names = z.namelist()
assert 'ünï cödé/f.txt' in names, names
assert z.read('dir/sub/seq.txt') == open('src/dir/sub/seq.txt', 'rb').read()
info = z.getinfo('dir/link')
assert (info.external_attr >> 16) & 0o170000 == 0o120000, 'symlink mode'
assert z.read('dir/link') == b'../a.txt'
"
    check "python reads our tar" python3 -c "
import tarfile
t = tarfile.open('out.tar.gz')
names = t.getnames()
assert any(n.endswith('.txt') and len(n) > 255 for n in names), 'long path'
assert t.getmember('hard.txt').islnk() or t.getmember('a.txt').islnk()
"
fi
if have bsdtar; then
    mkdir b1
    check "bsdtar extracts our zip" sh -c "cd b1 && bsdtar -xf ../out.zip"
    check "zip content identical (no special files in zip)" same_tree src b1 nofifo
fi
check "zip64 records on request" "$J" to-archive --zip64 img.json z64.zip
if have python3; then
    check "python reads forced zip64" python3 -c "
import zipfile
z = zipfile.ZipFile('z64.zip'); assert z.testzip() is None
assert z.read('dir/rand.bin') == open('src/dir/rand.bin','rb').read()"
fi
check "pax headers (ns times) on request" "$J" to-archive --pax img.json pax.tar
if have tar && tar --version 2>/dev/null | grep -q GNU; then
    mkdir g3 && check "GNU tar extracts pax" tar -xf pax.tar -C g3 && check "pax identical" same_tree src g3
    expect "pax keeps nanoseconds" "$(stat -c %y src/dir/sub/seq.txt)" "$(stat -c %y g3/dir/sub/seq.txt)"
fi

# ---- archives from other tools, imported -------------------------------------------
roundtrip() { # desc archive [from-archive options...]
    d=$1
    a=$2
    shift 2
    rm -rf rt rt.json rt.json.lock
    if ! "$J" from-archive "$@" "$a" rt.json >"$T/out" 2>&1; then
        bad "$d: import"
        sed 's/^/    /' "$T/out" | head -5 >&2
        return
    fi
    ok
    "$J" unpack rt.json rt >/dev/null 2>&1 || bad "$d: unpack"
    if [ "${NOFIFO:-}" = 1 ]; then check "$d: identical" same_tree src rt nofifo; else check "$d: identical" same_tree src rt; fi
    check "$d: fsck" "$J" fsck rt.json
}
roundtrip "our tar" out.tar
roundtrip "our tar.gz" out.tar.gz
NOFIFO=1 roundtrip "our zip" out.zip
NOFIFO=1 roundtrip "our zip64" z64.zip
if have tar && tar --version 2>/dev/null | grep -q GNU; then
    tar --format=gnu -cf gnu.tar -C src . && roundtrip "GNU tar (gnu format, long names)" gnu.tar
    tar --format=posix -cf posix.tar -C src . && roundtrip "GNU tar (posix/pax format)" posix.tar
    tar --format=gnu -czf gnu.tgz -C src . && roundtrip "GNU tar.gz" gnu.tgz
    rm -rf src_short && cp -a src src_short && rm -rf "src_short/long"
    tar --format=ustar -cf ustar.tar -C src_short . 2>/dev/null && {
        rm -rf rt rt.json
        check "ustar import" "$J" from-archive ustar.tar rt.json
        "$J" unpack rt.json rt >/dev/null && check "ustar identical" same_tree src_short rt
    }
fi
if have zip; then
    (cd src && zip -qry ../infozip.zip .) && NOFIFO=1 roundtrip "Info-ZIP zip (symlinks)" infozip.zip
    if zip -h2 2>/dev/null | grep -q bzip2; then
        (cd src && zip -qry -Z bzip2 ../bz.zip .) && {
            if has_libarchive; then
                NOFIFO=1 roundtrip "zip with bzip2 members (libarchive fallback)" bz.zip
            else
                check_fails "zip with bzip2 members needs libarchive" "$J" from-archive bz.zip bzx.json
            fi
        }
    fi
fi
if have python3; then
    python3 - <<'EOF'
import zipfile, os
# zip64 extra fields in local headers, no directory entries, Windows-style attributes
with zipfile.ZipFile('py.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    with z.open('a/b/c.txt', 'w', force_zip64=True) as f:
        f.write(b'zip64 local header\n')
    z.writestr('a/plain.txt', b'plain\n')
    z.writestr(zipfile.ZipInfo('stored.bin'), os.urandom(1000))
import tarfile, io
with tarfile.open('evil.tar', 'w') as t:
    for name, data in (('../evil.txt', b'evil'), ('/abs/path.txt', b'abs'), ('ok/fine.txt', b'fine'),
                       ('ok/../../escape.txt', b'esc')):
        info = tarfile.TarInfo(name); info.size = len(data); t.addfile(info, io.BytesIO(data))
EOF
    check "python zip (zip64 local headers, implicit dirs)" "$J" from-archive py.zip py.json
    check "implicit directories created" "$J" cat py.json /a/b/c.txt
    expect "zip64 local entry content" "zip64 local header" "$(cat "$T/out")"
    check "hostile tar paths are contained" "$J" from-archive evil.tar evil.json
    check "listing" "$J" ls -R evil.json
    grep -q "evil.txt\|escape.txt" "$T/out" && bad "'..' entries must be skipped" || ok
    check "absolute paths land inside the image" "$J" cat evil.json /abs/path.txt
    check "normal entries kept" "$J" cat evil.json /ok/fine.txt
fi

# ---- libarchive formats ----------------------------------------------------------------
if has_libarchive; then
    for f in out.7z out.tar.xz out.tar.bz2 out.tar.zst out.cpio; do
        check "write $f (libarchive)" "$J" to-archive img.json "$f"
    done
    NOFIFO=1 roundtrip "7z round trip" out.7z
    roundtrip "tar.xz round trip" out.tar.xz
    roundtrip "tar.bz2 round trip" out.tar.bz2
    roundtrip "tar.zst round trip" out.tar.zst
    roundtrip "cpio round trip" out.cpio
    if have bsdtar; then
        mkdir b2 && check "bsdtar extracts our tar.xz" sh -c "cd b2 && bsdtar -xf ../out.tar.xz" &&
            check "tar.xz identical" same_tree src b2
    fi
    if have 7z; then
        (cd src_short 2>/dev/null || cd src; 7z a -bd -snl ../ext.7z . >/dev/null) &&
            check "import a 7z made by 7-Zip" "$J" from-archive ext.7z ext7.json
    fi
    check "native backend refuses 7z" sh -c "! '$J' from-archive --backend native out.7z n.json"
    check "libarchive backend for zip too" "$J" to-archive --backend libarchive img.json la.zip
    NOFIFO=1 roundtrip "zip written by libarchive" la.zip
else
    check_fails "7z needs libarchive" "$J" to-archive img.json no.7z
fi

# ---- errors, convert, --into, --at -------------------------------------------------------
head -c 3000 out.tar >trunc.tar
check_fails "truncated tar is rejected" "$J" from-archive trunc.tar t.json
check "failed import leaves no image" test ! -e t.json
cp out.zip badcrc.zip
python3 -c "
d = bytearray(open('badcrc.zip','rb').read()); i = d.find(b'hello'); d[i] ^= 1
open('badcrc.zip','wb').write(d)" 2>/dev/null && check_fails "zip CRC mismatch is rejected" "$J" from-archive badcrc.zip b.json
check_fails "unknown output format" "$J" to-archive img.json out.unknown
check "convert archive -> image" "$J" convert out.tar.gz conv.json
check "convert image -> archive" "$J" convert conv.json conv.zip
NOFIFO=1 roundtrip "convert round trip" conv.zip
check "mkdir target" "$J" mkdir img.json /imported
check "--into adds below a folder" "$J" from-archive --into /imported/zipped out.zip img.json
check "--into content" "$J" cat img.json /imported/zipped/a.txt
expect "--into content value" "hello" "$(cat "$T/out")"
check "export a subdirectory" "$J" to-archive img.json sub.tar /dir
check "subdirectory archive layout" sh -c "tar -tf sub.tar | grep -qx 'sub/seq.txt'"
check "archive formats listing" "$J" archive-formats

echo "archive tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
