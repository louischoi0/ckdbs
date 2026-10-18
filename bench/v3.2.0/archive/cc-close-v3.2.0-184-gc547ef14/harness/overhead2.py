#!/usr/bin/env python3
"""Second, independent replicate of the OVERHEAD cells: fresh servers, 10 rounds (the first replicate had 5)."""
import os, random, sys, time
sys.path.insert(0, "/home/cdkbs/bench-runs/cc-close/harness")
os.environ.setdefault("ROUNDS", "10")
import cc_close_run as H

H.ROUNDS = 10


def main():
    srvs = {k: H.Srv(k) for k in ("A", "A2", "B")}
    for s in srvs.values(): H.setup_ov(s)
    ops_of = H.load_json("ops-overhead.json", None)
    for c, make in H.OVERHEAD.items():
        for s in srvs.values():
            H.status(f"overhead2 warm {c} {s.label}")
            rng = random.Random(f"w-{c}")
            for _ in range(min(ops_of[c], 3000)): s.q(make(rng))
    time.sleep(2.5)
    H.rounds_phase("overhead2", srvs, H.OVERHEAD, [("A", "A"), ("A2", "A2"), ("B", "B")], ops_of)
    for s in srvs.values(): s.stop()
    H.status("OVERHEAD2 DONE")


if __name__ == "__main__":
    main()
