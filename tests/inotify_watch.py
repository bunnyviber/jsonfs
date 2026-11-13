#!/usr/bin/env python3
"""Minimal inotify watcher for the JSONfs tests (Linux only, stdlib only).

usage: inotify_watch.py OUTFILE SECONDS DIR...

Watches every DIR (non-recursively) for SECONDS and writes one line per event
to OUTFILE: "<EVENT> <dir>/<name>".  Writes "READY" to OUTFILE.ready once the
watches are installed.
"""
import ctypes
import os
import select
import struct
import sys
import time

FLAGS = {
    0x00000002: "MODIFY", 0x00000004: "ATTRIB", 0x00000008: "CLOSE_WRITE",
    0x00000040: "MOVED_FROM", 0x00000080: "MOVED_TO", 0x00000100: "CREATE",
    0x00000200: "DELETE", 0x00000400: "DELETE_SELF",
}
MASK = sum(FLAGS)


def main():
    out, seconds, dirs = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
    libc = ctypes.CDLL(None, use_errno=True)
    fd = libc.inotify_init1(os.O_NONBLOCK)
    if fd < 0:
        sys.exit("inotify_init1 failed")
    wds = {}
    for d in dirs:
        wd = libc.inotify_add_watch(fd, d.encode(), MASK)
        if wd < 0:
            sys.exit("cannot watch %s: errno %d" % (d, ctypes.get_errno()))
        wds[wd] = d
    with open(out + ".ready", "w") as f:
        f.write("READY\n")
    end = time.time() + seconds
    with open(out, "w") as f:
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.1)
            if not r:
                continue
            buf = os.read(fd, 65536)
            i = 0
            while i < len(buf):
                wd, mask, _cookie, ln = struct.unpack_from("iIII", buf, i)
                name = buf[i + 16:i + 16 + ln].rstrip(b"\0").decode("utf-8", "replace")
                i += 16 + ln
                for bit, label in FLAGS.items():
                    if mask & bit:
                        f.write("%s %s/%s\n" % (label, wds.get(wd, "?"), name))
                f.flush()


if __name__ == "__main__":
    main()
