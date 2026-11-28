#!/usr/bin/env python3
"""EH's close, localising group-60480: three grouped shapes and one global one, KDS only."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eh_s0_premise as eh, parity_bench as pb
KEEP = ("agg-global", "group-7560", "group-7560-sum", "group-60480")
if __name__ == "__main__":
    pb.SHAPES.clear()
    pb.SHAPES.update({n: (ops, (lambda sql: lambda r, a: sql)(sql)) for n, (ops, sql) in eh.SHAPES.items() if n in KEEP})
    pb.main()
