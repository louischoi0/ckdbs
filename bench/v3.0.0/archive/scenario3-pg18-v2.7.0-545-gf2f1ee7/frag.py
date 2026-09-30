#!/usr/bin/env python3
"""Table fragments for the PostgreSQL 18.6 floor documents (scenarios 0-4).

usage: frag.py <s0|s2|s3|s4|s1>   -> markdown tables on stdout

Reads the PG cells' JSON/logs from the run directory and the KDS cells' JSON from
bench/v3.0.0/archive/scenario<N>-v2.7.0-531-g9a0525d/json (measured 2026-09-30
at 9a0525d). Values carry their unit in every cell (bench/README.md).
"""
import json, os, re, statistics, sys

RUN = os.environ.get("PGRUN", "/home/cdkbs/bench-runs/pg18-f2f1ee7")
J = f"{RUN}/json"
L = f"{RUN}/logs"
WT = "/home/cdkbs/ckdbs/.claude/worktrees/bench-pg-floor"
KA = lambda n: f"{WT}/bench/v3.0.0/archive/scenario{n}-v2.7.0-531-g9a0525d"
REPS = ["", "r2-", "r3-"]


def jl(p):
    return json.load(open(p))


def ph(d, name):
    for p in d["phases"]:
        if p["phase"] == name:
            return p
    return None


def pct_row(label, p, extra=""):
    return (f"| {label} | {p['ops']:,} ops | {p['p0_us']:,.1f} µs | {p['p25_us']:,.1f} µs | "
            f"{p['p50_us']:,.1f} µs | {p['p95_us']:,.1f} µs | {p['p99_us']:,.1f} µs | "
            f"{p['max_us']:,.1f} µs | {p['errors']} errors |{extra}")


PCT_HDR = ("| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |\n"
           "|---|---|---|---|---|---|---|---|---|---|")


def load_avg(tag):
    for l in open(f"{L}/{tag}.precheck"):
        if l.startswith("loadavg"):
            return " / ".join(l.split(":")[1].split()[:3])


def pgrep_state(tag):
    t = open(f"{L}/{tag}.precheck").read()
    return "none" if "  none" in t else "SEE precheck FILE"


def stamp_at(tag):
    return open(f"{L}/{tag}.precheck").readline().split(": ")[1].strip()


def spread(v):
    return 100 * (max(v) / min(v) - 1)


def host_table(tags):
    out = ["| cell | precheck UTC | loadavg 1/5/15 | cc1plus / cmake --build / ctest |", "|---|---|---|---|"]
    for t in tags:
        out.append(f"| `{t}` | {stamp_at(t)} | {load_avg(t)} | {pgrep_state(t)} |")
    return "\n".join(out)


