"""AT-S13: the wait breakdown, from the driver's per-core SHOW META snapshots.

Per core, after minus before: foreground polls (statements run), polled time
per poll (engine time inside a statement's task), idle blocks per poll and
idle time per poll (the reactor asleep between statements), wakes received.
"""
import json
import sys

KEYS = ("sched_foreground_polls", "sched_foreground_polled_us", "sched_idle_blocks",
        "sched_idle_block_us", "sched_wakes_received", "sched_iterations")


def parse(text):
    out = {}
    for tok in text.replace("\\n", " ").split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            if k in KEYS:
                out[k] = float(v)
    return out


for path in sys.argv[1:]:
    d = json.load(open(path))
    print(f"== {path.rsplit('/', 1)[-1]}")
    for label in d["show_meta_after"]:
        for core, after in sorted(d["show_meta_after"][label].items()):
            before = d["show_meta_before"][label].get(core)
            if before is None:
                continue
            a, b = parse(after), parse(before)
            delta = {k: a.get(k, 0) - b.get(k, 0) for k in KEYS}
            polls = delta["sched_foreground_polls"] or 1
            print(f"  {label:<10} core {core}: polls={delta['sched_foreground_polls']:>7.0f} "
                  f"polled_us/poll={delta['sched_foreground_polled_us'] / polls:6.1f} "
                  f"idle_blocks/poll={delta['sched_idle_blocks'] / polls:5.2f} "
                  f"idle_us/poll={delta['sched_idle_block_us'] / polls:7.1f} "
                  f"wakes={delta['sched_wakes_received']:.0f}")
