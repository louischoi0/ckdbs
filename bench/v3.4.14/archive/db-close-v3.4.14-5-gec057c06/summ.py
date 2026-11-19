import json, sys, collections
rows = [json.loads(l) for f in sys.argv[1:] for l in open(f)]
cells = collections.OrderedDict()
rec = []
for r in rows:
    if r.get("recovery"):
        rec.append(r); continue
    cells.setdefault(r["cell"], {}).setdefault(r["label"], []).append(r)

def avg(xs): return sum(xs) / len(xs)

def per(r):
    n = r["statements"]; d = r["delta"]
    ck = r["checkpoints"]
    fi = d.get("pool_fetch_images")
    w = r["waits_us"]
    return dict(sps=r["stmt_per_s"], lat=r["lat_us"], cpu=r["cpu_us_per_stmt"],
                logb=d["wal_append_lsn"] / n, fi=fi, fib=(fi / n if fi is not None else None),
                ckn=len(ck), ckmean=avg([c["duration_us"] for c in ck]) if ck else None,
                ckmax=max([c["duration_us"] for c in ck]) if ck else None,
                ckpg=d["contention_checkpoint_pages_written"],
                drained=d["pool_dirty_drained"], bad=r["bad"], load=r["loadavg"], w=w, n=n,
                flush=w.get("commit.wal_flush", [0, 0]))

print("== per cell, per round")
for c, d in cells.items():
    for lab in "AB":
        for i, r in enumerate(d.get(lab, [])):
            p = per(r)
            l = p["lat"]
            print(f"{c:28s} {lab}{i+1} sps={p['sps']:8.0f} p0={l['0']:.0f} p25={l['25']:.0f} p50={l['50']:.0f} "
                  f"p90={l['90']:.0f} p99={l['99']:.0f} cpu={p['cpu']:.1f} logB={p['logb']:.1f} "
                  f"fi={p['fi']} fiB/st={(p['fib'] or 0):.2f} ck={p['ckn']} ckmean={p['ckmean']} ckmax={p['ckmax']} "
                  f"ckpg={p['ckpg']} drained={p['drained']} bad={p['bad']} load={p['load']}")
print("== recovery")
for r in rec:
    print(r["label"], r["interval_ms"], r["rep"], f"mount={r['mount_s']:.3f}s lsn={r['wal_append_lsn_at_kill']} wal={r['wal_dir_bytes']} work={r['work_s']:.1f} bad={r['bad']} {r['count_after']}")
