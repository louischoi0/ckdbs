#!/usr/bin/env python3
"""Third replicate of the two read OVERHEAD cells only, 16 rounds, to tighten the point-read delta."""
import os, random, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
import cc_close_run as H

H.ROUNDS = 16


def main():
    srvs = {k: H.Srv(k) for k in ("A", "A2", "B")}
    for s in srvs.values(): H.setup_ov(s)
    ops_of = H.load_json("ops-overhead.json", None)
    cells = {k: H.OVERHEAD[k] for k in ("ov-point-read", "ov-join3-tuplekey")}
    for c, make in cells.items():
        for s in srvs.values():
            H.status(f"overhead3 warm {c} {s.label}")
            rng = random.Random(f"w-{c}")
            for _ in range(min(ops_of[c], 3000)): s.q(make(rng))
    time.sleep(2.5)
    H.rounds_phase("overhead3", srvs, cells, [("A", "A"), ("A2", "A2"), ("B", "B")], ops_of)
    for s in srvs.values(): s.stop()
    H.status("OVERHEAD3 DONE")


if __name__ == "__main__":
    main()
