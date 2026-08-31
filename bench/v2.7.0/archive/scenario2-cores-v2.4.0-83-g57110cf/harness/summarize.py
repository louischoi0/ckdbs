#!/usr/bin/env python3
"""Fold the core-matrix cells into the tables the results file carries."""
import json, os, re, sys, glob

OUT = os.path.expanduser("~/bench-s2-cores/out")
PHASES = ["booking", "commit", "cargo-lookup", "credit-lookup", "capacity-read",
          "recipe-read", "freight-insert", "charge-insert", "operation-update",
          "org-update", "manifest-scan", "voyage-rollup", "customer-statement"]


def cell(label):
    jp = os.path.join(OUT, label + ".json")
    tp = os.path.join(OUT, label + ".txt")
    mp = os.path.join(OUT, label + ".meta")
    if not os.path.exists(jp):
        return None
    d = json.load(open(jp))
    ph = {p["phase"]: p for p in d["phases"]}
    txt = open(tp, errors="replace").read() if os.path.exists(tp) else ""
    meta = open(mp, errors="replace").read() if os.path.exists(mp) else ""

    m = re.search(r"(\d+) statement\(s\) failed; first: (.*)", txt)
    refused, refused_first = (int(m.group(1)), m.group(2).strip()) if m else (0, "")

    ticks_b = re.search(r"ticks before: (\d+) (\d+)", txt)
    ticks_a = re.search(r"ticks after:  (\d+) (\d+)", txt)
    cpu_s = None
    if ticks_b and ticks_a:
        d_ticks = (int(ticks_a.group(1)) + int(ticks_a.group(2))
                   - int(ticks_b.group(1)) - int(ticks_b.group(2)))
        cpu_s = d_ticks / os.sysconf("SC_CLK_TCK")

    def g(name, field="mean_us"):
        return ph.get(name, {}).get(field)

    def metaval(key):
        mm = re.search(re.escape(key) + r"=(\S+)", meta)
        return mm.group(1) if mm else None

    return dict(
        label=label,
        cores=int(re.search(r"cores: (\d+)", txt).group(1)) if re.search(r"cores: (\d+)", txt) else None,
        pl=re.search(r"peer_listeners: (\w+)", txt).group(1) if re.search(r"peer_listeners: (\w+)", txt) else None,
        bookers=d["meta"]["bookers"],
        committed=d["meta"]["outcomes"]["committed"],
        tps=d["meta"]["tps"],
        conflicted=d["meta"]["outcomes"]["conflicted"],
        retries=d["meta"]["retries"],
        booking_mean=g("booking"), booking_p50=g("booking", "p50_us"),
        booking_p95=g("booking", "p95_us"), booking_p99=g("booking", "p99_us"),
        booking_p0=g("booking", "p0_us"), booking_p25=g("booking", "p25_us"),
        booking_max=g("booking", "max_us"),
        commit_mean=g("commit"), commit_p50=g("commit", "p50_us"),
        commit_p95=g("commit", "p95_us"), commit_p99=g("commit", "p99_us"),
        commit_p0=g("commit", "p0_us"), commit_p25=g("commit", "p25_us"),
        commit_max=g("commit", "max_us"),
        eight=sum(g(p) or 0 for p in ["cargo-lookup", "credit-lookup", "capacity-read",
                                      "recipe-read", "freight-insert", "operation-update",
                                      "org-update"]) + (g("charge-insert") or 0) *
              ((ph.get("charge-insert", {}).get("ops", 0) or 0) /
               max(1, ph.get("booking", {}).get("ops", 1))),
        phases=ph,
        refused=refused, refused_first=refused_first,
        verify_checks=d["meta"].get("verify", {}).get("checks", 0),
        verify_failures=d["meta"].get("verify", {}).get("failures", 0),
        cpu_s=cpu_s,
        load_max=(re.search(r"load samples max: ([\d.]+)", txt).group(1)
                  if re.search(r"load samples max: ([\d.]+)", txt) else None),
        ranges=metaval("split_relation_detail"),
        meta_raw=meta,
    )


def main():
    labels = sorted(os.path.basename(p)[:-5] for p in glob.glob(os.path.join(OUT, "*.json")))
    rows = [c for c in (cell(l) for l in labels) if c]
    for r in rows:
        print(f"{r['label']:<18} cores={r['cores']} pl={r['pl']:<3} b={r['bookers']} "
              f"tps={r['tps']:8.1f} book={r['booking_mean'] or 0:9.1f} "
              f"commit={r['commit_mean'] or 0:9.1f} "
              f"eight={r['eight']:8.1f} refused={r['refused']:3d} "
              f"vchk={r['verify_checks']}/{r['verify_failures']} "
              f"cpu={r['cpu_s']} load={r['load_max']} ranges={r['ranges']}")
    json.dump(rows, open(os.path.expanduser("~/bench-s2-cores/summary.json"), "w"),
              indent=1, default=str)


if __name__ == "__main__":
    main()
