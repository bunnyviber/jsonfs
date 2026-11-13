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

start_server() { # name dir [cert key] [--no-validators] -> sets PORT
    rm -f "$T/$1.port"
    python3 "$HERE/http_server.py" "$2" "$T/$1.port" ${3:-} ${4:-} ${5:-} &
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
        # ---- refresh of web mounts ------------------------------------------------
        mkdir -p r/www r/src r/mnt
        printf 'one\n' >r/src/a.txt
        mkdir r/src/d && printf 'x\n' >r/src/d/x.txt && printf 'gone soon\n' >r/src/del.txt
        "$J" pack r/src r/www/img.json >/dev/null
        start_server r1 r/www
        R1=$PORT
        URL="http://127.0.0.1:$R1/img.json"
        check "mount with periodic refresh" "$M" -o refresh_interval=0.5 "$URL" r/mnt
        MOUNTED="$T/r/mnt"
        linux_events=0
        if [ "$(uname -s)" = Linux ]; then
            linux_events=1
            python3 "$HERE/inotify_watch.py" ev.txt 7 r/mnt r/mnt/d &
            i=0
            while [ ! -f ev.txt.ready ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
        fi
        printf 'two\n' >two.txt
        "$J" add -f r/www/img.json two.txt /a.txt >/dev/null
        "$J" add r/www/img.json two.txt /d/new.txt >/dev/null
        "$J" mv r/www/img.json /d/x.txt /moved.txt >/dev/null
        "$J" rm r/www/img.json /del.txt >/dev/null
        "$J" mkdir r/www/img.json /newdir >/dev/null
        "$J" add r/www/img.json two.txt /newdir/n.txt >/dev/null
        i=0
        while [ $i -lt 60 ] && [ ! -e r/mnt/newdir/n.txt ]; do sleep 0.1; i=$((i + 1)); done
        # metadata-only change (same content, new mtime) -> ATTRIB
        sleep 0.3
        touch -d "2020-01-01 00:00:00" two.txt
        "$J" add -f r/www/img.json two.txt /d/new.txt >/dev/null
        sleep 1.5
        expect "periodic refresh: modified content" "two" "$(cat r/mnt/a.txt)"
        check "periodic refresh: new file" test -f r/mnt/d/new.txt
        check "periodic refresh: rename" test -f r/mnt/moved.txt
        check "periodic refresh: delete" test ! -e r/mnt/del.txt
        check "periodic refresh: new directory" test -f r/mnt/newdir/n.txt
        if [ $linux_events = 1 ]; then
            sleep 5.5
            check "inotify: CREATE" grep -q "CREATE r/mnt/d/new.txt" ev.txt
            check "inotify: MODIFY" grep -q "MODIFY r/mnt/a.txt" ev.txt
            check "inotify: MOVED_FROM" grep -q "MOVED_FROM r/mnt/d/x.txt" ev.txt
            check "inotify: MOVED_TO" grep -q "MOVED_TO r/mnt/moved.txt" ev.txt
            check "inotify: DELETE" grep -q "DELETE r/mnt/del.txt" ev.txt
            check "inotify: mkdir" grep -q "CREATE r/mnt/newdir" ev.txt
            check "inotify: ATTRIB (metadata-only change)" grep -q "ATTRIB r/mnt/d/new.txt" ev.txt
            [ $fail -eq 0 ] || sed 's/^/    event: /' ev.txt >&2
        fi
        # unchanged image: only 304s, no bodies
        : >"$T/r1.port.log"
        sleep 2
        n304=$(grep -c " 304 0$" "$T/r1.port.log")
        nother=$(grep -vc " 304 0$" "$T/r1.port.log")
        [ "$n304" -ge 2 ] && ok || bad "unchanged image is revalidated with 304 (got $n304)"
        expect "no full downloads while unchanged" "0" "$nother"
        # image removed / failing: keep serving the last version
        mv r/www/img.json r/www/img.json.bak
        sleep 1.5
        expect "removed image: last version kept" "two" "$(cat r/mnt/a.txt)"
        check "removed image: tree kept" test -f r/mnt/newdir/n.txt
        mv r/www/img.json.bak r/www/img.json
        printf 'back\n' >back.txt
        "$J" add r/www/img.json back.txt / >/dev/null
        i=0
        while [ $i -lt 40 ] && [ ! -e r/mnt/back.txt ]; do sleep 0.1; i=$((i + 1)); done
        check "updates resume after the image is back" test -e r/mnt/back.txt
        check_fails "web mount is still read-only" sh -c "echo x > r/mnt/w.txt"
        check "unmount refreshing mount" umnt r/mnt
        MOUNTED=""

        # strict mode: every open / listing sees the newest version immediately
        check "mount with strict refresh" "$M" -o refresh=strict,refresh_interval=3600 "$URL" r/mnt
        MOUNTED="$T/r/mnt"
        printf 'strict\n' >strict.txt
        "$J" add -f r/www/img.json strict.txt /a.txt >/dev/null
        expect "strict: open sees the new content at once" "strict" "$(cat r/mnt/a.txt)"
        "$J" add r/www/img.json strict.txt /strict.txt >/dev/null
        check "strict: listing sees the new file at once" sh -c "ls r/mnt | grep -q strict.txt"
        check "unmount strict" umnt r/mnt
        MOUNTED=""

        # servers without ETag/Last-Modified: 4 KiB header probes
        mkdir -p r2/www && cp r/www/img.json r2/www/img.json
        start_server r2 r2/www --no-validators
        R2=$PORT
        check "mount (server without validators)" "$M" -o refresh_interval=0.5 "http://127.0.0.1:$R2/img.json" r/mnt
        MOUNTED="$T/r/mnt"
        sleep 1.5
        nprobe=$(grep -c " 206 " "$T/r2.port.log")
        big=$(awk '$3 == 206 && $4 > 4096' "$T/r2.port.log" | wc -l | tr -d ' ')
        [ "$nprobe" -ge 2 ] && ok || bad "header probes used without validators (got $nprobe)"
        expect "probes are at most 4 KiB" "0" "$big"
        printf 'probe\n' >probe.txt
        "$J" add r2/www/img.json probe.txt / >/dev/null
        i=0
        while [ $i -lt 40 ] && [ ! -e r/mnt/probe.txt ]; do sleep 0.1; i=$((i + 1)); done
        check "probe mode picks up changes" test -f r/mnt/probe.txt
        check "unmount probe mode" umnt r/mnt
        MOUNTED=""

        # on-disk cache: revalidated with a conditional request, used when offline
        check "mount with cache" "$M" -o cache="$T/cache.json",refresh=off "$URL" r/mnt
        MOUNTED="$T/r/mnt"
        check "cache file written" test -s "$T/cache.json"
        check "unmount cache mount" umnt r/mnt
        MOUNTED=""
        : >"$T/r1.port.log"
        check "remount with cache" "$M" -o cache="$T/cache.json",refresh=off "$URL" r/mnt
        MOUNTED="$T/r/mnt"
        expect "remount revalidates the cache with a 304" "304" "$(awk 'NR==1{print $3}' "$T/r1.port.log")"
        expect "remount from cache content" "strict" "$(cat r/mnt/a.txt)"
        check "unmount cache mount 2" umnt r/mnt
        MOUNTED=""
        mv r/www/img.json r/www/gone.json
        check "mount from cache while the image is gone" "$M" -o cache="$T/cache.json" "$URL" r/mnt
        MOUNTED="$T/r/mnt"
        expect "offline content from cache" "strict" "$(cat r/mnt/a.txt)"
        check "unmount offline mount" umnt r/mnt
        MOUNTED=""
        check_fails "without a cache the mount fails" "$M" "$URL" r/mnt
    elif grep -qi "FUSE is not available" err.txt; then
        echo "http tests: FUSE part skipped ($(head -1 err.txt))"
    else
        bad "http mount failed: $(cat err.txt)"
    fi
fi

echo "http tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
