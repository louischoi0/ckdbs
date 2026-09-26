"""AT-S13: markdown tables from the driver's JSON, p0/p25/p50/p99/qps/err per arm."""
import json
import sys

ARMS = ("update-hot", "update-disjoint", "update-disjoint-again", "insert-omitted",
        "select-hot", "ping")

for path in sys.argv[1:]:
    d = json.load(open(path))
    labels = list(d["arms"])
    print(f"\n**`{path.rsplit('/', 1)[-1]}`** - sessions {d['sessions']}, "
          f"{d['ops_per_session_per_arm']} ops/arm/session, {d['blocks']} blocks; "
          f"load before `{d['host_before']['loadavg']}`, after `{d['host_after']['loadavg']}`")
    for label in labels:
        pool = d.get("pools", {}).get(label, {})
        print(f"- `{label}`: cores {', '.join(pool.get('cores', [])) or '?'}"
              + (f", pinned to {pool['pin_core']} ({pool['opened']} connections opened)"
                 if pool.get("pin_core") is not None else ""))
    print()
    print("| arm | " + " | ".join(f"{l} p0 / p25 / p50 / p99 µs, qps, err" for l in labels) + " |")
    print("|---|" + "---|" * len(labels))
    for arm in ARMS:
        cells = []
        for label in labels:
            s = d["arms"][label].get(arm)
            if s is None:
                cells.append("-")
                continue
            cells.append(f"{s['p0_us']} / {s['p25_us']} / {s['p50_us']} / {s['p99_us']}, "
                         f"{s['qps']:.0f}, {s['errors']}")
        print(f"| `{arm}` | " + " | ".join(cells) + " |")