# ------------------------------------------------------------------ scenario 0
def s0():
    kds_cells = {"c1-g": "cores 1, group", "c8-g": "cores 8, group", "c1-s": "cores 1, strict", "c8-s": "cores 8, strict"}
    kt = {c: [jl(f"{KA(0)}/json/{r}s0-{c}.json") for r in REPS] for c in kds_cells}
    kv = {c: [d["meta"]["tps"] for d in kt[c]] for c in kds_cells}
    pg = {m: [jl(f"{J}/s0-pg-{m}-r{r}.json") for r in (1, 2, 3)] for m in ("on", "off")}
    pv = {m: [d["meta"]["tps"] for d in pg[m]] for m in pg}

    print("### A. throughput, PostgreSQL, three runs\n")
    print("| cell | synchronous_commit | run 1 | run 2 | run 3 | median | spread (max/min-1) | durable at the reply |")
    print("|---|---|---|---|---|---|---|---|")
    for m, sc, dur in (("on", "on (default)", "yes"), ("off", "off", "**no (non-durable, not like-for-like)**")):
        v = pv[m]
        print(f"| `s0-pg-{m}` | {sc} | " + " | ".join(f"{x:,.1f} tps" for x in v) +
              f" | **{statistics.median(v):,.1f} tps** | {spread(v):.1f} % | {dur} |")
    print()
    print("### B. side by side with the KDS cells of 9a0525d (median of 3 runs each)\n")
    print("| engine / cell | durability | tps median | spread | KDS / PG-on |")
    print("|---|---|---|---|---|")
    pm = statistics.median(pv["on"])
    print(f"| PostgreSQL 18.6, 8 traders | `synchronous_commit = on` | **{pm:,.1f} tps** | {spread(pv['on']):.1f} % | 1.00x (reference for this table) |")
    for c, name in kds_cells.items():
        m = statistics.median(kv[c])
        print(f"| KDS `s0-{c}` ({name}) | {'group' if c.endswith('g') else 'strict'} | {m:,.1f} tps | {spread(kv[c]):.1f} % | {m/pm:.2f}x |")
    print()
    print("### C. percentiles, PostgreSQL `synchronous_commit = on` (all three runs)\n")
    print(PCT_HDR)
    for r in (1, 2, 3):
        d = pg["on"][r - 1]
        for name in ("txn", "trade-insert", "account-update", "profit-scan", "profit-insert"):
            print(pct_row(f"`s0-pg-on-r{r}` | {name}", ph(d, name)).replace("| `", "| `", 1))
    print()
    print("### D. percentiles, PostgreSQL `synchronous_commit = off` (NON-DURABLE, all three runs)\n")
    print(PCT_HDR)
    for r in (1, 2, 3):
        d = pg["off"][r - 1]
        for name in ("txn", "trade-insert", "account-update", "profit-scan", "profit-insert"):
            print(pct_row(f"`s0-pg-off-r{r}` | {name}", ph(d, name)))
    print()
    print("### E. KDS run-1 percentiles of the same phases (from the 9a0525d archive)\n")
    print(PCT_HDR)
    for c in kds_cells:
        d = kt[c][0]
        for name in ("txn", "trade-insert", "account-update", "profit-scan", "profit-insert"):
            print(pct_row(f"`s0-{c}` | {name}", ph(d, name)))
    print()
    print("### F. wait shares of the business transaction (phase mean x ops over txn mean x ops; run 1)\n")
    print("| cell | trade-insert | account-update | unattributed (client, socket, between statements) | txn mean |")
    print("|---|---|---|---|---|")

    def shares(label, d):
        t = ph(d, "txn"); a = ph(d, "trade-insert"); b = ph(d, "account-update")
        tot = t["mean_us"] * t["ops"]
        sa = a["mean_us"] * a["ops"] / tot; sb = b["mean_us"] * b["ops"] / tot
        print(f"| {label} | {100*sa:.1f} % | {100*sb:.1f} % | {100*(1-sa-sb):.1f} % | {t['mean_us']:,.0f} µs |")
    shares("`s0-pg-on-r1`", pg["on"][0])
    shares("`s0-pg-off-r1` (non-durable)", pg["off"][0])
    for c in kds_cells:
        shares(f"KDS `s0-{c}`", kt[c][0])
    print()
    print("### G. correctness and errors\n")
    print("| cell | committed | torn | underfunded (skipped, no statement sent) | balance verify | error replies (all phases) | driver exit |")
    print("|---|---|---|---|---|---|---|")
    for m in ("on", "off"):
        for r in (1, 2, 3):
            tag = f"s0-pg-{m}-r{r}"
            d = jl(f"{J}/{tag}.json")
            out = open(f"{L}/{tag}.stdout").read()
            uf = re.search(r"underfunded\s+(\d+)", out)
            vr = re.search(r"balance verify\s+(.*)", out)
            errs = sum(p["errors"] for p in d["phases"])
            rc = open(f"{L}/{tag}.rc").read().strip()
            print(f"| `{tag}` | {d['meta']['committed']:,} txns | {d['meta']['torn']} txns | {uf.group(1) if uf else '?'} txns | {vr.group(1).strip() if vr else '?'} | {errs} replies | exit {rc} |")
    print()
    print("### H. host stamps\n")
    print(host_table([f"s0-pg-{m}-r{r}" for r in (1, 2, 3) for m in ("on", "off")]))


