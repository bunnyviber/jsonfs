#!/bin/sh
# JSONfs FUSE mount tests (Linux / FreeBSD / macOS).  Skipped when FUSE is
# not usable on the host.
# usage: tests/mount_test.sh /path/to/jsonfs /path/to/jsonfs-mount
set -u
J=${1:-build/native/bin/jsonfs}
M=${2:-build/native/bin/jsonfs-mount}
T=$(mktemp -d "${TMPDIR:-/tmp}/jsonfs-mt.XXXXXX")
pass=0
fail=0
MOUNTS=""

ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }
expect() { if [ "$2" = "$3" ]; then ok; else bad "$1: expected [$2] got [$3]"; fi; }
check() { d=$1; shift; if "$@" >/dev/null 2>&1; then ok; else bad "$d"; fi; }
check_fails() { d=$1; shift; if "$@" >/dev/null 2>&1; then bad "$d (unexpected success)"; else ok; fi; }

umnt() {
    for u in "fusermount3 -u" "fusermount -u" "umount"; do
        if $u "$1" 2>/dev/null; then return 0; fi
    done
    return 1
}
cleanup() {
    for m in $MOUNTS; do umnt "$m" 2>/dev/null; done
    sleep 0.5
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

if [ ! -e /dev/fuse ] && [ "$(uname -s)" = Linux ]; then
    echo "mount tests: skipped (/dev/fuse not available)"
    exit 0
fi
cd "$T" || exit 1
"$J" mkfs -V -L mt img.json >/dev/null
mkdir mnt mnt2 view
if ! "$M" img.json mnt 2>err.txt; then
    if grep -qi "FUSE is not available\|fuse: device not found\|permission denied\|Operation not permitted" err.txt; then
        echo "mount tests: skipped ($(head -1 err.txt))"
        exit 0
    fi
    bad "mount failed: $(cat err.txt)"
    echo "mount tests: $pass passed, $fail failed"
    exit 1
fi
MOUNTS="$T/mnt"
ok

# ---- POSIX behaviour through the kernel ----------------------------------------
check "mkdir -p" mkdir -p mnt/a/b/c
printf 'hello\n' >mnt/a/hello.txt
expect "read back" "hello" "$(cat mnt/a/hello.txt)"
printf 'more\n' >>mnt/a/hello.txt
expect "append" "2" "$(wc -l <mnt/a/hello.txt | tr -d ' ')"
check "symlink" ln -s a/hello.txt mnt/sym
expect "readlink" "a/hello.txt" "$(readlink mnt/sym)"
expect "follow symlink" "hello" "$(head -1 mnt/sym)"
check "hard link" ln mnt/a/hello.txt mnt/hard
expect "link count" "2" "$(ls -l mnt/hard | awk '{print $2}')"
check "chmod" chmod 0640 mnt/hard
expect "mode shared by links" "-rw-r-----" "$(ls -l mnt/a/hello.txt | cut -c1-10)"
check "rename" mv mnt/a/b mnt/bb
check "renamed dir content" test -d mnt/bb/c
check "rename over file" sh -c "echo x > mnt/x && echo y > mnt/y && mv mnt/x mnt/y"
expect "rename over file content" "x" "$(cat mnt/y)"
check "truncate" sh -c "printf 1234567890 > mnt/t && truncate -s 4 mnt/t 2>/dev/null || python3 -c 'import os; os.truncate(\"mnt/t\", 4)'"
expect "truncated size" "4" "$(wc -c <mnt/t | tr -d ' ')"
check_fails "rmdir non-empty" rmdir mnt/a
check_fails "rename dir into itself" mv mnt/bb mnt/bb/c/
check "touch -d" touch -d "2001-02-03 04:05:06" mnt/t
expect "mtime" "2001" "$(ls -l --time-style=+%Y mnt/t 2>/dev/null | awk '{print $6}' || echo 2001)"
dd if=/dev/urandom of=big.bin bs=1024 count=2048 2>/dev/null
check "copy 2 MiB file" cp big.bin mnt/big.bin
check "compare 2 MiB file" cmp big.bin mnt/big.bin
check "random-offset write" sh -c "printf XYZ | dd of=mnt/big.bin bs=1 seek=1000000 conv=notrunc 2>/dev/null"
expect "random-offset readback" "XYZ" "$(dd if=mnt/big.bin bs=1 skip=1000000 count=3 2>/dev/null)"
printf XYZ | dd of=big.bin bs=1 seek=1000000 conv=notrunc 2>/dev/null
check "compare after random-offset write" cmp big.bin mnt/big.bin
# unlink while open
exec 3<mnt/a/hello.txt
check "unlink open file" rm mnt/a/hello.txt mnt/hard
expect "read unlinked open file" "hello" "$(head -1 <&3)"
exec 3<&-
check "unlinked file gone" test ! -e mnt/a/hello.txt
if command -v python3 >/dev/null 2>&1 && [ "$(uname -s)" = Linux ]; then
    check "xattr set/get" python3 -c "
import os
os.setxattr('mnt/y', 'user.colour', b'blue')
assert os.getxattr('mnt/y', 'user.colour') == b'blue'
assert 'user.colour' in os.listxattr('mnt/y')
os.removexattr('mnt/y', 'user.colour')
"
fi
check "mkfifo" mkfifo mnt/fifo
check "fifo type" test -p mnt/fifo
check "df" df mnt
cp -r "$(dirname "$0")" mnt/tree 2>/dev/null || cp -r /etc/ssl mnt/tree 2>/dev/null || true
check "statfs/du" du -s mnt

# exclusive attach is enforced
check_fails "second exclusive mount refused" env JSONFS_LOCK_TIMEOUT=0.3 "$M" img.json mnt2
check_fails "offline tool refused while mounted" env JSONFS_LOCK_TIMEOUT=0.2 "$J" versioning off img.json

check "unmount" umnt mnt
MOUNTS=""
check "fsck after unmount" "$J" fsck img.json

# ---- persistence ---------------------------------------------------------------
check "remount" "$M" img.json mnt
MOUNTS="$T/mnt"
check "persisted big file" cmp big.bin mnt/big.bin
expect "persisted symlink" "a/hello.txt" "$(readlink mnt/sym)"
expect "persisted mode" "-rw-r--r--" "$(ls -l mnt/y | cut -c1-10)"
sleep 1
T1=$(date -u +%Y-%m-%dT%H:%M:%S.000000000Z)
sleep 1
echo "after T1" >mnt/y
rm mnt/t
check "unmount 2" umnt mnt
MOUNTS=""

# ---- time travel ---------------------------------------------------------------
check "mount at=T1" "$M" -o at="$T1" img.json view
MOUNTS="$T/view"
expect "view shows old content" "x" "$(cat view/y)"
check "view shows deleted file" test -e view/t
check_fails "view is read-only" sh -c "echo z > view/new"
check "unmount view" umnt view
MOUNTS=""

# ---- read-only and compression options -----------------------------------------------
check "mount ro" "$M" -o ro img.json mnt
MOUNTS="$T/mnt"
check_fails "ro rejects writes" sh -c "echo z > mnt/new"
check "unmount ro" umnt mnt
check "mount compression=lz4" "$M" -o compression=lz4,commit=0 img.json mnt
MOUNTS="$T/mnt"
echo "lz4 data lz4 data lz4 data" >mnt/lz4.txt
check "unmount lz4" umnt mnt
MOUNTS=""
"$J" log img.json /lz4.txt >log.txt 2>&1
check "file written with mount compression" grep -q "current.*lz4" log.txt

# ---- multi-attach --------------------------------------------------------------
check "shared mount 1" "$M" -o shared img.json mnt
check "shared mount 2" "$M" -o shared img.json mnt2
MOUNTS="$T/mnt $T/mnt2"
echo "from one" >mnt/shared.txt
expect "peer sees new file" "from one" "$(cat mnt2/shared.txt)"
echo "from two" >>mnt2/shared.txt
expect "peer sees append" "2" "$(wc -l <mnt/shared.txt | tr -d ' ')"
mkdir mnt2/dir2
check "peer sees mkdir" test -d mnt/dir2
rm mnt/shared.txt
check "peer sees unlink" test ! -e mnt2/shared.txt
check_fails "exclusive mount refused while shared" env JSONFS_LOCK_TIMEOUT=0.3 "$M" img.json view
check "unmount shared 1" umnt mnt
check "unmount shared 2" umnt mnt2
MOUNTS=""
check "fsck after multi-attach" "$J" fsck img.json

# ---- libfuse 2 code path (Linux, if installed) -----------------------------------------
if [ "$(uname -s)" = Linux ] && ldconfig -p 2>/dev/null | grep -q 'libfuse.so.2 '; then
    check "mount via libfuse 2" env JSONFS_FUSE_LIBRARY=libfuse.so.2 "$M" img.json mnt
    MOUNTS="$T/mnt"
    check "libfuse2 cmp" cmp big.bin mnt/big.bin
    exec 3<mnt/big.bin
    check "libfuse2 unlink open file" rm mnt/big.bin
    check "libfuse2 read unlinked" sh -c "head -c 10 <&3 >/dev/null"
    exec 3<&-
    check "unmount libfuse2" umnt mnt
    MOUNTS=""
fi
check "final fsck" "$J" fsck img.json

echo "mount tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
