#!/usr/bin/env python3
"""DI-S0 arm A0: RecordStatement's call of StatementStats::Record compiled out.
  a0.py SRC apply|revert"""
import sys

path = sys.argv[1] + "/src/server/command_dispatcher.cpp"
on = "    stats::StatementStats::Record(core_id_, out.fetch_id, line, refused,\n"
off = "    if (false) stats::StatementStats::Record(core_id_, out.fetch_id, line, refused,\n"
s = open(path).read()
old, new = (on, off) if sys.argv[2] == "apply" else (off, on)
assert s.count(old) == 1, (sys.argv[2], s.count(old))
open(path, "w").write(s.replace(old, new))
