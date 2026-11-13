#!/bin/sh
# JSONfs toolkit end-to-end tests.
# usage: tests/toolkit_test.sh /path/to/jsonfs
set -u
J=${1:-build/native/bin/jsonfs}
T=$(mktemp -d "${TMPDIR:-/tmp}/jsonfs-tk.XXXXXX")
trap 'rm -rf "$T"' EXIT INT TERM
pass=0
fail=0

ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }
run() { # description, command...
    d=$1
    shift
    if "$@" >"$T/out" 2>&1; then ok; else bad "$d (exit $?)"; sed 's/^/    /' "$T/out" >&2; fi
}
fails() { # description, command... (expects non-zero)
    d=$1
    shift
    if "$@" >"$T/out" 2>&1; then bad "$d (unexpected success)"; else ok; fi
}
expect() { # description, expected, actual
    if [ "$2" = "$3" ]; then ok; else bad "$1: expected [$2] got [$3]"; fi
}

cd "$T" || exit 1
mkdir -p src/sub/deep
printf 'hello\n' >src/hello.txt
i=0
while [ $i -lt 2000 ]; do echo "line $i of a fairly compressible file"; i=$((i + 1)); done >src/sub/text.txt
dd if=/dev/urandom of=src/sub/deep/random.bin bs=1024 count=300 2>/dev/null
ln -s hello.txt src/link
: >src/empty
chmod 0751 src/sub

# ---- mkfs / info -----------------------------------------------------------
run "mkfs" "$J" mkfs -L tk -V img.json
fails "mkfs refuses to overwrite" "$J" mkfs img.json
run "mkfs -f overwrites" "$J" mkfs -f -L tk -V img.json
fails "mkfs rejects unknown compression" "$J" mkfs -c zzz other.json
run "mkfs.jsonfs alias" sh -c "ln -s '$J' mkfs.jsonfs && ./mkfs.jsonfs -c rle alias.json"
run "info" "$J" info img.json
grep -q "versioning:          on" out || bad "info shows versioning"
run "info --json" "$J" info --json img.json
grep -q '"schema_version":2' out || bad "info --json schema"

# ---- import / ls / cat / export ---------------------------------------------
run "import" "$J" import img.json src /
run "ls -lR" "$J" ls -lR img.json /
grep -q "random.bin" out || bad "ls lists nested file"
grep -q "link -> hello.txt" out || bad "ls shows symlink target"
grep -q "drwxr-x--x" out || bad "ls keeps directory mode"
run "cat" "$J" cat img.json /hello.txt
expect "cat content" "hello" "$(cat out)"
run "export" "$J" export img.json exported
run "export matches source" diff -r src exported
expect "export keeps symlink" "hello.txt" "$(readlink exported/link)"
fails "cat missing file" "$J" cat img.json /nope

# ---- fsck --------------------------------------------------------------------
run "fsck clean" "$J" fsck img.json
grep -q "0 error(s), 0 warning(s)" out || bad "fsck clean summary"
cp img.json broken.json
# corrupt one base64 payload character (keeps JSON valid)
python3 - broken.json <<'EOF' 2>/dev/null || sed -i.bak 's/"data": "\([A-Za-z0-9+\/]\{8\}\)\([A-Za-z0-9+\/]\)/"data": "\1A/' broken.json
import json, sys
p = sys.argv[1]
d = json.load(open(p))
for k, v in d["inodes"].items():
    if v.get("type") == "file" and v.get("size", 0) > 1000:
        s = v["data"]
        v["data"] = s[:20] + ("A" if s[20] != "A" else "B") + s[21:]
        break
json.dump(d, open(p, "w"), indent=1)
EOF
"$J" fsck broken.json >out 2>&1
rc=$?
expect "fsck exit code on corruption" 4 "$rc"
grep -q "ERROR" out || bad "fsck reports corruption"
run "fsck.jsonfs alias" sh -c "ln -s '$J' fsck.jsonfs && ./fsck.jsonfs img.json"

# ---- versioning, log, snapshot -----------------------------------------------
sleep 1
T1=$(date -u +%Y-%m-%dT%H:%M:%SZ)
sleep 1
printf 'changed\n' >src/hello.txt
run "re-import" "$J" import img.json src/hello.txt /
run "log" "$J" log img.json /hello.txt
[ "$(grep -c file out)" -ge 2 ] || bad "log lists >= 2 versions"
run "cat --at" "$J" cat --at "$T1" img.json /hello.txt
expect "old version via --at" "hello" "$(cat out)"
run "cat current" "$J" cat img.json /hello.txt
expect "current version" "changed" "$(cat out)"
run "snapshot" "$J" snapshot --at "$T1" img.json snap.json
run "snapshot content" "$J" cat snap.json /hello.txt
expect "snapshot content" "hello" "$(cat out)"
run "snapshot info" "$J" info snap.json
grep -q "versioning:          off" out || bad "snapshot defaults to versioning off"
grep -q "history records:     0" out || bad "snapshot has no history"
fails "snapshot refuses overwrite" "$J" snapshot --at "$T1" img.json snap.json
run "fsck snapshot" "$J" fsck snap.json

