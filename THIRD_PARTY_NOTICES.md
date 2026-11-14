# Third-party notices

JSONfs itself is MIT licensed (see `LICENSE`). It contains no third-party code.
Filesystem drivers are loaded at runtime only. The Windows installers named
`*-setup-with-drivers.exe` also carry the unmodified driver installers listed
below. The plain `*-setup.exe` installers carry none of them.

## WinFsp

WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos
<https://github.com/winfsp/winfsp>

- **Bundled file:** `winfsp-2.0.23075.msi` (WinFsp 2.0), unmodified, in every
  `*-setup-with-drivers.exe`. It is used on Windows 7 and later.
- **License:** GNU General Public License version 3, with the WinFsp FLOSS
  exception. That exception allows the unmodified WinFsp installer to be
  distributed together with free/open-source software such as JSONfs.
- **Source code:** `winfsp-2.0-source.tar.gz` is attached to every JSONfs
  release. It is also at <https://github.com/winfsp/winfsp/tree/v2.0>.

## Dokan 0.6.0

Dokan Library, Copyright (C) Hiroki Asakawa and the Dokan contributors
<https://github.com/dokan-dev/dokany>

- **Bundled file:** `DokanInstall_0.6.0.exe`, unmodified, in the x86 and x64
  `*-setup-with-drivers.exe`. It is used on Windows XP and Vista.
- **License:** mainly the GNU Lesser General Public License version 3, and
  MIT for some tools. The license files in the source archive are the
  authoritative statement.
- **Source code:** `dokan-0.6.0-source.tar.gz` is attached to every JSONfs
  release. It is also at <https://github.com/dokan-dev/dokany/tree/0.6.0>.

## Runtime-only libraries (never shipped)

These are used only if they are already installed on the computer:

- libfuse, macFUSE, FUSE-T and FreeBSD fusefs-libs.
- libcurl and libarchive.

They keep their own licenses.
