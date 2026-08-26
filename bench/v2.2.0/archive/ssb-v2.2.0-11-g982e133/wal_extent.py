#!/usr/bin/env python3
"""Bytes actually appended to each core's WAL stream.

Segments are preallocated to their full size, so `ls` and `du` both say 64
MiB whatever was written. The written extent is the offset of the last
non-zero byte, found by scanning backwards a chunk at a time.
"""
import os
import sys

CHUNK = 1 << 20

for path in sorted(sys.argv[1:]):
    size = os.path.getsize(path)
    last = 0
    with open(path, "rb") as fh:
        off = size
        while off > 0:
            start = max(0, off - CHUNK)
            fh.seek(start)
            buf = fh.read(off - start)
            trimmed = buf.rstrip(b"\x00")
            if trimmed:
                last = start + len(trimmed)
                break
            off = start
    print(f"{os.path.basename(path):16s} written={last:>12,} bytes "
          f"of {size:,} preallocated")
