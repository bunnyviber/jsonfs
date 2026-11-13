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

* The image is downloaded once and mounted **read-only** (`-o rw` and
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
