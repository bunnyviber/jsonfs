# JSONfs - target definitions for native and cross builds.
#
# TARGET            toolchain                    notes
# ----------------- ---------------------------- -----------------------------------
# native            $(CC) (default cc)           whatever the build host is
# linux-x86_64      zig cc / x86_64-linux-gnu    glibc >= 2.17, FUSE 2 or 3 at runtime
# linux-i686        zig cc / x86-linux-gnu
# linux-aarch64     zig cc / aarch64-linux-gnu
# linux-armv7       zig cc / arm-linux-gnueabihf
# macos-x86_64      zig cc / x86_64-macos        macOS 10.13+, macFUSE or FUSE-T at runtime
# macos-arm64       zig cc / aarch64-macos       macOS 11+
# freebsd-x86_64    zig cc / x86_64-freebsd      FreeBSD 13+, fusefs(5) + fusefs-libs
# freebsd-aarch64   zig cc / aarch64-freebsd
# windows-x86       i686-w64-mingw32-gcc         Windows XP SP2+ (32 bit)
# windows-x64       x86_64-w64-mingw32-gcc       Windows XP x64 / Server 2003+ (64 bit)
# windows-arm64     zig cc / aarch64-windows-gnu Windows 10+ on ARM
#
# Override the compiler with CC=... (e.g. CC=clang for native, or a
# different cross compiler); ZIG=... selects the zig executable.

ZIG ?= zig
EXE :=
TARGET_CFLAGS :=
TARGET_LDFLAGS :=
TOOL_LIBS :=
MOUNT_LIBS :=

# Windows: keep the PE subsystem version at 5.01/5.02 so the binaries load on
# Windows XP, target the XP API level and link libgcc statically (no
# libgcc_s/winpthread DLLs to ship).  WinFsp headers are optional at build
# time (scripts/fetch-deps.sh); without them only the Dokan backend is built.
WINFSP_INC ?= third_party/winfsp/inc
WIN_COMMON_CFLAGS = -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -D__USE_MINGW_ANSI_STDIO=0 \
                    $(if $(wildcard $(WINFSP_INC)/fuse/fuse.h),-I$(WINFSP_INC) -DJFS_HAVE_WINFSP=1,)
WIN_COMMON_LIBS   = -static -static-libgcc -lwinhttp -luser32 -ladvapi32 -lkernel32

ifeq ($(TARGET),native)
  CC ?= cc
  UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
  ifeq ($(UNAME_S),Linux)
    TOOL_LIBS  := -lpthread -ldl
    MOUNT_LIBS := -lpthread -ldl
  else ifeq ($(UNAME_S),Darwin)
    TOOL_LIBS  :=
    MOUNT_LIBS :=
  else ifneq ($(filter FreeBSD NetBSD OpenBSD DragonFly,$(UNAME_S)),)
    TOOL_LIBS  := -lpthread
    MOUNT_LIBS := -lpthread
  else
    # MSYS2 / MinGW native build on Windows
    EXE := .exe
    TARGET_CFLAGS += $(WIN_COMMON_CFLAGS)
    TOOL_LIBS  := $(WIN_COMMON_LIBS)
    MOUNT_LIBS := $(WIN_COMMON_LIBS)
  endif
else ifeq ($(TARGET),linux-x86_64)
  CC := $(ZIG) cc -target x86_64-linux-gnu.2.17
  AR := $(ZIG) ar
  TOOL_LIBS := -lpthread -ldl
  MOUNT_LIBS := -lpthread -ldl
else ifeq ($(TARGET),linux-i686)
  CC := $(ZIG) cc -target x86-linux-gnu.2.17
  AR := $(ZIG) ar
  TOOL_LIBS := -lpthread -ldl
  MOUNT_LIBS := -lpthread -ldl
else ifeq ($(TARGET),linux-aarch64)
  CC := $(ZIG) cc -target aarch64-linux-gnu.2.17
  AR := $(ZIG) ar
  TOOL_LIBS := -lpthread -ldl
  MOUNT_LIBS := -lpthread -ldl
else ifeq ($(TARGET),linux-armv7)
  CC := $(ZIG) cc -target arm-linux-gnueabihf.2.17
  AR := $(ZIG) ar
  TOOL_LIBS := -lpthread -ldl
  MOUNT_LIBS := -lpthread -ldl
else ifeq ($(TARGET),macos-x86_64)
  CC := $(ZIG) cc -target x86_64-macos.10.13
  AR := $(ZIG) ar
else ifeq ($(TARGET),macos-arm64)
  CC := $(ZIG) cc -target aarch64-macos.11.0
  AR := $(ZIG) ar
else ifeq ($(TARGET),freebsd-x86_64)
  CC := $(ZIG) cc -target x86_64-freebsd
  AR := $(ZIG) ar
  TOOL_LIBS := -lpthread
  MOUNT_LIBS := -lpthread
else ifeq ($(TARGET),freebsd-aarch64)
  CC := $(ZIG) cc -target aarch64-freebsd
  AR := $(ZIG) ar
  TOOL_LIBS := -lpthread
  MOUNT_LIBS := -lpthread
else ifeq ($(TARGET),windows-x86)
  CC := i686-w64-mingw32-gcc
  AR := i686-w64-mingw32-ar
  EXE := .exe
  TARGET_CFLAGS += $(WIN_COMMON_CFLAGS) -march=i686
  TARGET_LDFLAGS += -Wl,--major-subsystem-version,5,--minor-subsystem-version,1 \
                    -Wl,--major-os-version,5,--minor-os-version,1
  TOOL_LIBS := $(WIN_COMMON_LIBS)
  MOUNT_LIBS := $(WIN_COMMON_LIBS)
else ifeq ($(TARGET),windows-x64)
  CC := x86_64-w64-mingw32-gcc
  AR := x86_64-w64-mingw32-ar
  EXE := .exe
  TARGET_CFLAGS += $(WIN_COMMON_CFLAGS)
  TARGET_LDFLAGS += -Wl,--major-subsystem-version,5,--minor-subsystem-version,2 \
                    -Wl,--major-os-version,5,--minor-os-version,2
  TOOL_LIBS := $(WIN_COMMON_LIBS)
  MOUNT_LIBS := $(WIN_COMMON_LIBS)
else ifeq ($(TARGET),windows-arm64)
  CC := $(ZIG) cc -target aarch64-windows-gnu
  AR := $(ZIG) ar
  EXE := .exe
  TARGET_CFLAGS += $(if $(wildcard $(WINFSP_INC)/fuse/fuse.h),-I$(WINFSP_INC) -DJFS_HAVE_WINFSP=1,)
  TOOL_LIBS := -lwinhttp -luser32 -ladvapi32
  MOUNT_LIBS := -lwinhttp -luser32 -ladvapi32
else
  $(error unknown TARGET '$(TARGET)'; see scripts/targets.mk)
endif
