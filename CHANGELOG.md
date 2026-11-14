# Changelog

## 1.3.1 (2026-11-14)

**Windows installers, two flavors:**
- `*-setup-with-drivers.exe` bundles the unmodified WinFsp 2.0 (Windows 7+)
  and Dokan 0.6.0 (XP/Vista, x86/x64) installers, and installs the driver
  when it is missing.
- `*-setup.exe` has no drivers and links to their download pages, as before.
- The driver source archives are attached to the release, and
  `THIRD_PARTY_NOTICES.md` covers their licenses.

**macOS code signing:** the `.run` installer has a new
`--codesign IDENTITY` option. It signs `jsonfs` and `jsonfs-mount` with a
certificate from your keychain (a free self-signed one works), so that macOS
privacy protection can be granted access to them. This fixes "Operation not
permitted" in the home folder. Updates re-sign automatically.
See docs/USAGE.md, "macOS: code signing".

`docs/PACKAGES.md` describes every release file.

## 1.3.0 (2026-11-13)

**Archive conversion.** `jsonfs convert`, `from-archive`, `to-archive` and `archive-formats` turn archives into JSONfs images and back.

- **Built in, on every platform:**
  - tar: ustar, pax, GNU and v7.
  - tar.gz.
  - zip, including zip64.
- **Through libarchive:**
  - 7z, tar.xz, tar.bz2, tar.zst, cpio, iso and more can be read and written.
  - rar, cab and lha are read only.
- **What carries over:** hard links, permissions, times, extended attributes and UTF-8 names.
- **Safety:** paths that would leave the image are refused, and a failed import leaves nothing behind.
- `jsonfs pack` now keeps the hard links of the source folder.

## 1.2.0

**Folder ↔ image tools:**
- `jsonfs pack` and `unpack` convert between folders and images.
- `add`, `mv`, `get`, `rm` and `mkdir` edit an image without mounting it.

**Live web mounts:**
- **Refresh modes:**
  - `refresh=periodic` (eventually consistent, the default).
  - `refresh=strict` (checks on every open and directory listing).
  - `refresh=off`.
- **Cheap checks:** an unchanged image costs one `304` response (ETag) or a 4 KiB header probe.
- **Failures:** the last good version stays mounted when fetching fails.
- **Cache:** `cache=` keeps an on-disk copy for cheap remounts and offline mounts.

**File system events:**
- inotify, kqueue/FSEvents and ReadDirectoryChangesW (WinFsp) report changes made through a mount.
- Remote updates of web mounts are reported too.

## 1.1.0

**Mounting images over the web:** `jsonfs-mount https://… folder`.
- The mount is read-only.
- TLS certificates are verified by default; `--insecure` turns verification off and `--cacert` adds a private CA.
- Plain HTTP never follows redirects, and HTTPS never redirects to HTTP.

## 1.0.0

**The filesystem:** a POSIX filesystem stored in a single JSON document.
- Per-file compression (deflate, lz4, rle or none).
- SHA-256 checksums of every file.
- Time-based versioning, with read-only views of any past moment.
- Multi-attach: several mounts can share one image.

**Maintenance toolkit:** `mkfs`, `fsck`, `snapshot`, `versioning`, `recompress`, `upgrade`, `prune`, `log`, `ls`, `cat`, `import` and `export`.

**Drivers and installers:**
- Drivers for Linux, macOS, FreeBSD and Windows XP–11.
- Installers that can also uninstall.
- Cross-compilation of every target from Linux.
