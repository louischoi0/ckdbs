import sys, json
sys.path.insert(0, "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e/analysis")
from docs_common import *

CELLS = ["c1-g", "c8-g", "c1-s", "c8-s"]
DUR = {"g": "group", "s": "strict"}
REPS = ["", "r2-", "r3-"]
PHASES = ["booking", "commit", "freight-insert", "charge-insert", "operation-update", "org-update",
          "cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "manifest-scan"]
SHARE_NAMES = ["cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "freight-insert", "charge-insert",
               "operation-update", "org-update", "commit"]
PGD = f"{OLDDIR}/archive/scenario2-pg18-v2.7.0-545-gf2f1ee7/json"


def tpsl(c, old=None, pre=""):
    return [load(f"{r}{pre}s2-{c}", old)["meta"]["tps"] for r in REPS]


med = {c: statistics.median(tpsl(c)) for c in CELLS}
omed = {c: statistics.median(tpsl(c, 2)) for c in CELLS}
spr = {c: 100 * (max(tpsl(c)) / min(tpsl(c)) - 1) for c in CELLS}
ospr = {c: 100 * (max(tpsl(c, 2)) / min(tpsl(c, 2)) - 1) for c in CELLS}


def pct_table(rep, phases, cells=CELLS):
    out = ["| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |", "|---|---|---|---|---|---|---|---|---|---|"]
    for c in cells:
        d = load(f"{rep}s2-{c}")
        for n in phases:
            p = ph(d, n)
            if p:
                out.append(f"| `{rep}s2-{c}` | {n} | {pct_row(p)} |")
    return "\n".join(out)


def share_row(d):
    t = ph(d, "booking")
    tot = t["mean_us"] * t["ops"]
    sh = [ph(d, n)["mean_us"] * ph(d, n)["ops"] / tot for n in SHARE_NAMES]
    return sh, 1 - sum(sh), t["mean_us"]


# ---------- tables ----------
T_tps = ["| cell | cores | durability | run 1 | run 2 | run 3 | median | spread (max/min-1) | 9a0525d median (cross-version) | 9a0525d spread | delta of medians (cross-version) |",
         "|---|---|---|---|---|---|---|---|---|---|---|"]
for c in CELLS:
    v = tpsl(c)
    T_tps.append(f"| `s2-{c}` | {c[1]} | {DUR[c[-1]]} | " + " | ".join(f"{x:,.1f} tps" for x in v) +
                 f" | **{med[c]:,.1f} tps** | {spr[c]:.1f} % | {omed[c]:,.1f} tps | {ospr[c]:.1f} % | {100*(med[c]/omed[c]-1):+.1f} % |")
T_tps = "\n".join(T_tps)

T_shares = ["| cell | " + " | ".join(SHARE_NAMES) + " | unattributed (BEGIN, refused attempts, client, socket) | booking mean |",
            "|---|" + "---|" * (len(SHARE_NAMES) + 2)]
for c in CELLS:
    for lab, d in (("now", load(f"s2-{c}")), ("9a0525d", load(f"s2-{c}", 2))):
        sh, un, bm = share_row(d)
        T_shares.append(f"| `s2-{c}` ({lab}) | " + " | ".join(f"{100*x:.1f} %" for x in sh) + f" | {100*un:.1f} % | {bm:,.0f} µs |")
T_shares = "\n".join(T_shares)

T_x = ["| cell | phase | p0 now | p0 9a0525d | p50 now | p50 9a0525d | p50 ratio now/then | mean now | mean 9a0525d |", "|---|---|---|---|---|---|---|---|---|"]
for c in CELLS:
    d, o = load(f"s2-{c}"), load(f"s2-{c}", 2)
    for n in ["booking", "commit", "freight-insert", "charge-insert", "operation-update", "cargo-lookup", "recipe-read"]:
        p, q = ph(d, n), ph(o, n)
        T_x.append(f"| `s2-{c}` | {n} | {fmt_us(p['p0_us'])} | {fmt_us(q['p0_us'])} | {fmt_us(p['p50_us'])} | {fmt_us(q['p50_us'])} | {p['p50_us']/q['p50_us']:.2f}x | {fmt_us(p['mean_us'])} | {fmt_us(q['mean_us'])} |")
T_x = "\n".join(T_x)

T_out = ["| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | axes (op / org / read / commit) | verify failures / checks | error replies (all phases) | exit |",
         "|---|---|---|---|---|---|---|---|---|---|---|"]
for ri, rep in enumerate(REPS):
    for c in CELLS:
        tag = f"{rep}s2-{c}"
        d = load(tag)
        m = d["meta"]
        o, a = m["outcomes"], m["axes"]
        errs = sum(p["errors"] for p in d["phases"])
        rc = open(f"{L}/{tag}.rc").read().strip()
        T_out.append(f"| `{tag}` | {ri+1} | {o['committed']:,} bookings | {o['rejected-capacity']} bookings | {o['rejected-credit']} bookings | {o['conflicted']} conflicts | {m['abandoned']} bookings | {a['operations']} / {a['organizations']} / {a['read']} / {a['commit']} conflicts | {m['verify']['failures']} / {m['verify']['checks']} checks | {errs} replies | exit {rc} |")
T_out = "\n".join(T_out)

T_srv = ["| cell | run 1 | run 2 | run 3 |", "|---|---|---|---|"]
for c in CELLS:
    row = []
    for r in REPS:
        txt = open(f"{L}/{r}s2-{c}.serverlog").read().split("\n")
        row.append(f"{sum('TXN_CONFLICT' in x for x in txt)} conflict lines ({sum('row id=' in x for x in txt)} row, {sum('btree descent' in x and 'gave up' in x for x in txt)} descent)")
    T_srv.append(f"| `s2-{c}` | " + " | ".join(row) + " |")
T_srv = "\n".join(T_srv)

T_rr = ["| cell | isolation | tps | committed | conflicted (retried) | axes (op / org) | verify failures / checks |", "|---|---|---|---|---|---|---|"]
for c in ("c1-g", "c8-g"):
    for lab, d in (("rr", load(f"rr-s2-{c}")),):
        m = d["meta"]
        T_rr.append(f"| `rr-s2-{c}` | repeatable-read | {m['tps']:,.1f} tps | {m['outcomes']['committed']:,} bookings | {m['outcomes']['conflicted']} conflicts | {m['axes']['operations']} / {m['axes']['organizations']} conflicts | **{m['verify']['failures']} / {m['verify']['checks']} checks** |")
    v = [load(f"{r}s2-{c}")["meta"] for r in REPS]
    T_rr.append(f"| `s2-{c}` (median of 3) | server default (read-committed) | {med[c]:,.1f} tps | 3,000 bookings | " +
                ", ".join(str(x['outcomes']['conflicted']) for x in v) + " conflicts | - | " + ", ".join(str(x['verify']['failures']) for x in v) + " / 400 checks |")
T_rr = "\n".join(T_rr)
T_rrp = ["| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |", "|---|---|---|---|---|---|---|---|---|---|"]
for c in ("c1-g", "c8-g"):
    d = load(f"rr-s2-{c}")
    for n in ("booking", "commit", "operation-update", "org-update"):
        T_rrp.append(f"| `rr-s2-{c}` | {n} | {pct_row(ph(d, n))} |")
T_rrp = "\n".join(T_rrp)

# matched single-booker cells vs PG (reused)
pg = [json.load(open(f"{PGD}/s2-pg-on-r{i}.json")) for i in (1, 2, 3)]
oldg = [json.load(open(f"{PGD}/s2-kds-b1-c1-g-r{i}.json")) for i in (1, 2, 3)]
olds = [json.load(open(f"{PGD}/s2-kds-b1-c1-s-r{i}.json")) for i in (1, 2, 3)]
newg = [load(f"{r}b1-s2-c1-g") for r in REPS]
news = [load(f"{r}b1-s2-c1-s") for r in REPS]
pgt = [x["meta"]["tps"] for x in pg]


def mt(ds):
    v = [x["meta"]["tps"] for x in ds]
    return statistics.median(v), 100 * (max(v) / min(v) - 1)


T_b1 = ["| engine / cell | clients | durability | run 1 | run 2 | run 3 | median | spread | vs PG18 |", "|---|---|---|---|---|---|---|---|---|"]
pgm = statistics.median(pgt)
T_b1.append("| PostgreSQL 18.6 `synchronous_commit = on` (reused, `f2f1ee7`, 2026-09-30) | 1 | durable | " + " | ".join(f"{x:,.1f} tps" for x in pgt) + f" | **{pgm:,.1f} tps** | {100*(max(pgt)/min(pgt)-1):.1f} % | 1.00x |")
for lab, ds, dur in (("KDS `cores = 1`, `--bookers 1`, this run", newg, "group"), ("KDS `cores = 1`, `--bookers 1`, this run", news, "strict"),
                     ("KDS `cores = 1`, `--bookers 1`, 9a0525d binary (reused from the PG18 file)", oldg, "group"),
                     ("KDS `cores = 1`, `--bookers 1`, 9a0525d binary (reused from the PG18 file)", olds, "strict")):
    v = [x["meta"]["tps"] for x in ds]
    m, s = mt(ds)
    T_b1.append(f"| {lab} | 1 | {dur} | " + " | ".join(f"{x:,.1f} tps" for x in v) + f" | **{m:,.1f} tps** | {s:.1f} % | {m/pgm:.2f}x |")
T_b1 = "\n".join(T_b1)

T_b1p = ["| phase | PG18 p50 (reused) | KDS group p50 now | KDS group p50 9a0525d | KDS strict p50 now | KDS strict p50 9a0525d | KDS group mean now | PG18 mean |", "|---|---|---|---|---|---|---|---|"]
for n in ["booking", "commit", "freight-insert", "charge-insert", "operation-update", "org-update", "cargo-lookup", "credit-lookup", "capacity-read", "recipe-read"]:
    def md(ds, k="p50_us"):
        return statistics.median([ph(d, n)[k] for d in ds])
    T_b1p.append(f"| {n} | {fmt_us(md(pg))} | {fmt_us(md(newg))} | {fmt_us(md(oldg))} | {fmt_us(md(news))} | {fmt_us(md(olds))} | {fmt_us(md(newg, 'mean_us'))} | {fmt_us(md(pg, 'mean_us'))} |")
T_b1p = "\n".join(T_b1p)

T_b1pct = ["| cell | phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |", "|---|---|---|---|---|---|---|---|---|---|"]
for c in ("c1-g", "c1-s"):
    d = load(f"b1-s2-{c}")
    for n in ["booking", "commit", "freight-insert", "charge-insert", "operation-update", "org-update", "cargo-lookup", "credit-lookup", "capacity-read", "recipe-read", "manifest-scan"]:
        p = ph(d, n)
        if p:
            T_b1pct.append(f"| `b1-s2-{c}` | {n} | {pct_row(p)} |")
T_b1pct = "\n".join(T_b1pct)

sh_b1 = []
for d in newg + news:
    t = ph(d, "booking")
    sh_b1.append(ph(d, "commit")["mean_us"] * ph(d, "commit")["ops"] / (t["mean_us"] * t["ops"]))
pg_sh = [ph(d, "commit")["mean_us"] * ph(d, "commit")["ops"] / (ph(d, "booking")["mean_us"] * ph(d, "booking")["ops"]) for d in pg]

def _md(ds, n, k="p50_us"):
    return statistics.median([ph(d, n)[k] for d in ds])


rat_w = [_md(newg, n) / _md(pg, n) for n in ("freight-insert", "charge-insert", "operation-update", "org-update")]
rat_r = [_md(newg, n) / _md(pg, n) for n in ("cargo-lookup", "credit-lookup", "capacity-read")]
rat_rec = _md(newg, "recipe-read") / _md(pg, "recipe-read")
stm = [_md(newg, n) / _md(oldg, n) for n in ("freight-insert", "charge-insert", "operation-update", "org-update", "cargo-lookup", "credit-lookup", "capacity-read", "recipe-read")]

# host stamps
tags = [f"{r}s2-{c}" for r in REPS for c in CELLS] + ["rr-s2-c1-g", "rr-s2-c8-g"] + [f"{r}b1-s2-{c}" for r in REPS for c in ("c1-g", "c1-s")]
T_host = ["| cell | window UTC | loadavg 1/5/15 at precheck | gate wait | build/test processes at precheck | monitor samples | with build/test | with foreign >= 50 % | with minor 10-50 % |", "|---|---|---|---|---|---|---|---|---|"]
for (t, tm, e, la, gw, pr, n, b, h, mi) in gate_tbl(tags):
    T_host.append(f"| `{t}` | {tm[:10]} {tm[11:19]} to {e[11:19]} | {la} | {gw} s | {pr} | {n} samples | {b} | {h} | {mi} |")
T_host = "\n".join(T_host)

therm = [ph(load(t), "load-cargos")["p50_us"] for t in tags]

doc = f"""# Scenario 2 (freight) at `{DESC}`

**One line.** The eight-statement freight booking runs faster at every
core count and durability than at `9a0525d`, and the commit path no longer
bounds `strict`: eight contended bookers at `cores = 8` run
{med['c8-g']:,.1f} tps under `group` and {med['c8-s']:,.1f} tps under `strict` (medians of three,
spreads {spr['c8-g']:.1f} % and {spr['c8-s']:.1f} %), against {omed['c8-g']:,.1f} and {omed['c8-s']:,.1f} tps in the
comparator (cross-version, +{100*(med['c8-g']/omed['c8-g']-1):.1f} % and +{100*(med['c8-s']/omed['c8-s']-1):.1f} %), and `cores = 1` is
{med['c1-g']:,.1f} tps (`group`, +{100*(med['c1-g']/omed['c1-g']-1):.1f} %) and {med['c1-s']:,.1f} tps (`strict`, +{100*(med['c1-s']/omed['c1-s']-1):.1f} %).
What did **not** move is the single durable client: one booker is
{mt(newg)[0]:,.1f} tps (`group`) and {mt(news)[0]:,.1f} tps (`strict`), the same as the comparator's
{mt(oldg)[0]:,.1f} and {mt(olds)[0]:,.1f} tps, with the commit fsync {100*statistics.median(sh_b1):.0f} % of every booking. The
invariant check still fails under the default isolation (13 to 34 of 400)
and passes 400 of 400 under `--isolation repeatable-read`, as at the
comparator.

Driver: `tools/scenario2_freight.py`, unmodified, `--schema-only`, then
`--load-only`, then the run on the same server, with the 2026-09-30
rebaseline's arguments and cell order. Measured by `ck-tester`; the
operator lifted the time budget for this run. Comparator (cross-version):
`bench/v3.0.0/results-scenario2-freight-v2.7.0-531-g9a0525d.md`, held at
`36449d28`. PostgreSQL 18.6 figures are **reused, not re-measured**: from
`results-scenario2-freight-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`,
measured 2026-09-30, **14 days before this run**).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-10-13 23:38 UTC to 2026-10-14 00:48 UTC. Runs 1 and 2 of the four cells ran 23:38 to 23:41; run 3 was delayed by another session's builds and `ctest` and its cells ran 23:58 to 00:08 (three of them re-run, see section 9); the isolation probe 23:52 to 23:53; the matched single-booker cells 00:44 to 00:48. Per-cell windows: section 9 |
{COMMON_STAMP}
| Server config | `log_level = warn`, **`buffer_pool_frames = 65536`** (required since BE; the comparator's config predates the key and ran without it), `cores` and `durability` per cell, all else default. Default `isolation` (read-committed; the driver reports `server default`) except the probe in section 6. Ports 15604 to 15607, never 15432 |
{HOST_ROWS}
| PostgreSQL | **reused, not re-measured**: PostgreSQL 18.6 at its defaults, one booking connection, `results-scenario2-freight-pg18-v2.7.0-545-gf2f1ee7.md` (held at `36449d28`), measured 2026-09-30 at `f2f1ee7`. Section 8 |

## 2. What was run

The four cells of the comparator's matrix, **three runs each (twelve
cells)**, plus two `--isolation repeatable-read` probe cells (one run each),
plus six extra cells that are not in the comparator's matrix: `--bookers 1`
at `cores = 1`, `group` and `strict`, three runs each, because the PostgreSQL
twin drives one booking connection and the 8-booker cells have no matched
PostgreSQL counterpart (section 8). Arguments: `--organizations 300 --ships 30
--operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1
--sync`, with `--txn`, `--contend` and `--manifest` at their defaults (on) and
`--capacity-mode cached`. A run is 3,000 committed bookings, 375 per booker,
contended on 6 hot routes; the eight relations hold 24,644 to 24,664 rows at
the end of a run.

Cell order within a run: `c8-s`, `c1-s`, `c1-g`, `c8-g` (the comparator's
order). Commands (`run_all3.sh` in the archive; `$P` is the run prefix):

    python3 tools/scenario2_freight.py --port <p> --suffix <s> $S2_ARGS --schema-only
    python3 tools/scenario2_freight.py --port <p> --suffix <s> $S2_ARGS --load-only
    python3 tools/scenario2_freight.py --port <p> --suffix <s> $S2_ARGS --json <cell>.json

Rule 9's 200 / 1K / 10K sweep is **not executed** for this driver, as in the
comparator: the measured unit (an eight-statement booking on a pk-addressed
voyage and customer) does not scale with the row count of the ledgers it
appends to, and the mapping from flags to rows is fixed (`--organizations 300`,
`--cargos 4000`, `--bookings 3000` give the 24.6 k rows above). Rows loaded
per run are in the driver's JSON `meta.loaded`.

Load and build check per cell: section 9. **No reported cell has a build or
test process in its window.**

## 3. Throughput

Eight contended bookers: throughput is up in every cell, and the one that
moved most is the one the comparator could not read. `strict` at eight cores
is {med['c8-s']:,.1f} tps with a {spr['c8-s']:.1f} % spread, where the comparator's median
({omed['c8-s']:,.1f} tps) carried a {ospr['c8-s']:.1f} % spread and "is the least reliable number in
this file".

{T_tps}

| ratio of medians | now | 9a0525d (cross-version) |
|---|---|---|
| `cores = 8` over `cores = 1`, `group` | {med['c8-g']/med['c1-g']:.2f}x | {omed['c8-g']/omed['c1-g']:.2f}x |
| `cores = 8` over `cores = 1`, `strict` | {med['c8-s']/med['c1-s']:.2f}x | {omed['c8-s']/omed['c1-s']:.2f}x |
| `group` over `strict`, `cores = 1` | {med['c1-g']/med['c1-s']:.2f}x | {omed['c1-g']/omed['c1-s']:.2f}x |
| `group` over `strict`, `cores = 8` | {med['c8-g']/med['c8-s']:.2f}x | {omed['c8-g']/omed['c8-s']:.2f}x |

Every cell committed its 3,000 bookings (`abandoned` bookings: section 5).
**Cores help as they did, and durability now costs little extra at eight
cores**: `group` over `strict` fell from {omed['c8-g']/omed['c8-s']:.2f}x to {med['c8-g']/med['c8-s']:.2f}x. At eight cores `strict`
runs {100*med['c8-s']/med['c8-g']:.0f} % of `group`'s rate; the comparator had it at {100*omed['c8-s']/omed['c8-g']:.0f} %.

## 4. Percentiles

The booking (the eight-statement transaction, client-perceived) and its
constituent phases. `ops` is the phase's count; `booking` exceeds 3,000
because a rejected or conflicted attempt opens and closes a span. Run 1 in full:

{pct_table('', PHASES)}

Runs 2 and 3, booking, commit and the two appends only (the rest is in the
archive's JSON):

{pct_table('r2-', ['booking', 'commit', 'freight-insert', 'charge-insert'])}

{pct_table('r3-', ['booking', 'commit', 'freight-insert', 'charge-insert'])}

**One core queues; eight cores mostly do not.** Statement p0 is the same
at both core counts (`cargo-lookup` {ph(load('s2-c1-g'),'cargo-lookup')['p0_us']:.1f} µs at `s2-c1-g`, {ph(load('s2-c8-g'),'cargo-lookup')['p0_us']:.1f} µs at
`s2-c8-g`; `freight-insert` {ph(load('s2-c1-g'),'freight-insert')['p0_us']:.1f} and {ph(load('s2-c8-g'),'freight-insert')['p0_us']:.1f} µs) but p50 falls going from
one core to eight: `booking` {ph(load('s2-c1-g'),'booking')['p50_us']/ph(load('s2-c8-g'),'booking')['p50_us']:.2f}x
({ph(load('s2-c1-g'),'booking')['p50_us']:,.1f} to {ph(load('s2-c8-g'),'booking')['p50_us']:,.1f} µs), the four reads {min(ph(load('s2-c1-g'),n)['p50_us']/ph(load('s2-c8-g'),n)['p50_us'] for n in ('cargo-lookup','credit-lookup','capacity-read','recipe-read')):.2f}x to {max(ph(load('s2-c1-g'),n)['p50_us']/ph(load('s2-c8-g'),n)['p50_us'] for n in ('cargo-lookup','credit-lookup','capacity-read','recipe-read')):.2f}x,
`charge-insert` {ph(load('s2-c1-g'),'charge-insert')['p50_us']/ph(load('s2-c8-g'),'charge-insert')['p50_us']:.2f}x and **`freight-insert` {ph(load('s2-c1-g'),'freight-insert')['p50_us']/ph(load('s2-c8-g'),'freight-insert')['p50_us']:.1f}x**
({ph(load('s2-c1-g'),'freight-insert')['p50_us']:,.1f} to {ph(load('s2-c8-g'),'freight-insert')['p50_us']:,.1f} µs). The difference at equal p0 is the wait behind
seven other sessions on the one reactor, not the statement. `commit` goes
the other way: p50 {ph(load('s2-c1-g'),'commit')['p50_us']:,.1f} µs at one core, {ph(load('s2-c8-g'),'commit')['p50_us']:,.1f} µs at eight under `group`. Why
`freight-insert`, the first write of a booking, falls furthest is not
explained by this run (the comparator said the same of its 10.0x).

## 5. Where the time goes

A booking is the sum of its statements, its commit, and what is left (BEGIN,
refused attempts, the client and the socket). Each phase's mean times its ops
over the booking's mean times its ops, run 1, now and at the comparator:

{T_shares}

| Wait | Reading |
|---|---|
| Durability / commit | `commit` is {100*share_row(load('s2-c1-g'))[0][-1]:.1f} % of a booking at `s2-c1-g`, **{100*share_row(load('s2-c8-g'))[0][-1]:.1f} % at `s2-c8-g`** and {100*share_row(load('s2-c8-s'))[0][-1]:.1f} % at `s2-c8-s` (comparator {100*share_row(load('s2-c8-g',2))[0][-1]:.1f} % and {100*share_row(load('s2-c8-s',2))[0][-1]:.1f} %). At eight cores the commit, not the statements, bounds the booking: its p50 rises {ph(load('s2-c8-g'),'commit')['p50_us']/ph(load('s2-c1-g'),'commit')['p50_us']:.2f}x going from one core to eight while every statement's p50 falls. With one booker (section 8) the commit is {100*statistics.median(sh_b1):.0f} % and its p50 {ph(load('b1-s2-c1-g'),'commit')['p50_us']:,.1f} µs equals PostgreSQL's {statistics.median([ph(d,'commit')['p50_us'] for d in pg]):,.1f} µs, which is the device's sync. A `group` and a `strict` commit now cost the same at eight cores (p50 {ph(load('s2-c8-g'),'commit')['p50_us']:,.1f} and {ph(load('s2-c8-s'),'commit')['p50_us']:,.1f} µs) |
| Write statements (`freight-insert`, `charge-insert`, `operation-update`, `org-update`) | one core: {100*share_row(load('s2-c1-g'))[0][5]:.1f} % for `charge-insert` alone (about 5.6 charges per booking) and {100*share_row(load('s2-c1-g'))[0][4]:.1f} / {100*share_row(load('s2-c1-g'))[0][6]:.1f} / {100*share_row(load('s2-c1-g'))[0][7]:.1f} % for the three others; eight cores {100*share_row(load('s2-c8-g'))[0][5]:.1f} / {100*share_row(load('s2-c8-g'))[0][4]:.1f} / {100*share_row(load('s2-c8-g'))[0][6]:.1f} / {100*share_row(load('s2-c8-g'))[0][7]:.1f} % |
| Reads (`cargo-lookup`, `credit-lookup`, `capacity-read`, `recipe-read`) | {100*share_row(load('s2-c1-g'))[0][0]:.1f} to {100*share_row(load('s2-c1-g'))[0][3]:.1f} % each at one core, {100*share_row(load('s2-c8-g'))[0][0]:.1f} to {100*share_row(load('s2-c8-g'))[0][3]:.1f} % at eight; `recipe-read` is the FilterScan and the largest |
| Lock or conflict wait | zero client-visible conflicts at any `cores = 1` cell and at `c8-g` 0 / 0 / 2 (the two in run 3 are held rows); **90 / 63 / 45 at `c8-s`** (`TXN_CONFLICT retryable=1 row id=... was written by transaction ...`, all on `operations` and `organizations` rows; comparator 125 / 50 / 77), all retried by the driver. The server log also records conflict lines that never reached the driver (table below): a statement that parks is re-run after the holder decides. Their time is inside the phase that met them and is not separable. The `unattributed` column of `s2-c8-s` ({100*share_row(load('s2-c8-s'))[1]:.1f} % against {100*share_row(load('s2-c8-s',2))[1]:.1f} % in the comparator) is where refused attempts land |
| Client and socket round trip | the unattributed column at the cells with no conflicts, {100*share_row(load('s2-c8-g'))[1]:.1f} to {100*share_row(load('s2-c1-s'))[1]:.1f} % |

Conflict lines in the server log (`TXN_CONFLICT`, split into held-row and
btree-descent lines). The btree re-descent refusal of a `freights` or
`charges` insert (`gave up after 5 attempts`) appears only at `cores = 8`, 2 to 6
times per cell, and **reached the driver in none of them** (no `freight-insert`
or `charge-insert` error in any of the twelve cells, and the driver counts a refused
charge insert as a conflict, so it would show). This is consistent with BA-S14 (a statement that wrote
nothing before the clustered tree refused it is run again after a yield), which this run did not
isolate; the server still logs the refusal. In the comparator the refusal reached the driver
three times in two of three `c8-g` runs.

{T_srv}

Outcomes, conflicts and the invariant check, every cell:

{T_out}

## 6. Correctness: what the invariant failures are

`--verify 100` runs 400 invariant checks per cell (I1 booked capacity against
the sum of freight volumes, I2 within ship capacity, I3 outstanding against
recomputed charges, I4 charge rows against rules). All twelve default-isolation
cells report failures, 13 to 34 of 400, **including at `cores = 1` where no two
statements execute at once**. The driver writes `SET booked_cbm = <literal>`
from a value it read earlier in the same transaction and says in its source
that this "is exactly the lost-update shape": two bookers that read one voyage
before either commits both write `read + their cbm`, and the second overwrites
the first. The probe cells run the same workload under `--isolation
repeatable-read`:

{T_rr}

Percentiles of the probe cells:

{T_rrp}

**Finding.** The failures are the read-committed lost update of a client-side
read-modify-write, not corruption of the engine: under repeatable-read the
engine refuses the second writer of a row (`TXN_CONFLICT`, {load('rr-s2-c1-g')['meta']['outcomes']['conflicted']} at one core,
{load('rr-s2-c8-g')['meta']['outcomes']['conflicted']} at eight), the driver retries, and the check passes 400 of 400 in both probes.
The price of the correct answer is {100*(load('rr-s2-c1-g')['meta']['tps']/med['c1-g']-1):+.1f} % at one core ({load('rr-s2-c1-g')['meta']['tps']:,.1f} against {med['c1-g']:,.1f} tps, inside
the {spr['c1-g']:.1f} % run-to-run spread) and **{100*(load('rr-s2-c8-g')['meta']['tps']/med['c8-g']-1):+.1f} % at eight cores** ({load('rr-s2-c8-g')['meta']['tps']:,.1f} against {med['c8-g']:,.1f} tps, outside the {spr['c8-g']:.1f} %
spread, one probe run). The comparator's probes showed no cost (+0.8 % and +4.0 %);
this run's eight-core probe does, once. `docs/spec/txn.md` owns what
`isolation` guarantees; whether a read-committed default is the wanted default for
a workload of this shape is not a decision this run makes.

## 7. Delta against the previous run of this shape (cross-version)

The comparator is `bench/v3.0.0/results-scenario2-freight-v2.7.0-531-g9a0525d.md`
(three runs per cell, held at `36449d28`). {version_distance_note()}
**495 commits and 176 first-parent merges separate the two runs**, so no
difference below is attributable to one change. Throughput deltas: section 3.
Statement-level, run 1 of each against run 1 of the comparator:

{T_x}

- **`cores = 1`: every phase is 17 to 27 % faster at p50 and the floor moved
  with it.** `group`: `booking` {ph(load('s2-c1-g'),'booking')['p50_us']/ph(load('s2-c1-g',2),'booking')['p50_us']:.2f}x of the comparator's p50, `commit` {ph(load('s2-c1-g'),'commit')['p50_us']/ph(load('s2-c1-g',2),'commit')['p50_us']:.2f}x, `freight-insert`
  {ph(load('s2-c1-g'),'freight-insert')['p50_us']/ph(load('s2-c1-g',2),'freight-insert')['p50_us']:.2f}x, `cargo-lookup` {ph(load('s2-c1-g'),'cargo-lookup')['p50_us']/ph(load('s2-c1-g',2),'cargo-lookup')['p50_us']:.2f}x; p0 of `cargo-lookup` {ph(load('s2-c1-g'),'cargo-lookup')['p0_us']:.1f} against {ph(load('s2-c1-g',2),'cargo-lookup')['p0_us']:.1f} µs. The reactor queue is
  shorter because the statements are cheaper; this run does not apportion it between
  milestones (BM's scan kernel and BN's read/update path are candidates for the reads
  and updates; BA's commit changes for `commit`).
- **`cores = 8`, `group`: the write statements are 33 to 41 % faster at p50** (`charge-insert`
  {ph(load('s2-c8-g'),'charge-insert')['p50_us']/ph(load('s2-c8-g',2),'charge-insert')['p50_us']:.2f}x, `freight-insert` {ph(load('s2-c8-g'),'freight-insert')['p50_us']/ph(load('s2-c8-g',2),'freight-insert')['p50_us']:.2f}x), the reads 8 to 12 % and `commit` {ph(load('s2-c8-g'),'commit')['p50_us']/ph(load('s2-c8-g',2),'commit')['p50_us']:.2f}x.
- **`cores = 8`, `strict`: the write statements did not change** (0.92x to 1.03x) **but
  `commit` and `booking` are 30 % faster** ({ph(load('s2-c8-s'),'commit')['p50_us']:,.1f} against {ph(load('s2-c8-s',2),'commit')['p50_us']:,.1f} µs): the whole
  gain is in the commit, which is where BA-S7 (a `strict` commit stages its record, parks with
  its marker held and publishes after its sync) acts; this run does not isolate it. The commit is still
  half of a `strict` booking at eight cores ({100*share_row(load('s2-c8-s',2))[0][-1]:.1f} % in the comparator, {100*share_row(load('s2-c8-s'))[0][-1]:.1f} % now), only shorter.

## 8. Against PostgreSQL 18.6 (reused, not re-measured)

PostgreSQL stays the floor and answers "is the engine in the right league", not
"is it good". The PG18 twin (`tools/pg_scenario2_freight.py`) drives **one**
booking connection with its manifest reporter interleaved, so the matched KDS
cell is `--bookers 1`; the eight-booker cells above have no PostgreSQL
counterpart and are not compared with it. The PostgreSQL figures are the three
`synchronous_commit = on` runs measured at `f2f1ee7` on 2026-09-30, 14 days
before these cells, on a host that has since run other work; the KDS
`--bookers 1` cells were re-run here on this binary, three runs each, and the
comparator's own `--bookers 1` cells (also reused from that PG18 file) are
shown beside them.

{T_b1}

Matched, one client, durable: KDS is **{mt(newg)[0]/pgm:.2f}x** (`group`) and **{mt(news)[0]/pgm:.2f}x** (`strict`) of
PostgreSQL's rate; the comparator's pair was {mt(oldg)[0]/pgm:.2f}x and {mt(olds)[0]/pgm:.2f}x. The KDS cells' own change since
`9a0525d` ({100*(mt(newg)[0]/mt(oldg)[0]-1):+.1f} % and {100*(mt(news)[0]/mt(olds)[0]-1):+.1f} %) is inside their spreads ({mt(newg)[1]:.1f} % and {mt(news)[1]:.1f} %), so **the
single durable client did not change** and PostgreSQL's three runs (spread {100*(max(pgt)/min(pgt)-1):.1f} %) are
{pgm/mt(newg)[0]:.2f}x of KDS. The reason is in the decomposition: the commit's fsync is {100*statistics.median(pg_sh):.0f} % of a PostgreSQL booking and
{100*statistics.median(sh_b1):.0f} % of a KDS one, and its p50 is the same on both engines. Per-phase p50, medians of three (the
comparator's KDS and the PG18 columns are reused):

{T_b1p}

KDS's write statements are {min(rat_w):.2f}x to {max(rat_w):.2f}x of PostgreSQL's p50 (faster), its three pk reads
{min(rat_r):.2f}x to {max(rat_r):.2f}x, and its `recipe-read` FilterScan {rat_rec:.2f}x (the PG twin serves it from an
index it creates; scenario 3's PG file shows the same class of gap). Percentiles
of the matched KDS cells, run 1:

{T_b1pct}

## 9. Noise floor and host stamps

Three runs per cell: the spread column of section 3 is the floor, {spr['c1-g']:.1f} % (`c1-g`), {spr['c8-g']:.1f} % (`c8-g`),
{spr['c1-s']:.1f} % (`c1-s`) and {spr['c8-s']:.1f} % (`c8-s`). A difference below those in the matching cell is not a
result. The `c1-s` spread is the widest of the uncontended cells; the comparator's
42 % `c8-s` spread did not repeat here (three runs within {spr['c8-s']:.1f} %), so the comparator's median for
that cell is the one to distrust. The device thermometer, `load-cargos` p50, is
{min(therm)/1000:.2f} to {max(therm)/1000:.2f} ms across all {len(therm)} cells (the comparator's was 1.30 to 1.38 ms): no device stall is
indicated (`recovery_checkpoint_us` was not sampled).

**Contamination and re-runs.** Another session's builds and test suite ran on this host
during the measurement. Three of run 3's cells were discarded and re-run: `r3-s2-c1-g` three times (the third
discard had a `ctest` running `kds_tests` in 44 of its 54 monitor samples), `r3-s2-c8-s` twice and
`r3-s2-c1-s` once. The discarded outputs are in `contaminated/` in the archive
(`r3-s2-c1-g.attempt0..2`, `r3-s2-c1-s.attempt0`, `r3-s2-c8-s.attempt0..1`). A cell is discarded for
a build or test process in its window, never for its result.
The first attempt of the whole run did not start: the server refused the comparator's
config because `buffer_pool_frames` is now required (BE), which is why that key is in
section 1's config row (`contaminated/attempt0-no-buffer-pool-key`).

{T_host}

## 10. What the run says about the engine

1. **The commit path no longer bounds `strict` at eight cores.** `group` over `strict`
   fell from {omed['c8-g']/omed['c8-s']:.2f}x to {med['c8-g']/med['c8-s']:.2f}x and `strict` at eight cores runs {med['c8-s']:,.1f} tps; `commit` p50 is {ph(load('s2-c8-s'),'commit')['p50_us']:,.1f}
   against {ph(load('s2-c8-g'),'commit')['p50_us']:,.1f} µs (`group`). This is the data point for `docs/spec/wal.md` §8 and §11's
   claim that a `strict` commit parks instead of blocking its reactor.
2. **A single durable client is device-bound and unchanged.** One booker: {mt(newg)[0]:,.1f} /
   {mt(news)[0]:,.1f} tps now, {mt(oldg)[0]:,.1f} / {mt(olds)[0]:,.1f} at the comparator, and the commit fsync is
   {100*statistics.median(sh_b1):.0f} % of the booking; the statements around it are {100*(1-max(stm)):.0f} to {100*(1-min(stm)):.0f} % faster at p50 and show
   only as the {100*(1-statistics.median(sh_b1)):.0f} % that is not commit. The statement side of the one-client path
   can gain at most that share while the commit is {ph(load('b1-s2-c1-g'),'commit')['p50_us']/1000:.2f} ms of a {ph(load('b1-s2-c1-g'),'booking')['p50_us']/1000:.2f} ms booking (p50).
3. **Cores help, and the host is the next bound.** {med['c8-g']/med['c1-g']:.2f}x under `group` for
   eight cores; the `cores = 8` cells put eight reactors, eight booker processes and a
   reporter on the host's eight logical CPUs, so how much of the headroom above 2.9x is
   the engine and how much the clients' own CPU is not separable here. No limit was
   found by adding load: the 8-core `group` cell spread is {spr['c8-g']:.1f} % and nothing in it
   saturates but the commit.
4. **Strict still conflicts, less.** `TXN_CONFLICT` replies reached the driver 90 / 63 / 45 times in
   the three `c8-s` runs, against 0 / 0 / 2 at `c8-g` (comparator 125 / 50 / 77 and 0 / 3 / 2): row locks held
   across the longer commit wait remain the likely mechanism; this run does not show it.
5. **The invariant failures are the driver's read-committed lost update** and
   repeatable-read removes them (0 of 400 in both probes); the price at eight cores is
   {abs(100*(load('rr-s2-c8-g')['meta']['tps']/med['c8-g']-1)):.1f} % in this run's single probe.

Archive: `bench/v3.1.0/archive/scenario2-{DESC}/`: per-cell JSON, driver
stdout, load and schema logs, prechecks, server logs, configs, run scripts
(`run_all3.sh`, `lib.sh`), `monitor.log`, the timeline, the binary hash, the
contaminated attempts.
"""
open(f"{OUT}/results-scenario2-freight-{DESC}.md", "w").write(doc)
print("written", len(doc))
