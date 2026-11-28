#!/usr/bin/env python3
"""EH-S0: what the native fold pays for a row, shape by shape, beside PostgreSQL.

`parity_bench.py` with its shapes replaced: the same data set (`pb_bars`,
60,480 rows of 8 columns by default), the same counting clients on both
sides, the same interleaving and the same server-CPU reading. Each shape adds
one thing to the one before it, so a difference between two rows of the result
is the cost of that thing:

  scan-reject      every row walked and rejected by the scan kernel; 0 rows
  count-star       every row reaches the fold; nothing is decoded
  count-col        ... and one int64 cell is decoded and tested for NULL
  sum              ... and added
  min-max          two folds over one decoded cell
  agg-global       COUNT(*), MIN, MAX, SUM: parity_bench's shape
  group-8          a GROUP BY probe a row, 8 groups
  group-8-sum      ... with a SUM beside the count
  group-7560       7,560 groups founded, 8 rows each
  group-7560-sum   parity_bench's `agg-by-session`
  group-60480      a group founded for every row

Usage: as `parity_bench.py` (`--kds-port`, `--kds-pid`, `--pg-port`, `--json`).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import parity_bench as pb  # noqa: E402

T = "pb_bars"
SHAPES = {
    "scan-reject": (20, f"SELECT * FROM {T} WHERE volume = -1"),
    "count-star": (20, f"SELECT COUNT(*) FROM {T}"),
    "count-col": (20, f"SELECT COUNT(close) FROM {T}"),
    "sum": (20, f"SELECT SUM(volume) FROM {T}"),
    "min-max": (20, f"SELECT MIN(close), MAX(close) FROM {T}"),
    "agg-global": (20, f"SELECT COUNT(*), MIN(close), MAX(close), SUM(volume) FROM {T}"),
    "group-8": (20, f"SELECT symbol_id, COUNT(*) FROM {T} GROUP BY symbol_id"),
    "group-8-sum": (20, f"SELECT symbol_id, COUNT(*), SUM(volume) FROM {T} GROUP BY symbol_id"),
    "group-7560": (10, f"SELECT session_no, COUNT(*) FROM {T} GROUP BY session_no"),
    "group-7560-sum": (10, f"SELECT session_no, COUNT(*), SUM(volume) FROM {T} GROUP BY session_no"),
    "group-60480": (5, f"SELECT id, COUNT(*) FROM {T} GROUP BY id"),
}

if __name__ == "__main__":
    pb.SHAPES.clear()
    pb.SHAPES.update({name: (ops, (lambda sql: lambda r, a: sql)(sql)) for name, (ops, sql) in SHAPES.items()})
    pb.main()
