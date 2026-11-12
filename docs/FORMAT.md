# JSONfs image format

A JSONfs filesystem image is **one JSON document** (RFC 8259, UTF-8). The
current schema version is **2**; version 1 is still readable and can be
converted with `jsonfs upgrade`.

## Top-level object

```json
{
  "format": "jsonfs",
  "schema_version": 2,
  "uuid": "7e3dfee1-2ed2-4c93-be36-f36e1dd7d20d",
  "label": "backup",
  "created":  "2026-11-11T23:54:10.941596072Z",
  "modified": "2026-11-11T23:54:10.975650961Z",
  "generation": 20,
  "features": ["xattr", "multi-attach", "btime"],
  "default_compression": "deflate",
  "versioning": true,
  "next_inode": 6,
  "root_inode": 1,
  "inodes": { "1": { ... }, "2": { ... } }
}
```

| field | meaning |
|---|---|
| `format` | always `"jsonfs"` |
| `schema_version` | structure version of this document |
| `uuid` | unique filesystem identifier (random UUIDv4 at mkfs time) |
| `label` | free-form volume label |
| `created` / `modified` | RFC 3339 UTC timestamps with nanoseconds: creation (mkfs) and last commit |
| `generation` | (v2) incremented by every commit; used for multi-attach change detection |
| `features` | (v2) informational list of optional features the schema provides |
| `default_compression` | algorithm chosen at mkfs time, used for new data unless a mount overrides it |
| `versioning` | whether time/date based versioning is active |
| `next_inode` | next inode number to allocate (inode numbers are never reused) |
| `root_inode` | inode number of `/` |
| `inodes` | object keyed by decimal inode number |

Unknown top-level keys are preserved verbatim by every tool (e.g.
`snapshot_of` / `snapshot_time` written by `jsonfs snapshot`).

All timestamps are `YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ` (UTC).

## Inode records

Every file, directory, symlink and special file is a JSON object holding all
of its metadata. Directories map names to inode numbers, which is what makes
hard links possible.

```json
"2": {
  "type": "file",
  "mode": "0644",
  "uid": 1000,
  "gid": 1000,
  "nlink": 1,
  "size": 6,
  "atime": "2026-11-11T23:54:10.944411306Z",
  "mtime": "2026-11-11T23:54:10.947518530Z",
  "ctime": "2026-11-11T23:54:10.947518530Z",
  "btime": "2026-11-11T23:54:10.944411306Z",
  "version_time": "2026-11-11T23:54:10.947518530Z",
  "compression": "deflate",
  "sha256": "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03",
  "data": "y0jNyclXyMgvSgEA",
  "xattrs": { "user.colour": "Ymx1ZQ==" },
  "history": [ ... ]
}
```

| field | types | meaning |
|---|---|---|
| `type` | all | `file`, `directory`, `symlink`, `fifo`, `socket`, `chardev`, `blockdev` |
| `mode` | all | permission bits incl. setuid/setgid/sticky, octal string |
| `uid`, `gid` | all | numeric owner |
| `nlink` | all | link count (informational, recomputed on load) |
| `size` | all | content bytes (file), target length (symlink), entry count (directory) |
| `atime` `mtime` `ctime` | all | POSIX timestamps |
| `btime` | all (v2) | birth/creation time |
| `version_time` | all | when this record became the current state (versioning) |
| `compression` | file | algorithm used for `data`; **decompression always uses this field** |
| `sha256` | file | hex SHA-256 of the *uncompressed* content |
| `data` | file | base64 (RFC 4648) of the compressed content |
| `target` | symlink | link target |
| `rdev` | chardev, blockdev | device number |
| `entries` | directory | `{ "name": inode_number }` (`.`/`..` are implicit) |
| `xattrs` | all (v2) | extended attributes, values base64 |
| `history` | all | older versions (see below) |
| `deleted` | marker | `true` when the current record is a delete marker |

### Compression algorithms

| name | format |
|---|---|
| `deflate` (default) | raw DEFLATE stream, RFC 1951 (zlib: `wbits=-15`) |
| `lz4` | LZ4 block format (no frame); the output size is the record's `size` |
| `rle` | PackBits run-length encoding |
| `none` | stored |

Different files (and different versions of one file) may use different
algorithms. A level may be given where an algorithm is selected
(`deflate:9`); the level is never stored.

## Versioning

When `versioning` is `true` nothing is ever really deleted:

* Before an inode is changed for the first time after a commit, its current
  record is appended to `history` (oldest first). Changes made between two
  commits are coalesced into one version.
* Deleting a file/directory replaces the current record with a timestamped
  **delete marker**: `{"type": "file", "deleted": true, "version_time": "..."}`
  (the history stays attached).
* Directory changes are versioned like any other inode, so the directory
  structure at any point in time can be reconstructed.

A history record of a regular file may omit `data` and instead carry
`"data_ref": "newer"`: its content is identical to the closest newer record
(history or current) with the same `sha256` that has `data`. This avoids
duplicating content for metadata-only changes (chmod, utimes, rename, ...).

**State at time T:** for every inode choose the record with the greatest
`version_time <= T` among `history` + current. If there is none, or it is a
delete marker, the inode did not exist at T. Directory entries whose target
did not exist at T are ignored. `jsonfs-mount -o at=T`, `jsonfs ls/cat/export
--at T` and `jsonfs snapshot --at T` all use this rule.

Not versioned: link counts, `ctime`-only and `atime`-only updates.

## Multi-attach

Every image has a companion lock file `IMAGE.lock` with two byte-range locks
at offset 2^30 (+0 *attach*, +1 *transaction*) and the current generation
number as text at the beginning. Byte-range locks work on POSIX (`fcntl`),
Windows (`LockFileEx`) and across NFS/SMB.

* Single-attach mounts and the offline tools hold the *attach* lock
  exclusively; multi-attach (`-o shared`) mounts hold it shared.
* A shared mount takes the *transaction* lock around every operation (shared
  for reads, exclusive for changes), reloads the image if the generation in
  the lock file (or the image file's signature) changed, and commits
  metadata changes before releasing the lock.
* File data written through an open handle is committed when the handle is
  flushed/closed (close-to-open consistency, as with NFS).

## Commits

The whole document is rewritten atomically: written to a temporary file,
`fsync`ed, then renamed over the image (`MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH)`
on Windows). A crash therefore leaves either the old or the new image.
Single-attach mounts commit every `commit=` seconds (default 5), on `fsync`
and at unmount.

## Schema history

* **1** – initial structure.
* **2** – adds `generation` (multi-attach), `features`, `xattrs` and `btime`.
  `jsonfs upgrade` converts 1 → 2 (btime initialised from ctime, also in
  history records).
