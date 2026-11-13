#!/bin/sh
# JSONfs web (http/https) image tests: toolkit access and FUSE mounts.
# usage: tests/http_test.sh /path/to/jsonfs /path/to/jsonfs-mount
# Needs python3 and openssl; FUSE parts are skipped when FUSE is unusable.
set -u
J=${1:-build/native/bin/jsonfs}
M=${2:-build/native/bin/jsonfs-mount}
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/jsonfs-http.XXXXXX")
pass=0
fail=0
PIDS=""
MOUNTED=""
export NO_PROXY="127.0.0.1,localhost${NO_PROXY:+,$NO_PROXY}"
export no_proxy="$NO_PROXY"

ok() { pass=$((pass + 1)); }
bad() { fail=$((fail + 1)); echo "FAIL: $*" >&2; }
check() { d=$1; shift; if "$@" >"$T/out" 2>&1; then ok; else bad "$d"; sed 's/^/    /' "$T/out" >&2; fi; }
check_fails() { d=$1; shift; if "$@" >"$T/out" 2>&1; then bad "$d (unexpected success)"; else ok; fi; }
expect() { if [ "$2" = "$3" ]; then ok; else bad "$1: expected [$2] got [$3]"; fi; }
umnt() { for u in "fusermount3 -u" "fusermount -u" "umount"; do $u "$1" 2>/dev/null && return 0; done; return 1; }
cleanup() {
    for m in $MOUNTED; do umnt "$m"; done
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

for tool in python3 openssl; do
    if ! command -v $tool >/dev/null 2>&1; then
        echo "http tests: skipped ($tool not found)"
        exit 0
    fi
done

start_server() { # name dir [cert key]  -> sets PORT
    rm -f "$T/$1.port"
    python3 "$HERE/http_server.py" "$2" "$T/$1.port" ${3:-} ${4:-} &
    PIDS="$PIDS $!"
    i=0
    while [ ! -s "$T/$1.port" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    PORT=$(cat "$T/$1.port")
}

cd "$T" || exit 1
mkdir www src
printf 'served over the web\n' >src/hello.txt
mkdir src/dir && printf 'nested\n' >src/dir/n.txt
"$J" mkfs -V -L web www/img.json >/dev/null
"$J" import www/img.json src / >/dev/null
printf 'not json' >www/bad.json

# certificates: a private CA, a cert for localhost/127.0.0.1, and one for another host name
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 2 -subj /CN=jsonfs-test-ca \
    >/dev/null 2>&1
mkcert() { # name san
    openssl req -newkey rsa:2048 -nodes -keyout "$1.key" -out "$1.csr" -subj "/CN=$1" >/dev/null 2>&1
    printf 'subjectAltName=%s\nbasicConstraints=CA:FALSE\n' "$2" >"$1.ext"
    openssl x509 -req -in "$1.csr" -CA ca.pem -CAkey ca.key -CAcreateserial -out "$1.pem" -days 2 \
        -extfile "$1.ext" >/dev/null 2>&1
}
mkcert good "DNS:localhost,IP:127.0.0.1"
mkcert wronghost "DNS:other.example"

start_server http www
HTTP=$PORT
start_server https www good.pem good.key
HTTPS=$PORT
start_server wrong www wronghost.pem wronghost.key
WRONG=$PORT

# ---- toolkit over http -----------------------------------------------------------
check "info over http" "$J" info "http://127.0.0.1:$HTTP/img.json"
check "ls over http" "$J" ls -lR "http://127.0.0.1:$HTTP/img.json"
grep -q "n.txt" "$T/out" || bad "ls over http lists nested file"
check "cat over http" "$J" cat "http://127.0.0.1:$HTTP/img.json" /hello.txt
expect "cat over http content" "served over the web" "$(cat "$T/out")"
check "fsck over http" "$J" fsck "http://127.0.0.1:$HTTP/img.json"
check "export over http" "$J" export "http://127.0.0.1:$HTTP/img.json" exported
check "export content" diff -r src exported
check "snapshot from http" "$J" snapshot --at now "http://127.0.0.1:$HTTP/img.json" local.json
check "log over http" "$J" log "http://127.0.0.1:$HTTP/img.json" /hello.txt
check_fails "http redirect is not followed" "$J" cat "http://127.0.0.1:$HTTP/redirect/img.json" /hello.txt
grep -q "redirect" "$T/out" || bad "redirect error mentions the redirect"
check_fails "404" "$J" info "http://127.0.0.1:$HTTP/missing.json"
check_fails "invalid JSON" "$J" info "http://127.0.0.1:$HTTP/bad.json"
check_fails "connection refused" "$J" info "http://127.0.0.1:1/img.json"
check_fails "remote images are read-only (versioning)" "$J" versioning off "http://127.0.0.1:$HTTP/img.json"
check_fails "remote images are read-only (recompress)" "$J" recompress -c lz4 "http://127.0.0.1:$HTTP/img.json"
check_fails "remote images are read-only (import)" "$J" import "http://127.0.0.1:$HTTP/img.json" src /x
check_fails "remote images are read-only (fsck -r)" "$J" fsck -r "http://127.0.0.1:$HTTP/img.json"
check_fails "mkfs at a URL" "$J" mkfs "http://127.0.0.1:$HTTP/new.json"

# ---- toolkit over https ---------------------------------------------------------
check_fails "https: untrusted certificate rejected by default" "$J" info "https://127.0.0.1:$HTTPS/img.json"
grep -qi "certificate" "$T/out" || bad "certificate error is explained"
check "https: --insecure" "$J" info --insecure "https://127.0.0.1:$HTTPS/img.json"
check "https: -k" "$J" cat -k "https://localhost:$HTTPS/img.json" /hello.txt
check "https: --cacert trusts the private CA" "$J" cat --cacert ca.pem "https://localhost:$HTTPS/img.json" /hello.txt
expect "https content" "served over the web" "$(cat "$T/out")"
check_fails "https: host name mismatch rejected even with the CA" "$J" info --cacert ca.pem "https://localhost:$WRONG/img.json"
check "https: host name mismatch accepted with --insecure" "$J" info -k "https://localhost:$WRONG/img.json"
check "https: redirect to https is followed" "$J" cat --cacert ca.pem "https://localhost:$HTTPS/redirect/img.json" /hello.txt
check_fails "https: redirect to http is refused" "$J" cat --cacert ca.pem "https://localhost:$HTTPS/redirect-http/$HTTP/img.json" /hello.txt

# ---- FUSE mounts ---------------------------------------------------------------
fuse_ok=1
[ -e /dev/fuse ] || [ "$(uname -s)" != Linux ] || fuse_ok=0
if [ $fuse_ok = 1 ]; then
    mkdir mnt
    if "$M" "http://127.0.0.1:$HTTP/img.json" mnt 2>err.txt; then
        MOUNTED="$T/mnt"
        ok
        expect "mounted http content" "served over the web" "$(cat mnt/hello.txt)"
        expect "mounted nested content" "nested" "$(cat mnt/dir/n.txt)"
        check_fails "http mount is read-only" sh -c "echo x > mnt/new.txt"
        check "unmount http" umnt mnt
        MOUNTED=""
        check_fails "https mount rejects an untrusted certificate" "$M" "https://127.0.0.1:$HTTPS/img.json" mnt
        check "https mount with --insecure" "$M" --insecure "https://127.0.0.1:$HTTPS/img.json" mnt
        MOUNTED="$T/mnt"
        expect "mounted https content" "nested" "$(cat mnt/dir/n.txt)"
        check "unmount https" umnt mnt
        check "https mount with -o cafile" "$M" -o cafile="$T/ca.pem" "https://localhost:$HTTPS/img.json" mnt
        expect "mounted https (cafile) content" "served over the web" "$(cat mnt/hello.txt)"
        check "unmount https 2" umnt mnt
        MOUNTED=""
        check_fails "http mount does not follow redirects" "$M" "http://127.0.0.1:$HTTP/redirect/img.json" mnt
        check_fails "rw is refused for web images" "$M" -o rw "http://127.0.0.1:$HTTP/img.json" mnt
        check_fails "shared is refused for web images" "$M" -o shared "http://127.0.0.1:$HTTP/img.json" mnt
        check "mount at= over http" "$M" -o at=now "http://127.0.0.1:$HTTP/img.json" mnt
        MOUNTED="$T/mnt"
        check "at= view content" test -f mnt/dir/n.txt
        check "unmount at= view" umnt mnt
        MOUNTED=""
    elif grep -qi "FUSE is not available" err.txt; then
        echo "http tests: FUSE part skipped ($(head -1 err.txt))"
    else
        bad "http mount failed: $(cat err.txt)"
    fi
fi

echo "http tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
