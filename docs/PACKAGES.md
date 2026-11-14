# Release packages

Every release has the files below. `VER` is the version (for example `1.3.1`)
and `ARCH` is the CPU architecture.

## Which file do I need?

| Computer | Download | Then |
|---|---|---|
| Windows 7–11, no WinFsp yet | `jsonfs-VER-windows-ARCH-setup-with-drivers.exe` | run it, and the included WinFsp is installed when needed |
| Windows XP / Vista (x86, x64) | `jsonfs-VER-windows-ARCH-setup-with-drivers.exe` | the included Dokan 0.6.0 is installed when needed |
| Windows with WinFsp or Dokan already installed, or offline/managed PCs that get drivers elsewhere | `jsonfs-VER-windows-ARCH-setup.exe` | run it |
| Windows, no installation | `jsonfs-VER-windows-ARCH.zip` | unzip and run `bin\jsonfs.exe` |
| Linux | `jsonfs-VER-linux-ARCH.run` | `sudo sh FILE`, and install `fuse3` (or `fuse`) from your distribution |
| macOS | `jsonfs-VER-macos-ARCH.run` | `sudo sh FILE`, and install macFUSE or FUSE-T. Read [code signing](USAGE.md#macos-code-signing) if macOS says "Operation not permitted" |
| FreeBSD | `jsonfs-VER-freebsd-ARCH.run` | `sudo sh FILE`, then `pkg install fusefs-libs3; kldload fusefs` |

Windows `ARCH` values:
- `x86`: 32-bit.
- `x64`: 64-bit Intel/AMD.
- `arm64`: Windows on ARM.

Linux `ARCH` values: `x86_64`, `i686`, `aarch64` and `armv7`.

On macOS, use `arm64` for Apple silicon and `x86_64` for Intel Macs.

## Every file

| File | What it is |
|---|---|
| `jsonfs-VER-windows-ARCH-setup.exe` | Windows installer **without drivers**. It installs `jsonfs.exe` and `jsonfs-mount.exe`, documentation, PATH, Start-menu entries and an uninstaller (*Add/Remove Programs*). If no filesystem driver is installed, it offers to open the WinFsp / Dokan download page. The maintenance tools work without a driver; only mounting needs one. |
| `jsonfs-VER-windows-ARCH-setup-with-drivers.exe` | The same installer **with drivers bundled**. When no driver is installed, it installs the unmodified WinFsp 2.0 installer on Windows 7 and later, or Dokan 0.6.0 on XP/Vista (x86/x64 only, since Dokan 0.6 has no ARM build). Use it for one-step installs. It is about 2 MB bigger. Uninstalling JSONfs leaves the driver in place, because other programs may use it. |
| `jsonfs-VER-windows-ARCH.zip` | Portable Windows build: the two programs and the documentation. Nothing is installed. |
| `jsonfs-VER-OS-ARCH.run` | Self-extracting installer for Linux, macOS and FreeBSD. It is a shell script with an embedded archive. Commands: `sudo sh FILE` installs, `sudo sh FILE uninstall` removes, `sh FILE status` shows the installation. Options: `--prefix DIR`, and `--codesign IDENTITY` on macOS. |
| `jsonfs-VER-OS-ARCH.tar.gz` | The same files as the `.run`, as a plain archive. Use it for packaging or manual installs (`sh install.sh` inside it). |
| `winfsp-2.0-source.tar.gz` | Source code of the bundled WinFsp, published as its license asks. You do not need it to use JSONfs. |
| `dokan-0.6.0-source.tar.gz` | Source code of the bundled Dokan 0.6.0 (LGPL). You do not need it to use JSONfs. |
| `SHA256SUMS` | SHA-256 checksums of all files above. Check them with `sha256sum -c SHA256SUMS --ignore-missing`. |

## Why only Windows has driver-bundled installers

- **Linux and FreeBSD:** FUSE comes from the operating system's own packages
  (`fuse3`, `fusefs-libs3`), which are kept up to date by the distribution.
  A copy shipped with JSONfs would only fall behind.
- **macOS:**
  - macFUSE's license does not allow redistributing it without permission.
  - FUSE-T is distributed by its author.
  - Either one has to be approved by the user in System Settings anyway.
- **Windows:** WinFsp and Dokan can be redistributed unmodified together with
  open-source software (see `THIRD_PARTY_NOTICES.md`). That makes a one-step
  installer possible.

## Building the packages

`scripts/build-release.sh all` builds the unbundled packages.
`scripts/build-release.sh --bundle-drivers all` also builds the
`-setup-with-drivers.exe` installers and the driver source archives, which is
what the release workflow runs. The driver installers are downloaded from
their official GitHub releases and checked against SHA-256 values pinned in
the script.
