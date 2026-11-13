# JSONfs

A POSIX filesystem stored in a single JSON document. The JSON object *is*
the filesystem image: it can be mounted on Linux, macOS, FreeBSD and every
Windows release since Windows XP, inspected with any JSON tool, diffed,
mailed around or checked into git.

```sh
jsonfs mkfs -V -L notes notes.json        # create (versioning on)
jsonfs-mount notes.json /mnt/notes        # mount
echo hi > /mnt/notes/hello.txt
fusermount3 -u /mnt/notes
jsonfs ls -l notes.json                   # or read it without mounting
```

## Features

* **POSIX semantics** – regular files, directories, hard links, symlinks,
  FIFOs, sockets, device nodes, permissions, ownership, setuid/setgid/sticky,
  nanosecond timestamps (atime/mtime/ctime/birth time), extended attributes,
  rename (incl. `RENAME_NOREPLACE`/`RENAME_EXCHANGE`), unlink-while-open,
  sparse writes, `statfs`.
* **One JSON object per file** carrying all its details – type, mode, owner,
  timestamps, size, the **SHA-256 of the original content**, the
  **compression algorithm** and the compressed content as **base64**
  ([format](docs/FORMAT.md)).
* **Per-file compression**: `deflate` (default), `lz4`, `rle`, `none`. The
  default is chosen at `mkfs` time, can be overridden for new data with
  `-o compression=...`, and decompression always follows the algorithm
  recorded in each file.
* **Image header** with a UUID, schema version, creation and last-modified
  timestamps.
* **Time/date based versioning** – nothing is deleted for real: changes push
  the previous record to the file's history and deletes leave timestamped
  delete markers. `-o at=2026-11-11T10:00:00Z` mounts a read-only view of
  any past moment.
* **Multi-attach** (`-o shared`): several mounts – on one host or several
  hosts through a network share – can use the same image concurrently,
  coordinated by byte-range locks in a lock file (works with `fcntl` on POSIX
  and `LockFileEx` on Windows, also across NFS/SMB), with close-to-open
  consistency.
* **Mount over the web**: `jsonfs-mount https://pupu.me/fs/pupu-fs.json folder`
  mounts a published image read-only. Certificates are verified by default
  (`--insecure` to skip, `--cacert` for private CAs); plain `http://` works
  too and never follows redirects.
* **Live web mounts**: remote updates are picked up automatically
  (eventually consistent, or `refresh=strict` to check on every open and
  directory listing) using `304`/ETag revalidation or 4 KiB header probes -
  the full image is only fetched when it changed; the last good version stays
  mounted when the server fails; optional on-disk cache for offline mounts.
* **File system events** (inotify, kqueue/FSEvents, ReadDirectoryChangesW)
  for local changes *and* for remote updates of web mounts.
* **Archive conversion**: `jsonfs convert photos.zip photos.json` and back;
  zip, tar and tar.gz built in, 7z / tar.xz / tar.bz2 / tar.zst / cpio / iso
  / rar (read) … through libarchive when installed.
* **Folder ↔ image tools**: `jsonfs pack` / `unpack`, plus `add`, `mv`, `get`,
  `rm`, `mkdir` to edit images without mounting.
* **Crash safety** – commits are atomic (write temp file, fsync, rename).
* **Offline maintenance toolkit** (`jsonfs`): `fsck` (verifies every
  checksum, including history; repairs structure), `snapshot` (extract the
  state at a given time into a new image), `versioning on|off`,
  `recompress`, `upgrade` (schema 1 → 2), `prune`, `log`, `ls`, `cat`,
  `import`, `export`, `info`.
* **Drivers** for Linux & FreeBSD (libfuse 2/3), macOS (macFUSE or FUSE-T),
  Windows 7–11 (WinFsp) and Windows XP/Vista (Dokan 0.6). FUSE/WinFsp/Dokan
  are loaded at runtime, so one binary per platform works with whichever is
  installed.
* **Installers with uninstallers**: self-extracting `.run` for
  Linux/macOS/FreeBSD, NSIS `setup.exe` for Windows.
* No dependencies: plain C99, cross-compiled for 13 targets from Linux.

## Documentation

* [docs/USAGE.md](docs/USAGE.md) – mounting, options, versioning, multi-attach, maintenance, installers
* [docs/FORMAT.md](docs/FORMAT.md) – the JSON image format
* [docs/BUILDING.md](docs/BUILDING.md) – native and cross builds, release pipeline

## Quick build

```sh
make && make test
scripts/fetch-deps.sh all && scripts/build-release.sh all   # every platform + installers
```

## License

MIT (see [LICENSE](LICENSE)). The Windows driver uses, when installed,
WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos
(https://github.com/winfsp/winfsp), or Dokan (https://github.com/dokan-dev/dokany).
