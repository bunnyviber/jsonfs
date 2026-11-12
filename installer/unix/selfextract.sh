#!/bin/sh
# JSONfs self-extracting installer (@VERSION@, @TARGET@).
#
#   sh jsonfs-@VERSION@-@TARGET@.run [install] [--prefix DIR] [--yes]
#   sh jsonfs-@VERSION@-@TARGET@.run uninstall [--prefix DIR]
#   sh jsonfs-@VERSION@-@TARGET@.run status
#   sh jsonfs-@VERSION@-@TARGET@.run --extract [DIR]      (unpack only)
set -e
T=$(mktemp -d "${TMPDIR:-/tmp}/jsonfs-install.XXXXXX")
trap 'rm -rf "$T"' EXIT INT TERM
SKIP=$(awk '/^__JSONFS_PAYLOAD_BELOW__$/ { print NR + 1; exit 0 }' "$0")
tail -n +"$SKIP" "$0" | gzip -dc | (cd "$T" && tar xf -)
if [ "${1:-}" = "--extract" ]; then
    DEST=${2:-jsonfs-@VERSION@-@TARGET@}
    mkdir -p "$DEST"
    cp -R "$T"/. "$DEST"/
    echo "extracted into $DEST"
    exit 0
fi
sh "$T/install.sh" "$@"
exit $?
__JSONFS_PAYLOAD_BELOW__