# ---- versioning toggle ---------------------------------------------------------
run "versioning status" "$J" versioning status img.json
run "versioning off --purge" "$J" versioning off --purge img.json
run "versioning status off" "$J" versioning status img.json
grep -q "versioning is off (0 history records, 0 delete markers)" out || bad "purged history"
run "versioning on" "$J" versioning on img.json

# ---- recompress --------------------------------------------------------------
for algo in lz4 rle none deflate:9 deflate; do
    run "recompress $algo" "$J" recompress -c "$algo" --set-default --history img.json
    run "fsck after recompress $algo" "$J" fsck img.json
    grep -q "0 error(s)" out || bad "fsck after recompress $algo"
done
run "export after recompress" "$J" export img.json exported2
run "content survives recompression" diff -r src exported2
fails "recompress unknown algorithm" "$J" recompress -c brotli img.json

# ---- upgrade -------------------------------------------------------------------
run "mkfs schema 1" "$J" mkfs --schema 1 old.json
run "import into v1" "$J" import old.json src /
run "fsck v1 warns" "$J" fsck old.json
grep -q "outdated" out || bad "fsck warns about old schema"
run "upgrade" "$J" upgrade old.json
grep -q "from schema 1 to 2" out || bad "upgrade message"
run "upgrade is idempotent" "$J" upgrade old.json
grep -q "already at schema 2" out || bad "already upgraded"
run "fsck upgraded" "$J" fsck old.json
grep -q "0 error(s), 0 warning(s)" out || bad "upgraded image is clean"

# ---- prune ---------------------------------------------------------------------
run "prune" "$J" prune --before now img.json

# ---- pack / unpack / add / mv / get / rm / mkdir ------------------------------------
run "pack" "$J" pack -c lz4 -L packed src packed.json
fails "pack refuses to overwrite" "$J" pack src packed.json
fails "pack needs a folder" "$J" pack src/hello.txt p2.json
run "unpack" "$J" unpack packed.json unpacked
run "pack/unpack round trip" diff -r src unpacked
expect "unpack keeps symlinks" "hello.txt" "$(readlink unpacked/link)"
fails "unpack refuses a non-empty folder" "$J" unpack packed.json unpacked
run "unpack -f" "$J" unpack -f packed.json unpacked
printf 'added\n' >added.txt
run "add file" "$J" add packed.json added.txt /
fails "add refuses to overwrite" "$J" add packed.json added.txt /added.txt
grep -q "image left unchanged" out || bad "failed add leaves the image unchanged"
printf 'replaced\n' >added.txt
run "add -f replaces" "$J" add -f packed.json added.txt /added.txt
run "cat added" "$J" cat packed.json /added.txt
expect "replaced content" "replaced" "$(cat out)"
run "add -p into new folders" "$J" add -p packed.json added.txt src/sub /new/deep/
run "added tree" "$J" ls -R packed.json /new/deep
grep -q "random.bin" out || bad "add copies folders recursively"
run "mv rename" "$J" mv packed.json /added.txt /renamed.txt
run "mv into folder" "$J" mv packed.json /renamed.txt /hello.txt /new
run "ls after mv" "$J" ls packed.json /new
grep -q "renamed.txt" out || bad "mv moved file into folder"
fails "mv -n refuses to clobber" "$J" mv -n packed.json /new/renamed.txt /new/hello.txt
fails "mv missing source" "$J" mv packed.json /nope /x
run "get file" "$J" get packed.json /new/renamed.txt got.txt
expect "get content" "replaced" "$(cat got.txt)"
fails "get refuses to overwrite" "$J" get packed.json /new/renamed.txt got.txt
run "get folder" "$J" get packed.json /new/deep gotdir
run "get folder content" diff -r src/sub gotdir/sub
fails "rm folder without -r" "$J" rm packed.json /new
run "rm -r" "$J" rm -r packed.json /new
fails "rm missing" "$J" rm packed.json /new
run "rm -f missing" "$J" rm -f packed.json /new
fails "rm root" "$J" rm -r packed.json /
run "mkdir -p" "$J" mkdir -p -m 0700 packed.json /m/n/o
run "ls mkdir" "$J" ls -l packed.json /m/n
grep -q "drwx------" out || bad "mkdir -m sets the mode"
run "fsck after edits" "$J" fsck packed.json
grep -q "0 error(s), 0 warning(s)" out || bad "edited image is clean"

# ---- compressors / version ---------------------------------------------------------
run "compressors" "$J" compressors
run "version" "$J" version

echo "toolkit tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
