import json, sys, glob, os
D = "/home/cdkbs/ckdbs/.claude/worktrees/online-index-build/build-release/ci-close"

def pct(xs, p):
    xs = sorted(xs)
    i = min(len(xs) - 1, max(0, int(round(p / 100 * (len(xs) - 1)))))
    return xs[i]

def meta(m):
    d = {}
    for r in m[1]:
        d[bytes.fromhex(r[1]).decode()] = bytes.fromhex(r[2]).decode()
    return d

def row(name, lat):
    ms = [x * 1000 for x in lat]
    return f"| {name} | {len(ms):,} | " + " | ".join(f"{pct(ms,p):.3f}" for p in (0, 25, 50, 90, 99, 99.9, 100)) + " |"

for v in ("base", "closing"):
    for i in range(3):
        f = f"{D}/b-{v}-{i}.json"
        if not os.path.exists(f):
            continue
        d = json.load(open(f))
        r = d["runs"][0]
        b = r["build"]
        w = r["writer"]
        s, e = b["start"], b["end"]
        inb = [l for t, l in w if s <= t < e]
        pre = [l for t, l in w if t < s]
        post = [l for t, l in w if t >= e]
        allw = [l for t, l in w]
        ma, mb, mt, m0 = meta((None, b["meta_a"][1])), meta((None, b["meta_b"][1])), meta((None, r["meta_after"][1])), meta((None, r["meta_before"][1]))
        lsn_b = int(mb["wal_append_lsn"]); lsn_a = int(ma["wal_append_lsn"]); lsn_e = int(mt["wal_append_lsn"])
        tail_ops = len(post)
        bytes_op = (lsn_e - lsn_a) / max(1, tail_ops)
        w_in_build = len([1 for t, l in w if s <= t < e])
        mx = max(w, key=lambda x: x[1])
        print(f"== {v} pair {i}: build {b['elapsed']:.3f}s [{s:.2f},{e:.2f}] tag={b['tag']!r}")
        print(f"   writer ops total {len(w)} pre {len(pre)} in-build {len(inb)} post {len(post)}; max stall {mx[1]*1000:.1f} ms at t={mx[0]:.2f}")
        print(f"   in-build rate {len(inb)/(e-s):.0f}/s pre rate {len(pre)/s:.0f}/s post {len(post)/max(1e-9,(max(t for t,l in w)-e)):.0f}/s; reader ops {len(r['reader'])} errs {r['writer_errs']} {r['reader_errs']}")
        print(f"   lsn build delta {lsn_a-lsn_b:,} B; tail writer bytes/op {bytes_op:.0f}; build net of writer {(lsn_a-lsn_b)-len(inb)*bytes_op:,.0f}; pages alloc {m0['pages_allocated']}->{mt['pages_allocated']} (build window {mb['pages_allocated']}->{ma['pages_allocated']})")
        print(f"   server_cpu_s {r['server_cpu_s']:.1f}; checkpoint_runs {mt['checkpoint_runs']} last_pages {mt['checkpoint_last_pages']}")
        print(row("pre", pre)); print(row("in-build", inb)); print(row("post", post)); print(row("all", allw))
        lw = r["lock_waits"]
        print("   lock_waits fields", [x if isinstance(x, str) else x for x in lw[0]] if isinstance(lw, list) else lw)
        if isinstance(lw, list):
            for x in lw[1]:
                print("    ", [bytes.fromhex(y).hex() if y else y for y in x][:14])
        print("   indexes", r["indexes"][1] if isinstance(r["indexes"], list) else r["indexes"])
        top = sorted(w, key=lambda x: -x[1])[:6]
        print("   top stalls", [(round(t,2), round(l*1000,1)) for t, l in top])