# ------------------------------------------------------------------ scenario 2
S2_PH = ["booking", "commit", "freight-insert", "charge-insert", "operation-update", "org-update",
         "cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "manifest-scan"]


def s2():
    kb8 = {c: [jl(f"{KA(2)}/json/{r}s2-{c}.json") for r in REPS] for c in ("c1-g", "c8-g", "c1-s", "c8-s")}
    kb1 = {m: [jl(f"{J}/s2-kds-b1-c1-{m}-r{r}.json") for r in (1, 2, 3)] for m in ("g", "s")}
    pg = {m: [jl(f"{J}/s2-pg-{m}-r{r}.json") for r in (1, 2, 3)] for m in ("on", "off")}
    tps = lambda ds: [d["meta"]["tps"] for d in ds]

    print("### A. throughput, three runs each\n")
    print("| cell | bookers (client concurrency) | durable at the reply | run 1 | run 2 | run 3 | median | spread (max/min-1) |")
    print("|---|---|---|---|---|---|---|---|")

    def row(label, conc, dur, ds):
        v = tps(ds)
        print(f"| {label} | {conc} | {dur} | " + " | ".join(f"{x:,.1f} tps" for x in v) +
              f" | **{statistics.median(v):,.1f} tps** | {spread(v):.1f} % |")
    row("PostgreSQL 18.6 `synchronous_commit = on`", "1 connection", "yes", pg["on"])
    row("PostgreSQL 18.6 `synchronous_commit = off`", "1 connection", "**no (non-durable)**", pg["off"])
    row("KDS `cores = 1`, group, `--bookers 1` (this run)", "1 booker", "yes", kb1["g"])
    row("KDS `cores = 1`, strict, `--bookers 1` (this run)", "1 booker", "yes", kb1["s"])
    for c, nm in (("c1-g", "cores 1, group"), ("c8-g", "cores 8, group"), ("c1-s", "cores 1, strict"), ("c8-s", "cores 8, strict")):
        row(f"KDS `s2-{c}` ({nm}), 9a0525d archive", "8 bookers, contended", "yes", kb8[c])
    print()
    pm = statistics.median(tps(pg["on"]))
    print("### B. ratios against PostgreSQL `on` (median tps)\n")
    print("| KDS cell | KDS median | PG-on median | KDS / PG-on | like-for-like? |")
    print("|---|---|---|---|---|")
    for lab, ds, like in (("`--bookers 1` cores 1 group", kb1["g"], "yes: 1 client, durable, same work"),
                          ("`--bookers 1` cores 1 strict", kb1["s"], "yes: 1 client, durable, same work"),
                          ("`s2-c1-g` 8 bookers", kb8["c1-g"], "no: 8 clients vs 1"),
                          ("`s2-c8-g` 8 bookers", kb8["c8-g"], "no: 8 clients vs 1"),
                          ("`s2-c1-s` 8 bookers", kb8["c1-s"], "no: 8 clients vs 1"),
                          ("`s2-c8-s` 8 bookers", kb8["c8-s"], "no: 8 clients vs 1")):
        m = statistics.median(tps(ds))
        print(f"| {lab} | {m:,.1f} tps | {pm:,.1f} tps | {m/pm:.2f}x | {like} |")
    print()
    print("### C. percentiles (all three runs)\n")
    print(PCT_HDR)
    for tagm, ds in (("s2-pg-on", pg["on"]), ("s2-pg-off", pg["off"]),
                     ("s2-kds-b1-c1-g", kb1["g"]), ("s2-kds-b1-c1-s", kb1["s"])):
        for i, d in enumerate(ds, 1):
            for name in S2_PH:
                p = ph(d, name)
                if p:
                    print(pct_row(f"`{tagm}-r{i}` | {name}", p))
    print()
    print("### D. KDS 8-booker run-1 percentiles for booking and commit (9a0525d archive)\n")
    print(PCT_HDR)
    for c in ("c1-g", "c8-g", "c1-s", "c8-s"):
        for name in ("booking", "commit", "charge-insert", "recipe-read"):
            print(pct_row(f"`s2-{c}` | {name}", ph(kb8[c][0], name)))
    print()
    print("### E. wait shares of a booking (phase mean x ops over booking mean x ops; run 1)\n")
    names = ["cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "freight-insert", "charge-insert",
             "operation-update", "org-update", "commit"]
    print("| cell | " + " | ".join(names) + " | unattributed (BEGIN, refused attempts, client, socket) | booking mean |")
    print("|---|" + "---|" * (len(names) + 2))

    def share(label, d):
        t = ph(d, "booking"); tot = t["mean_us"] * t["ops"]
        sh = [ph(d, n)["mean_us"] * ph(d, n)["ops"] / tot for n in names]
        print(f"| {label} | " + " | ".join(f"{100*x:.1f} %" for x in sh) + f" | {100*(1-sum(sh)):.1f} % | {t['mean_us']:,.0f} µs |")
    share("`s2-pg-on-r1`", pg["on"][0])
    share("`s2-pg-off-r1` (non-durable)", pg["off"][0])
    share("`s2-kds-b1-c1-g-r1`", kb1["g"][0])
    share("`s2-kds-b1-c1-s-r1`", kb1["s"][0])
    share("KDS `s2-c8-g` (8 bookers)", kb8["c8-g"][0])
    print()
    print("### F. outcomes and the invariant check\n")
    print("| cell | committed | rejected-capacity | rejected-credit | conflicted | verify failures / checks | error replies (all phases) | driver exit |")
    print("|---|---|---|---|---|---|---|---|")
    for tag, d in ([(f"s2-pg-on-r{i}", pg["on"][i - 1]) for i in (1, 2, 3)] + [(f"s2-pg-off-r{i}", pg["off"][i - 1]) for i in (1, 2, 3)]
                   + [(f"s2-kds-b1-c1-g-r{i}", kb1["g"][i - 1]) for i in (1, 2, 3)] + [(f"s2-kds-b1-c1-s-r{i}", kb1["s"][i - 1]) for i in (1, 2, 3)]):
        m = d["meta"]; o = m["outcomes"]
        v = m.get("verify", {})
        errs = sum(p["errors"] for p in d["phases"])
        rc = open(f"{L}/{tag}.rc").read().strip()
        print(f"| `{tag}` | {o['committed']:,} bookings | {o['rejected-capacity']} bookings | {o['rejected-credit']} bookings | {o.get('conflicted', 0)} conflicts | "
              f"{v.get('failures', '?')} / {v.get('checks', '?')} checks | {errs} replies | exit {rc} |")
    print()
    print("### G. host stamps\n")
    tags = []
    for r in (1, 2, 3):
        tags += [f"s2-pg-on-r{r}", f"s2-kds-b1-c1-g-r{r}", f"s2-pg-off-r{r}", f"s2-kds-b1-c1-s-r{r}"]
    print(host_table(tags))
    print()
    print("### H. per-phase derived qps (1,000,000 / mean µs, one serial connection; median of 3 runs)\n")
    print("| phase | PG on | PG off (non-durable) | KDS c1 group b1 | KDS c1 strict b1 | KDS group / PG on | KDS strict / PG on |")
    print("|---|---|---|---|---|---|---|")
    for name in S2_PH:
        def q(ds):
            return statistics.median(1e6 / ph(d, name)["mean_us"] for d in ds)
        a, b, c, e = q(pg["on"]), q(pg["off"]), q(kb1["g"]), q(kb1["s"])
        print(f"| {name} | {a:,.0f} qps | {b:,.0f} qps | {c:,.0f} qps | {e:,.0f} qps | {c/a:.2f}x | {e/a:.2f}x |")


# ------------------------------------------------------------------ scenario 3
S3_SHAPES = ["pk-user", "loans-by-user", "loans-by-book", "resv-by-user", "books-by-author", "books-by-genre",
             "loans-by-daterange", "overdue", "join-loan-user", "join-no-literal", "exists-correlated", "count-by-user"]
NS = [200, 1000, 10000]
MODES = ["none", "all"]


def s3():
    def pgd(n, m, r):
        return jl(f"{J}/s3-pg-n{n}-{m}-r{r}.json")

    def kdd(c, n, m, rep):
        return jl(f"{KA(3)}/json/{rep}s3-c{c}-n{n}-{m}.json")

    def qps_med_pg(shape, n, m):
        return statistics.median(1e6 / ph(pgd(n, m, r), shape)["mean_us"] for r in (1, 2, 3))

    def qps_med_kd(shape, c, n, m):
        return statistics.median(1e6 / ph(kdd(c, n, m, rep), shape)["mean_us"] for rep in REPS)

    def spr_pg(shape, n, m):
        v = [1e6 / ph(pgd(n, m, r), shape)["mean_us"] for r in (1, 2, 3)]
        return spread(v)
    print("### A. PostgreSQL derived qps (1,000,000 / mean µs, median of 3 runs)\n")
    print("| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |")
    print("|---|---|---|---|---|---|---|")
    for s in S3_SHAPES:
        print(f"| {s} | " + " | ".join(f"{qps_med_pg(s, n, m):,.0f} qps" for n in NS for m in MODES) + " |")
    print()
    print("### B. KDS `cores = 1` over PostgreSQL (ratio of median qps; above 1.00x KDS is faster)\n")
    print("| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |")
    print("|---|---|---|---|---|---|---|")
    for s in S3_SHAPES:
        print(f"| {s} | " + " | ".join(f"{qps_med_kd(s, 1, n, m)/qps_med_pg(s, n, m):.2f}x" for n in NS for m in MODES) + " |")
    print()
    print("### C. KDS `cores = 8` over PostgreSQL (ratio of median qps)\n")
    print("| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |")
    print("|---|---|---|---|---|---|---|")
    for s in S3_SHAPES:
        print(f"| {s} | " + " | ".join(f"{qps_med_kd(s, 8, n, m)/qps_med_pg(s, n, m):.2f}x" for n in NS for m in MODES) + " |")
    print()
    print("### D. KDS `cores = 1` derived qps, same cells (9a0525d archive, median of 3), for reading B against\n")
    print("| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |")
    print("|---|---|---|---|---|---|---|")
    for s in S3_SHAPES:
        print(f"| {s} | " + " | ".join(f"{qps_med_kd(s, 1, n, m):,.0f} qps" for n in NS for m in MODES) + " |")
    print()
    print("### E. what the index buys inside each engine (none-qps over all-qps inverted: all / none, median qps)\n")
    print("| shape | PG n=200 | PG n=1,000 | PG n=10,000 | KDS c1 n=200 | KDS c1 n=1,000 | KDS c1 n=10,000 |")
    print("|---|---|---|---|---|---|---|")
    for s in S3_SHAPES:
        a = [qps_med_pg(s, n, "all") / qps_med_pg(s, n, "none") for n in NS]
        b = [qps_med_kd(s, 1, n, "all") / qps_med_kd(s, 1, n, "none") for n in NS]
        print(f"| {s} | " + " | ".join(f"{x:.2f}x" for x in a + b) + " |")
    print()
    print("### F. noise floor: three-run spread of PG qps (max/min-1)\n")
    print("| shape | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |")
    print("|---|---|---|---|---|---|---|")
    for s in S3_SHAPES:
        print(f"| {s} | " + " | ".join(f"{spr_pg(s, n, m):.1f} %" for n in NS for m in MODES) + " |")
    print()
    print("### G. PostgreSQL percentiles, run 1, 200 ops per shape\n")
    print("| n | mode | shape | p0 | p25 | p50 | p95 | p99 | plan (EXPLAIN top node, recorded for 5 shapes) |")
    print("|---|---|---|---|---|---|---|---|---|")
    for n in NS:
        for m in MODES:
            d = pgd(n, m, 1)
            plans = {pl.split(":")[0]: pl.split(":", 1)[1].strip().split("  (")[0] for pl in d["meta"].get("plans", [])}
            for s in S3_SHAPES:
                p = ph(d, s)
                print(f"| {n:,} | {m} | {s} | {p['p0_us']:,.1f} µs | {p['p25_us']:,.1f} µs | {p['p50_us']:,.1f} µs | {p['p95_us']:,.1f} µs | {p['p99_us']:,.1f} µs | {plans.get(s, 'not recorded by the driver')} |")
    print()
    print("### H. setup phases, PostgreSQL run 1 (ops, mean)\n")
    print("| n | mode | ddl | load | create-index |")
    print("|---|---|---|---|---|")
    for n in NS:
        for m in MODES:
            d = pgd(n, m, 1)
            f = lambda name: (lambda p: f"{p['ops']:,} ops, mean {p['mean_us']:,.1f} µs" if p else "-")(ph(d, name))
            print(f"| {n:,} | {m} | {f('ddl')} | {f('load')} | {f('create-index')} |")
    print()
    print("### I. host stamps\n")
    tags = [f"s3-pg-n{n}-{m}-r{r}" for r in (1, 2, 3) for n in NS for m in MODES]
    print(host_table(tags))
    print()
    print("### J. fixed and per-row cost of a walked equality (none mode; two-point fit, n=200 and n=10,000, loans scanned: rows = n)\n")
    print("| shape | PG fixed | PG per row | KDS c1 fixed | KDS c1 per row | KDS / PG per-row cost |")
    print("|---|---|---|---|---|---|")
    import math

    def mean_pg(s, n):
        return statistics.median(ph(pgd(n, "none", r), s)["mean_us"] for r in (1, 2, 3))

    def mean_kd(s, n):
        return statistics.median(ph(kdd(1, n, "none", rep), s)["mean_us"] for rep in REPS)
    for s in ("loans-by-user", "loans-by-book", "count-by-user", "loans-by-daterange", "overdue"):
        res = []
        for f in (mean_pg, mean_kd):
            a, b = f(s, 200), f(s, 10000)
            slope = (b - a) / 9800 * 1000  # ns per row
            fixed = a - slope * 200 / 1000
            res.append((fixed, slope))
        print(f"| {s} | {res[0][0]:,.1f} µs | {res[0][1]:,.1f} ns/row | {res[1][0]:,.1f} µs | {res[1][1]:,.1f} ns/row | {res[1][1]/res[0][1]:.2f}x |")
    print()
    print("### K. geometric mean of the KDS/PG qps ratio over the 12 shapes, per cell (and shapes where KDS is ahead by more than 1.05x)\n")
    print("| KDS cores | n=200 none | n=200 all | n=1,000 none | n=1,000 all | n=10,000 none | n=10,000 all |")
    print("|---|---|---|---|---|---|---|")
    for c in (1, 8):
        cells = []
        for n in NS:
            for m in MODES:
                rs = [qps_med_kd(s, c, n, m) / qps_med_pg(s, n, m) for s in S3_SHAPES]
                gm = math.exp(sum(math.log(x) for x in rs) / len(rs))
                cells.append(f"{gm:.2f}x ({sum(1 for x in rs if x > 1.05)} of 12 ahead)")
        print(f"| {c} | " + " | ".join(cells) + " |")


# ------------------------------------------------------------------ scenario 4
S4_GROUPS = [("open", "d1-open"), ("board", "d1-board"), ("tape200", "d1-tape200"), ("tape1k", "d1-tape1k"),
             ("tape10k", "d1-tape10k"), ("pk", "d1-pk"), ("close", "d1-close")]


def s4():
    pg = [jl(f"{J}/s4-pg-r{r}.json") for r in (1, 2, 3)]
    ka = jl(f"{KA(4)}/json/s4.json")
    kb = jl(f"{KA(4)}/json/cd2-s4.json")

    def pgp(g, d):
        return ph(d, f"{g}[pg]")

    def kp(g, arm, d=ka):
        return ph(d, f"{g}[{arm}]")
    print("### A. day-1 statement shapes, derived qps (1,000,000 / mean µs; PG median of 3 runs, KDS run A day 1)\n")
    print("| group | PG (no index) | KDS off (walks) | KDS on (controller) | KDS declared (Cabin) | KDS off / PG | KDS on / PG | KDS declared / PG |")
    print("|---|---|---|---|---|---|---|---|")
    for name, g in S4_GROUPS:
        pq = statistics.median(1e6 / pgp(g, d)["mean_us"] for d in pg)
        vals = [1e6 / kp(g, a)["mean_us"] for a in ("off", "on", "declared")]
        print(f"| {name} | {pq:,.0f} qps | " + " | ".join(f"{v:,.0f} qps" for v in vals) + " | " + " | ".join(f"{v/pq:.2f}x" for v in vals) + " |")
    print()
    print("### B. PostgreSQL noise floor: three-run spread of derived qps\n")
    print("| group | run 1 | run 2 | run 3 | spread (max/min-1) |")
    print("|---|---|---|---|---|")
    for name, g in S4_GROUPS:
        v = [1e6 / pgp(g, d)["mean_us"] for d in pg]
        print(f"| {name} | " + " | ".join(f"{x:,.0f} qps" for x in v) + f" | {spread(v):.1f} % |")
    print()
    print("### C. PostgreSQL percentiles, all three runs\n")
    print(PCT_HDR)
    for i, d in enumerate(pg, 1):
        for p in d["phases"]:
            print(pct_row(f"`s4-pg-r{i}` | {p['phase']}", p))
    print()
    print("### D. KDS day-1 percentiles, run A (9a0525d archive)\n")
    print(PCT_HDR)
    for name, g in S4_GROUPS:
        for a in ("off", "on", "declared"):
            print(pct_row(f"KDS `{a}` | {g}[{a}]", kp(g, a)))
    print()
    print("### E. errors and host stamps\n")
    print("| cell | client errors | driver exit | elapsed |")
    print("|---|---|---|---|")
    for i, d in enumerate(pg, 1):
        m = d["meta"]
        print(f"| `s4-pg-r{i}` | {m['client_errors']} errors | exit {open(f'{L}/s4-pg-r{i}.rc').read().strip()} | {m['elapsed_s']} s |")
    print()
    print(host_table([f"s4-pg-r{i}" for i in (1, 2, 3)]))


def s1():
    d = jl(f"{J}/s1-pg-r1.json")
    m = d["meta"]
    print("### A. phase percentiles (run 1, the only run)\n")
    print(PCT_HDR)
    for p in d["phases"]:
        print(pct_row(f"`s1-pg-r1` | {p['phase']}", p))
    print()
    print("### B. QPS by read shape and case (driver-measured, 100 statements per cell; the accelerator is a btree index)\n")
    print("| shape | detail | cold | warm | index built | index dropped |")
    print("|---|---|---|---|---|---|")
    for r in m["qps_sweep"]:
        f = lambda c: f"{r[c]['qps']:,.1f} qps (p50 {r[c]['p50_us']:,.1f} µs)" if r.get(c) else "not applicable"
        print(f"| {r['shape']} | {r['detail']} | {f('cold')} | {f('warm')} | {f('cabin')} | {f('dropped')} |")
    print()
    print("### C. INSERT qps by transaction batch size (2,000 rows per batch size)\n")
    print("| rows per BEGIN/COMMIT | qps | errors |")
    print("|---|---|---|")
    for r in m["write_sweep"]:
        print(f"| {r['batch']} | {r['qps']:,.1f} qps | {r['errors']} errors |")
    print()
    print("### D. qps by connection count (the 3-relation join, 200 statements spread across the connections)\n")
    print("| connections | qps | errors |")
    print("|---|---|---|")
    for r in m["concurrency_sweep"]:
        print(f"| {r['connections']} | {r['qps']:,.1f} qps | {r['errors']} errors |")
    print()
    print("### E. host stamp\n")
    print(host_table(["s1-pg-r1"]))


if __name__ == "__main__":
    {"s0": s0, "s2": s2, "s3": s3, "s4": s4, "s1": s1}[sys.argv[1]]()
