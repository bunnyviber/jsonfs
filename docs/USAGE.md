# Using JSONfs

## Create and mount

```sh
jsonfs mkfs -L data my.json                 # default compression: deflate
jsonfs mkfs -c lz4 -V versioned.json        # lz4 by default, versioning on
mkdir /mnt/json
jsonfs-mount my.json /mnt/json              # Linux/FreeBSD/macOS
...
fusermount3 -u /mnt/json                    # Linux (or: umount /mnt/json)
```

Windows (cmd.exe, as a user allowed to mount):

```bat
jsonfs mkfs C:\data\my.json
jsonfs-mount C:\data\my.json X:
rem Ctrl+C unmounts
```

### Mounting over the web (http / https)

```sh
jsonfs-mount https://pupu.me/fs/pupu-fs.json folder        # verified TLS
jsonfs-mount --insecure https://test.lan/fs/img.json folder # ignore certificate errors
jsonfs-mount --cacert my-ca.pem https://test.lan/img.json folder   # trust a private CA
jsonfs-mount http://intranet/fs/img.json folder            # plain HTTP
```

* The image is mounted **read-only** (`-o rw` and
  `-o shared` are refused). `-o at=TIME` works for versioned images.
* **Certificates are verified by default** (chain of trust and host name,
  using the system's CA store). `-k` / `--insecure` (or `-o insecure`)
  disables verification; `--cacert FILE` (or `-o cafile=FILE`) additionally
  trusts the CAs in a PEM file (Linux/macOS/FreeBSD; on Windows import the CA
  into the certificate store instead).
* **Redirects:** `http://` URLs never follow redirects (a 3xx answer is an
  error that names the target, so you can use the final URL directly).
  `https://` URLs follow up to 5 redirects, but only to other `https://`
  URLs - a redirect can never downgrade the connection to plain HTTP.
* Proxies are honoured (`https_proxy`/`http_proxy`/`no_proxy` on POSIX,
  the WinHTTP proxy settings on Windows). `https://user:pass@host/...` sends
  basic credentials; they are masked in messages and in the mount table.
* Transport: libcurl (loaded at runtime; present on macOS, usually on Linux,
  `pkg install curl` on FreeBSD) and WinHTTP on Windows. Windows XP's WinHTTP
  only speaks TLS 1.0, which most servers no longer accept - use plain HTTP
  or a newer Windows there.

### Keeping web mounts up to date

The server is checked for a newer version of the image, and a new version is
switched to atomically while the image stays mounted:

| mode | behaviour |
|---|---|
| `-o refresh=periodic` (default) | eventually consistent: a background check every `refresh_interval` seconds (default 60) |
| `-o refresh=strict` | before a file is opened or a directory is listed, the server is checked and a newer version applied first (plus the background checks); `refresh_min=S` lets checks younger than S seconds count |
| `-o refresh=off` | never check again after mounting |

Checks are designed to be light on the network:

* when the server sends an `ETag` (or `Last-Modified`), every check is a
  conditional request: an unchanged image costs one empty `304` response;
* servers without validators get a 4 KiB `Range` request of the image header
  (uuid + generation + modification time identify a version exactly);
* the full image is only downloaded when it has really changed (compressed
  with gzip when the server supports it);
* `-o cache=PATH` (or `cache=auto` for `~/.cache/jsonfs/…`) keeps the latest
  copy on disk: remounting is a conditional request (`304` = use the cache),
  and if the server is unreachable or the image was removed the image is
  mounted from the cached copy.

If a check fails - network error, `404`, invalid JSON - the version you have
stays mounted unchanged; the error is logged once (syslog when running in
the background) and updates resume as soon as the image is available again.

### File system events

Watching works with the usual tools (`inotifywait`, `watchdog`, IDEs, file
managers):

* changes made through the mount produce events natively (inotify on Linux,
  kqueue/FSEvents on macOS and FreeBSD, `ReadDirectoryChangesW` on Windows);
* **remote changes of web images produce events too**: when a new version
  arrives, the driver computes the differences (creations, deletions,
  renames, content and metadata changes) and replays them through the kernel
  on its own mount point, so watchers receive genuine `IN_CREATE`,
  `IN_DELETE`, `IN_MOVED_FROM/TO`, `IN_MODIFY`, `IN_ATTRIB` … events. With
  WinFsp the changes are reported with WinFsp's notification API. Dokan 0.6
  (Windows XP/Vista) has no such API, so there web images refresh without
  events. `-o events=off` disables the replay (the mount is then also
  read-only at the kernel level).

To let the replayed calls through, a web mount with events is read-write at
the kernel level while the volume itself refuses every write from other
processes with `EROFS`; it is private to the mounting user unless
`allow_other` is given (then permissions are checked by the kernel as usual).

The toolkit's read-only commands accept URLs too, with the same flags:
`jsonfs info|fsck|ls|cat|export|log URL`, `jsonfs snapshot --at T URL out.json`.

`jsonfs-mount` runs in the background on POSIX systems (use `-f` to stay in
the foreground, `-d` for debug output) and in the foreground on Windows.

### Mount options (`-o opt[,opt...]`)

| option | meaning |
|---|---|
| `ro` | read-only |
| `compression=ALGO[:LEVEL]` | algorithm for data written during this mount (`deflate`, `lz4`, `rle`, `none`) |
| `at=TIME` | read-only view of the image as it was at TIME (versioned images) |
| `shared` | multi-attach; every mount of the image must use it |
| `commit=SECONDS` | single-attach commit interval (default 5; `0` = after every change) |
| `insecure` / `cafile=FILE` | web images: skip certificate verification / trust extra CAs |
| `refresh=periodic\|strict\|off` | web images: how updates are picked up (see above) |
| `refresh_interval=S` / `refresh_min=S` | web images: background check interval (60) / strict-mode coalescing (0) |
| `cache=PATH\|auto` | web images: on-disk copy for cheap remounts and offline use |
| `events=on\|off` | web images: report remote changes as file system events (default on) |
| `backend=winfsp\|dokan` | Windows: force a driver |
| `volname=NAME` | Windows/macOS volume name |
| anything else | passed to FUSE (`allow_other`, `uid=`, `gid=`, ...) |

TIME accepts `2026-11-11T10:00:00Z`, `2026-11-11T11:00:00+01:00`,
`2026-11-11`, `@1762855200` and relative values such as `-2h`, `-3d`, `-1w`.

### fstab (Linux)

The installer creates `/sbin/mount.jsonfs`, so:

```
/srv/images/my.json  /mnt/json  jsonfs  defaults,compression=lz4  0 0
```

and `mount -t jsonfs /srv/images/my.json /mnt/json` work.

## Versioning

```sh
jsonfs versioning on my.json                 # start keeping history
jsonfs log my.json /notes.txt                # versions of a file
jsonfs cat --at 2026-11-11T09:00:00Z my.json /notes.txt
jsonfs-mount -o at=-2h my.json /mnt/past     # browse the state of 2 hours ago
jsonfs snapshot --at 2026-11-01 my.json nov1.json   # extract into a new image
jsonfs prune --before -30d my.json           # forget history older than 30 days
jsonfs versioning off --purge my.json        # stop and drop all history
```

## Multi-attach

```sh
jsonfs-mount -o shared /net/share/team.json /mnt/a     # host A
jsonfs-mount -o shared /net/share/team.json /mnt/b     # host B (or the same host)
```

Metadata changes are visible to the other attachments immediately; file
contents after the writer closes (or fsyncs) the file. The image must live
on storage whose byte-range locks are honoured by all hosts (local disks,
NFSv4, SMB). Multi-attach requires schema 2 (`jsonfs upgrade`).

## Maintenance (image must not be mounted)

| command | purpose |
|---|---|
| `jsonfs info IMAGE` | metadata and statistics (`--json` for scripts) |
| `jsonfs fsck [-v] [-r] IMAGE` | verify every checksum (also of history) and the structure; `-r` repairs link counts, dangling entries and moves orphans to `/lost+found`. Exit 0 = clean, 1 = repaired, 4 = errors left |
| `jsonfs recompress -c ALGO [--set-default] [--history] IMAGE` | re-encode all files |
| `jsonfs upgrade IMAGE` | convert to the newest schema |
| `jsonfs versioning on\|off\|status [--purge] IMAGE` | toggle versioning |
| `jsonfs snapshot --at TIME IMAGE OUT` | point-in-time extraction |
| `jsonfs prune --before TIME IMAGE` | drop old history |
| `jsonfs ls/cat/import/export` | access content without mounting |

## Archives (zip, tar, 7z, ...)

```sh
jsonfs convert photos.zip photos.json        # archive -> new image (format from the contents)
jsonfs convert photos.json photos.tar.gz     # image -> archive (format from the extension)
jsonfs from-archive -c lz4 -V src.tar.xz src.json     # with mkfs options
jsonfs from-archive --into /vendor lib.zip project.json   # into a folder of an existing image
jsonfs to-archive --at -2d project.json yesterday.7z     # a past state of a versioned image
jsonfs to-archive project.json docs.zip /docs            # only a sub-folder
jsonfs archive-formats                       # what is available here
```

| format | extensions | how |
|---|---|---|
| tar (ustar, pax, GNU, v7) | `.tar` | built in, read + write |
| gzip compressed tar | `.tar.gz` `.tgz` | built in, read + write |
| zip (stored / DEFLATE, zip64) | `.zip` `.jar` | built in, read + write |
| 7z, tar.xz, tar.bz2, tar.zst, tar.lz4, tar.lzma, tar.lz, cpio, iso, xar, ar, shar, warc | | libarchive, read + write |
| rar, rar5, cab, lha and anything else libarchive reads | | libarchive, read only |

The built-in formats need nothing and work on every platform including
Windows XP. Everything else uses libarchive, loaded at runtime when it is
installed (part of macOS and FreeBSD, `libarchive13` on Linux, `archive.dll`
next to `jsonfs.exe` on Windows); zip archives with bzip2/LZMA/zstd members
automatically fall back to it.

What is converted:

* regular files, directories, symlinks, hard links (tar, cpio; zip and 7z
  store the content twice), FIFOs and device nodes (tar, cpio), permissions,
  modification and access times, extended attributes (pax `SCHILY.xattr`)
* owners: kept when importing as root (like `tar`), otherwise the files
  belong to you; `--same-owner` / `--no-same-owner` override
* names: UTF-8 everywhere (zip: the UTF-8 flag and Info-ZIP's Unicode path
  field are honoured, legacy names are decoded as code page 437); long names
  use ustar prefixes, pax or GNU long-name records as needed
* tar writes plain ustar where possible and pax headers only when needed;
  `--pax` always adds them (nanosecond and access times)
* safety: entries with `..` components are skipped, absolute paths are kept
  inside the image, checksums (zip CRC-32, gzip CRC) are verified and a
  failed import leaves no half-filled image behind

## Folders and files without mounting

```sh
jsonfs pack ./photos photos.json            # folder -> new image (-c, -V, -L as for mkfs)
jsonfs unpack photos.json ./restored        # image -> folder (--at TIME for a past state)
jsonfs add photos.json new.jpg /2026/       # add files/folders (-p creates parents, -f replaces)
jsonfs mv photos.json /2026/new.jpg /2026/beach.jpg   # rename / move (-n: never replace)
jsonfs get photos.json /2026/beach.jpg .    # extract a file or folder (--at TIME)
jsonfs rm -r photos.json /2025              # remove (-r for folders, -f ignores missing)
jsonfs mkdir -p photos.json /2027/jan       # create folders
```

These commands lock the image like a mount does (they refuse to modify a
mounted image), honour versioning (removed files stay in the history), and
leave the image untouched when an operation fails part-way.

Offline tools wait up to `$JSONFS_LOCK_TIMEOUT` seconds (default 10) for a
mount that is shutting down, and refuse to modify a mounted image.

`mkfs.jsonfs` and `fsck.jsonfs` are installed as aliases, so `mkfs -t jsonfs`
and `fsck -t jsonfs` work on Linux.

## Installing / uninstalling

* Linux / macOS / FreeBSD: `sudo sh jsonfs-VER-OS-ARCH.run` (install),
  `sudo sh jsonfs-VER-OS-ARCH.run uninstall` or `sudo jsonfs-uninstall`.
  `--prefix DIR` installs elsewhere (e.g. `--prefix $HOME/.local`, no root).
* Windows: run `jsonfs-VER-windows-ARCH-setup.exe`; uninstall from
  *Add/Remove Programs* or the Start-menu entry. Silent: `/S`.

Kernel side: libfuse 2 or 3 (Linux), macFUSE or FUSE-T (macOS), fusefs +
fusefs-libs (FreeBSD), WinFsp (Windows 7–11), Dokan 0.6.0 (Windows XP/Vista).
