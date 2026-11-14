# JSONfs - build system
#
#   make                       native build into build/native/
#   make test                  run the test-suite (unit + toolkit + FUSE if available)
#   make TARGET=<t>            cross build (see scripts/targets.mk / docs/BUILDING.md)
#   make release               build every target and package installers (scripts/build-release.sh)
#
# Everything is plain C99 with no third-party runtime dependencies.  FUSE is
# loaded at runtime (dlopen) on Linux, macOS and FreeBSD; WinFsp / Dokan are
# loaded at runtime on Windows, so no driver SDK is needed to link.

VERSION     := 1.3.1
TARGET      ?= native
BUILD       ?= build/$(TARGET)

include scripts/targets.mk

CSTD        ?= -std=gnu99
OPT         ?= -O2
WARN        ?= -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CFLAGS      += $(CSTD) $(OPT) $(WARN) -DJFS_BUILD_VERSION=\"$(VERSION)\" $(TARGET_CFLAGS)
LDFLAGS     += $(TARGET_LDFLAGS)

LIB_SRCS    := src/lib/jfs_util.c src/lib/jfs_json.c src/lib/jfs_codec.c src/lib/jfs_deflate.c \
               src/lib/jfs_image.c src/lib/jfs_vfs.c src/lib/jfs_maint.c src/lib/jfs_http.c src/lib/jfs_remote.c src/lib/jfs_archive.c
TOOL_SRCS   := src/tools/jsonfs.c
MOUNT_SRCS  := src/mount/mount_main.c src/mount/mount_fuse.c src/mount/mount_dokan.c src/mount/mount_refresh.c

LIB_OBJS    := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(LIB_SRCS))
TOOL_OBJS   := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(TOOL_SRCS))
MOUNT_OBJS  := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(MOUNT_SRCS))

TOOL_BIN    := $(BUILD)/bin/jsonfs$(EXE)
MOUNT_BIN   := $(BUILD)/bin/jsonfs-mount$(EXE)
LIB_A       := $(BUILD)/lib/libjsonfs.a

.PHONY: all lib tools mount test unit-test clean distclean release fmt install uninstall

all: $(TOOL_BIN) $(MOUNT_BIN)

lib: $(LIB_A)
tools: $(TOOL_BIN)
mount: $(MOUNT_BIN)

$(BUILD)/obj/%.o: src/%.c $(wildcard src/lib/*.h src/mount/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_A): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(TOOL_BIN): $(TOOL_OBJS) $(LIB_A)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(TOOL_OBJS) $(LIB_A) $(LDFLAGS) $(TOOL_LIBS)

$(MOUNT_BIN): $(MOUNT_OBJS) $(LIB_A)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $(MOUNT_OBJS) $(LIB_A) $(LDFLAGS) $(MOUNT_LIBS)

# ---------------------------------------------------------------- tests ----
UNIT_BIN := $(BUILD)/bin/jfs-unit-tests$(EXE)
$(UNIT_BIN): tests/unit_tests.c $(LIB_A)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Isrc/lib -o $@ $< $(LIB_A) $(LDFLAGS) $(TOOL_LIBS)

unit-test: $(UNIT_BIN)
	$(UNIT_BIN)

test: all $(UNIT_BIN)
	$(UNIT_BIN)
	sh tests/toolkit_test.sh $(abspath $(TOOL_BIN))
	sh tests/archive_test.sh $(abspath $(TOOL_BIN))
	sh tests/mount_test.sh $(abspath $(TOOL_BIN)) $(abspath $(MOUNT_BIN))
	sh tests/http_test.sh $(abspath $(TOOL_BIN)) $(abspath $(MOUNT_BIN))

# -------------------------------------------------------------- install ----
PREFIX ?= /usr/local
install: all
	sh installer/unix/install.sh install --prefix "$(PREFIX)" --from "$(BUILD)/bin"

uninstall:
	sh installer/unix/install.sh uninstall --prefix "$(PREFIX)"

release:
	sh scripts/build-release.sh all

clean:
	rm -rf $(BUILD)

distclean:
	rm -rf build dist
