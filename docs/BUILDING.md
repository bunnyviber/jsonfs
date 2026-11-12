# Building JSONfs

JSONfs is plain C99 without third-party runtime libraries (JSON, base64,
SHA-256, DEFLATE, LZ4 and RLE are implemented in `src/lib`). The mount
driver loads the platform's FUSE library (or WinFsp/Dokan on Windows) at
runtime, so **no driver SDK is needed to link** and every target can be
cross-compiled from one Linux machine.

```
src/lib/      core library: image format, POSIX semantics, versioning, maintenance
src/tools/    jsonfs  (toolkit; also mkfs.jsonfs / fsck.jsonfs)
src/mount/    jsonfs-mount (FUSE 2/3, macFUSE, FUSE-T, WinFsp, Dokan 0.6 backends)
tests/        unit tests, toolkit tests, FUSE mount tests
installer/    unix .run installer/uninstaller, NSIS script for Windows
scripts/      targets.mk (toolchains), fetch-deps.sh, build-release.sh
```

## Native build

Requirements: a C compiler, `make`. Nothing else.

```sh
make                 # build/native/bin/{jsonfs,jsonfs-mount}
make test            # unit + toolkit tests, plus FUSE mount tests if /dev/fuse works
sudo make install    # PREFIX=/usr/local by default
sudo make uninstall
```

Works on Linux (gcc/clang), macOS (Xcode command line tools), FreeBSD
(`cc`, use `gmake`), and on Windows under MSYS2/MinGW.

## Cross builds

`make TARGET=<target>` selects a toolchain from `scripts/targets.mk`:

| TARGET | toolchain | runs on |
|---|---|---|
| `linux-x86_64`, `linux-i686`, `linux-aarch64`, `linux-armv7` | `zig cc` (glibc 2.17) | any Linux since ~2014 |
| `macos-x86_64` / `macos-arm64` | `zig cc` | macOS 10.13+ / 11+ |
| `freebsd-x86_64`, `freebsd-aarch64` | `zig cc` | FreeBSD 14 |
| `windows-x86` | `i686-w64-mingw32-gcc` | Windows XP SP2 … 11 (32 bit) |
| `windows-x64` | `x86_64-w64-mingw32-gcc` | Windows XP x64 / 2003 … 11 |
| `windows-arm64` | `zig cc` | Windows 10/11 on ARM |

Prerequisites:

```sh
sudo apt install make mingw-w64 nsis      # Debian/Ubuntu: Windows toolchain + installer builder
scripts/fetch-deps.sh all                 # zig 0.15.1 (via pip) + WinFsp FUSE headers into third_party/
```

`zig` from `PATH` is used when present, otherwise `third_party/bin/zig`
(`make TARGET=... ZIG=/path/to/zig` overrides). The WinFsp headers are
optional: without them the Windows driver is built with the Dokan backend
only.

Windows XP compatibility is ensured by: `_WIN32_WINNT=0x0501`, linking only
against `kernel32`, `advapi32`, `user32` and `msvcrt`, static libgcc, and
PE subsystem version 5.01/5.02 (newer binutils default to 6.0, which XP
refuses to load).

## Everything at once (release)

```sh
scripts/build-release.sh --test all       # or: unix | windows | <target>...
scripts/build-release.sh --bundle-drivers windows   # embed WinFsp MSI + Dokan 0.6 installer
```

produces in `dist/`:

* `jsonfs-VER-<target>.tar.gz` and self-extracting `jsonfs-VER-<target>.run`
  (install / uninstall / status / --extract) for Linux, macOS, FreeBSD
* `jsonfs-VER-windows-<arch>.zip` and `jsonfs-VER-windows-<arch>-setup.exe`
  (NSIS installer with uninstaller, PATH handling, driver detection)
* `SHA256SUMS`

The GitHub Actions workflow `.github/workflows/build.yml` runs the tests on
Linux and macOS and builds all release artifacts.

## Debugging

* `JSONFS_DEBUG=1 jsonfs ...` / `jsonfs-mount -d ...` print commits, reloads and FUSE traffic.
* `JSONFS_FUSE_LIBRARY=/path/libfuse.so.2 jsonfs-mount ...` forces a FUSE library.
* `make BUILD=build/asan OPT="-O1 -g -fsanitize=address,undefined" LDFLAGS=-fsanitize=address,undefined test`
  runs everything under ASan/UBSan.
* For extra POSIX conformance testing, run
  [pjdfstest](https://github.com/pjd/pjdfstest) as root inside a mount
  created with `-o allow_other`.
