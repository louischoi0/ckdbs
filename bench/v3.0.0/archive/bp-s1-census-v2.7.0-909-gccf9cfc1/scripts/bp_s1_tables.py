#!/usr/bin/env python3
"""BP-S1 census: every table of results-bp-s1-census-*.md, from the raw JSON.

Reads pass 1 (1592007e) under --p1 and pass 2 (ccf9cfc1) under --p2; prints
markdown to stdout. Analysis only: starts no server and runs no benchmark.
    nice python3 -I bp_s1_tables.py --p1 <dir of pass-1 out/> --p2 <dir of pass-2 out/> --section h10
Percentiles are nearest rank (tools/bench_common.nearest_rank)."""
import argparse, glob, gzip, json, math, os, re, statistics

med = statistics.median


def nr(xs, p):
    xs = sorted(xs)
    if not xs:
        return None
    rank = max(1, math.ceil(round(p * len(xs) / 100, 9))) - 1
    return xs[min(rank, len(xs) - 1)]


def gl(pattern):
    """Sorted matches of pattern, each file once, whether stored plain or as .gz."""
    seen = {}
    for f in glob.glob(pattern) + glob.glob(pattern + ".gz"):
        seen[f[:-3] if f.endswith(".gz") else f] = f
    return [seen[k] for k in sorted(seen)]


def lj(path):
    if path.endswith(".gz"):
        return json.load(gzip.open(path, "rt"))
    return json.load(open(path))


def ul(xs, unit):
    """A list of counts with its unit in the cell (bench/README.md, 'Every cell carries its unit')."""
    return f"{list(xs)} {unit}"


def f0(x): return f"{x:,.0f}"
def f1(x): return f"{x:,.1f}"
def f2(x): return f"{x:,.2f}"
def sp(xs, f=f1, unit=""):
    return f"{f(med(xs))}{unit} ({f(min(xs))}-{f(max(xs))})"


def meta_sum(cell_meta_after, cell_meta_before, key):
    return cell_meta_after.get(key, 0) - cell_meta_before.get(key, 0)


# ---------------------------------------------------------------- pass 1: H6
def h6_growth(p1):
    print("#### growth cell (pass 1): statements and latency")
    print("| durability | cores | sessions (relations) | runs | statements per run | stmt/s median (min-max) | p0 | p25 | p50 | p95 | p99 | max (largest of runs) | client CPU share (max over sessions) |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    tot = {}
    for dur in ("group", "relaxed"):
        for k in (1, 2, 4):
            rs = [lj(f) for f in sorted(glob.glob(f"{p1}/cell1-{dur}-k{k}/*.json"))]
            tot[(dur, k)] = rs
            q = lambda key: med([r[key] for r in rs])
            print(f"| `{dur}` | {k} | {2*k} | {len(rs)} | {f0(med([r['statements'] for r in rs]))} stmt | "
                  f"{sp([r['statements_per_s'] for r in rs], f0, ' stmt/s')} | {f1(q('p0_us'))} µs | {f1(q('p25_us'))} µs | "
                  f"{f1(q('p50_us'))} µs | {f1(q('p95_us'))} µs | {f1(q('p99_us'))} µs | {f1(max(r['max_us'] for r in rs))} µs | "
                  f"{max(max(r['client_cpu_share']) for r in rs):.2f} of a CPU |")
    print()
    print("#### growth cell (pass 1): word moves by cause and the memo drops they cost")
    print("| durability | cores | root moves per run | cabin | schema | resolve | memo drops per run | drops / (root moves x (cores-1)) | memo drops per statement | structural reruns | carves per run |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for (dur, k), rs in tot.items():
        g = lambda key: [r["meta_delta"].get("contention_" + key, 0) for r in rs]
        n = sum(r["statements"] for r in rs)
        roots = g("word_moves_root")
        drops = g("catalog_memo_drops")
        ratio = "-" if k == 1 else f"{sum(drops) / (sum(roots) * (k - 1)):.2f}"
        print(f"| `{dur}` | {k} | {ul([int(x) for x in roots], 'moves')} | {int(sum(g('word_moves_cabin')))} moves | {int(sum(g('word_moves_schema')))} moves | "
              f"{int(sum(g('word_moves_resolve')))} moves | {ul([int(x) for x in drops], 'drops')} | {ratio} (ratio) | {sum(drops) / n:.2e} drops/stmt | "
              f"{int(sum(g('structural_reruns')))} reruns | {ul([int(x) for x in g('carves')], 'carves')} |")
    print()
    print("#### growth cell (pass 1): host per run")
    print("| durability | cores | load 1 min before (per run) | load 1 min after | competing compile/test processes listed by the driver | data device |")
    print("|---|---|---|---|---|---|")
    for (dur, k), rs in tot.items():
        print(f"| `{dur}` | {k} | {ul([r['host_before']['loadavg'].split()[0] for r in rs], 'load')} | {ul([r['host_after']['loadavg'].split()[0] for r in rs], 'load')} | "
              f"{sum(len(r['host_before']['competing']) + len(r['host_after']['competing']) for r in rs)} processes | {rs[0]['data_device']} |")
    print()


def h6_cabin(p1):
    print("#### CABIN AUTO cell (pass 1)")
    print("| cores | run | readers | driver wall | load 1 min before -> after | root | cabin | schema | resolve | memo drops (total) | drops before the first cabin move | drops after it | structural reruns | reader statements | on-arm statements |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    rdrows = []
    for f in [x for x in sorted(glob.glob(p1 + "/cell2/cabin-k*-r*.json")) if ".driver" not in x]:
        d = lj(f)
        b, a = d["meta_before"], d["meta_after"]
        dl = {k[len("contention_"):]: a[k] - b.get(k, 0) for k in a if k.startswith("contention_") and ("word" in k or "memo" in k or "rerun" in k)}
        tl = d["timeline"]
        first = next((t for t, m in tl if m.get("word_moves_cabin", 0) > 0), None)
        pre = [m for t, m in tl if first is not None and t < first][-1] if first is not None else {}
        post = dl["catalog_memo_drops"] - pre.get("catalog_memo_drops", 0)
        rs = d.get("readers_stats") or {}
        nread = sum(r["statements"] for r in rs.values())
        js = lj(f + ".driver.json")
        on_ops = sum(p["ops"] for p in js["phases"] if "[on]" in p["phase"])
        print(f"| {d['cores']} | r{d['run']} | {'yes' if d['readers'] else 'no'} | {d['driver_s']} s | {d['loadavg_before'].split()[0]} -> {d['loadavg_after'].split()[0]} load | "
              f"{dl['word_moves_root']} moves | {dl['word_moves_cabin']} moves | {dl['word_moves_schema']} moves | {dl['word_moves_resolve']} moves | {dl['catalog_memo_drops']} drops | "
              f"{pre.get('catalog_memo_drops', '-')} drops | {str(post) + ' drops' if first is not None else '-'} | {dl['structural_reruns']} reruns | {f0(nread)} stmt | {f0(on_ops)} stmt |")
        for i, r in sorted(rs.items()):
            rdrows.append((d["cores"], d["run"], r))
    print()
    print("Readers (point `SELECT`, free-running, one session per core 1..3, same server as the scenario's session on core 0):")
    print("| run | reader core | statements | errors | p0 | p25 | p50 | p95 | p99 |")
    print("|---|---|---|---|---|---|---|---|---|")
    for k, run, r in rdrows:
        print(f"| r{run} | {int(r['core'])} | {f0(r['statements'])} stmt | {r['errors']} errors | {r['p0_us']} µs | {r['p25_us']} µs | {r['p50_us']} µs | {r['p95_us']} µs | {r['p99_us']} µs |")
    print()


def h6_pass2(p2):
    print("#### word moves and memo drops in pass 2's checkpoint cells (ccf9cfc1): four loaders into ONE relation `load`, 240 s, SHOW META after the load")
    print("| cell | cores | rows inserted into `load` | root moves | cabin | schema | resolve | memo drops | structural reruns | statements the cell issued (loader INSERTs + probes) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for f in h7_cells(p2 + "/h7", "bp2-k*-cp5000-*.json"):
        base = os.path.basename(f).replace(".gz", "")[:-5].split("-")
        o = h7_one(f)
        m = o["meta0"]
        stm = sum(l["statements"] for l in o["cell"]["load"]) + len(o["xs"]) + len(o["ps"])
        print(f"| {base[1]}-{base[3]} | {o['cores']} | {f0(o['rows'])} rows | {m.get('contention_word_moves_root')} moves | {m.get('contention_word_moves_cabin')} moves | {m.get('contention_word_moves_schema')} moves | "
              f"{m.get('contention_word_moves_resolve')} moves | {m.get('contention_catalog_memo_drops')} drops | {m.get('contention_structural_reruns')} reruns | {f0(stm)} stmt |")
    print()


def h6_reresolve(p1):
    d = lj(p1 + "/reresolve.json")
    print("#### one memo drop's cost (pass 1, `cores = 2`, reactors pinned, 300 iterations per statement, 900 `CREATE TABLE junk_i` over the run)")
    print("| statement | arm | n | p0 | p25 | p50 | p95 | p99 |")
    print("|---|---|---|---|---|---|---|---|")
    for s, v in d["stmts"].items():
        for arm, name in (("bump_cold", "first statement after the schema word moved (drop)"), ("bump_warm", "same statement, warm"),
                          ("control_first", "control: first statement after a word-neutral `SHOW META` on the other core"), ("control_rest", "control: same statement, rest")):
            x = v[arm]
            print(f"| {s} | {name} | {x['n']} stmt | {x['p0_us']} µs | {x['p25_us']} µs | {x['p50_us']} µs | {x['p95_us']} µs | {x['p99_us']} µs |")
    print()
    print("| statement | cold - warm, median (wall) | control first - rest (wall) | net of control (wall) | server CPU cold - warm | control (server CPU) | net of control (server CPU) | net wall by third of the run (catalog grows 0 -> 900 junk tables) |")
    print("|---|---|---|---|---|---|---|---|")
    for s, v in d["stmts"].items():
        thirds = [round(a - b, 1) for a, b in zip(v["bump_cold_minus_warm_median_us_by_third"], v["control_first_minus_rest_median_us_by_third"])]
        print(f"| {s} | {v['bump_cold_minus_warm_median_us']} µs | {v['control_first_minus_rest_median_us']} µs | {v['bump_cold_minus_warm_median_us'] - v['control_first_minus_rest_median_us']:.1f} µs | "
              f"{v['server_cpu_cold_minus_warm_median_us']} µs | {v['control_server_cpu_first_minus_rest_median_us']} µs | "
              f"{v['server_cpu_cold_minus_warm_median_us'] - v['control_server_cpu_first_minus_rest_median_us']:.1f} µs | {thirds} µs |")
    print()


# ---------------------------------------------------------------- H7
def h7_cells(dirn, pat):
    return gl(os.path.join(dirn, pat))


def h7_one(path):
    c = lj(path)
    lo = c["window"][0]
    hi = c.get("load_stop", c["window"][1])
    span = c["seconds"]
    runs_all = c["runs"]
    runs = [r for r in runs_all if r["end"] >= lo and r["start"] <= hi]
    metas = [m for m in c["meta"].values() if m]
    rows = sum(l["rows"] for l in c["load"])
    xs, ps, xin, xout = [], [], [], []
    spans = {}
    for r in runs:
        spans.setdefault(r["core"], []).append((r["start"], r["end"]))
    for key, v in c["samples"].items():
        kind, core = key.split(":")
        core = int(core)
        for t, ms in v:
            if ms < 0 or not (lo <= t <= hi):
                continue
            if kind == "stmt":
                xs.append(ms)
                (xin if any(s <= t <= e for s, e in spans.get(core, [])) else xout).append(ms)
            else:
                ps.append(ms)
    ends = sorted(r["end"] for r in runs if r["completed"] and lo <= r["end"] <= hi)
    gaps = [b - a for a, b in zip(ends, ends[1:])]
    out = dict(cell=c, runs=runs, rows=rows, span=span, xs=sorted(xs), ps=sorted(ps), xin=sorted(xin), xout=sorted(xout),
               gap=max(gaps) if gaps else None, cores=c["cores"])
    out["pages_counter"] = max(m.get("contention_checkpoint_pages_written", 0) for m in metas)
    out["syncs_counter"] = max(m.get("contention_data_syncs", m.get("contention_checkpoint_data_syncs", 0)) for m in metas)
    out["runs_counter"] = max(m.get("contention_checkpoint_runs", 0) for m in metas)
    out["pages_log"] = sum(r.get("pages", 0) for r in runs_all)
    out["pages_log_window"] = sum(r.get("pages", 0) for r in runs)
    out["meta0"] = metas[0]
    return out


def pctrow(xs, unit="ms", digits=2):
    if not xs:
        return "- | - | - | - | - | -"
    fmt = lambda v: f"{v:,.{digits}f} {unit}"
    return " | ".join(fmt(v) for v in (xs[0], nr(xs, 25), nr(xs, 50), nr(xs, 95), nr(xs, 99), xs[-1]))


def h7(p1, p2):
    print("#### checkpoint cell, pass 2 (ccf9cfc1): one row per cell")
    print("| cell | cores | rep | load 1 min before | io pressure `full` avg10 before / after | rows inserted | rows/s | checkpoint runs (load span) | runs per 5 s period | pages written (counter) | pages by run log, load span | pages per 1,000 rows (counter) | pages per 1,000 rows (run log) | share of core-seconds inside a run | MB/s written back | data syncs (counter) | syncs per run | run wall p50 / max | longest anchor gap | read as |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    res = {}
    for f in h7_cells(p2 + "/h7", "bp2-k*-cp5000-*.json"):
        base = os.path.basename(f).replace(".gz", "")[:-5].split("-")
        o = h7_one(f)
        res[(base[1], base[3])] = o
        c = o["cell"]
        io = lambda hs: re.search(r"full avg10=([\d.]+)", hs["pressure_io"]).group(1)
        n = len(o["runs"])
        walls = [r["wall_us"] / 1000 for r in o["runs"]]
        flag = {"k1a": "stalled by host I/O: not usable for rate or latency", "k1b": "stalled by host I/O: not usable for rate or latency", "k2a": "host I/O pressure at start", "k4b": "one 1.6 s stall seen by all four loaders"}.get(base[1] + base[3], "clean")
        print(f"| {base[1]}-{base[3]} | {c['cores']} | {base[3]} | {c['host_before']['loadavg'].split()[0]} load | {io(c['host_before'])} % / {io(c['host_after'])} % | {f0(o['rows'])} rows | "
              f"{f0(o['rows'] / o['span'])} rows/s | {n} runs | {n * 5 / o['span']:.2f} runs/period | {f0(o['pages_counter'])} pages | {f0(o['pages_log_window'])} pages | {1000 * o['pages_counter'] / o['rows']:.1f} pages/1,000 rows | {1000 * o['pages_log_window'] / o['rows']:.1f} pages/1,000 rows | "
              f"{100 * sum(walls) / 1000 / (c['cores'] * o['span']):.1f} % | {o['pages_counter'] * 8192 / 1e6 / o['span']:.1f} MB/s | {f0(o['syncs_counter'])} syncs | {o['syncs_counter'] / max(1, o['runs_counter']):.1f} syncs/run | "
              f"{med(walls):.0f} ms / {max(walls):.0f} ms | {o['gap']:.2f} s | {flag} |")
    print()
    print("#### the foreground probe (point `SELECT` of a 1,000-row relation, one session per core, open loop at 20 ms, latency from the scheduled time), all cores pooled, over the load span")
    print("| cell | n | p0 | p25 | p50 | p95 | p99 | max |")
    print("|---|---|---|---|---|---|---|---|")
    for key, o in res.items():
        print(f"| {key[0]}-{key[1]} | {len(o['xs']):,} samples | {pctrow(o['xs'])} |")
    print()
    print("Probe inside and outside the probed core's own checkpoint runs (clean cells):")
    print("| cell | class | n | p0 | p25 | p50 | p95 | p99 | max |")
    print("|---|---|---|---|---|---|---|---|---|")
    for key in (("k2", "a"), ("k2", "b"), ("k4", "a"), ("k4", "b")):
        o = res[key]
        print(f"| {key[0]}-{key[1]} | inside a run of its core | {len(o['xin']):,} samples | {pctrow(o['xin'])} |")
        print(f"| {key[0]}-{key[1]} | outside | {len(o['xout']):,} samples | {pctrow(o['xout'])} |")
    print()
    print("`C_PING` probe (no statement path) pooled:")
    print("| cell | n | p0 | p25 | p50 | p95 | p99 | max |")
    print("|---|---|---|---|---|---|---|---|")
    for key, o in res.items():
        print(f"| {key[0]}-{key[1]} | {len(o['ps']):,} samples | {pctrow(o['ps'])} |")
    print()
    print("#### where a checkpoint run's wall time goes (sum over the run records in the load span, per run; the phases are the server's own run line)")
    print("| cell | runs | wall per run | start | page writes | publish writeback | parked on the log (gate/end) | parked on a data sync | the rest (steps between, yields) |")
    print("|---|---|---|---|---|---|---|---|---|")
    for key, o in res.items():
        rs = o["runs"]
        n = len(rs)
        w = sum(r["wall_us"] for r in rs) / n / 1000
        st = sum(r.get("start_sum_us", 0) for r in rs) / n / 1000
        pw = sum(r.get("page_write_sum_us", 0) for r in rs) / n / 1000
        pb = sum(r.get("publish_writeback_sum_us", 0) for r in rs) / n / 1000
        lp = sum(r.get("log_park_us", 0) for r in rs) / n / 1000
        dp = sum(r.get("data_park_us", 0) for r in rs) / n / 1000
        rest = w - st - pw - pb - lp - dp
        pc = lambda v: f"{v:,.1f} ms ({100 * v / w:.0f} %)"
        print(f"| {key[0]}-{key[1]} | {n} runs | {w:,.1f} ms | {pc(st)} | {pc(pw)} | {pc(pb)} | {pc(lp)} | {pc(dp)} | {pc(rest)} |")
    print()
    print("#### the instance's waits over the cell (SHOW META `contention`, whole life of the server)")
    print("| cell | frame table | WAL stream | WAL sync gate | window | lock partition | page latch spin turns | share of core-seconds in frame-table waits |")
    print("|---|---|---|---|---|---|---|---|")
    for key, o in res.items():
        m = o["meta0"]
        cs = o["cores"] * (o["span"] + 2)
        ft = m.get("contention_frame_table_wait_us", 0) / 1e6
        print(f"| {key[0]}-{key[1]} | {f0(m.get('contention_frame_table_waits', 0))} waits, {ft:.2f} s | {f0(m.get('contention_wal_stream_waits', 0))} waits, {m.get('contention_wal_stream_wait_us', 0) / 1e6:.2f} s | "
              f"{f0(m.get('contention_wal_sync_gate_waits', 0))} waits, {m.get('contention_wal_sync_gate_wait_us', 0) / 1e6:.2f} s | {f0(m.get('contention_window_waits', 0))} waits, {m.get('contention_window_wait_us', 0) / 1e6:.3f} s | "
              f"{f0(m.get('contention_lock_partition_waits', 0))} waits, {m.get('contention_lock_partition_wait_us', 0) / 1e6:.4f} s | {f0(m.get('contention_page_latch_spin_turns', 0))} turns | {100 * ft / cs:.2f} % |")
    print()

    print("#### pass 1 (1592007e), the same cell: `bi_checkpoint_stall.py`, group, 240 s, 5 s cadence, one run per core count (counters of pages and syncs are the flawed ones and are not shown)")
    print("| cores | rows inserted | rows/s | checkpoint runs | runs per 5 s period | run wall p50 / max | share of core-seconds inside a run | pages per 1,000 rows (run log) | longest anchor gap | io pressure `full` avg10 before | probe n | p0 | p25 | p50 | p95 | p99 | max |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for f in gl(p1 + "/cell3/bi-k*-r0.json"):
        o = h7_one(f)
        c = o["cell"]
        walls = [r["wall_us"] / 1000 for r in o["runs"]]
        io = re.search(r"full avg10=([\d.]+)", c["host_before"]["pressure_io"]).group(1) if c["host_before"].get("pressure_io") else "-"
        n = len(o["runs"])
        print(f"| {c['cores']} | {f0(o['rows'])} rows | {f0(o['rows'] / o['span'])} rows/s | {n} runs | {n * 5 / o['span']:.2f} runs/period | {med(walls):.0f} ms / {max(walls):.0f} ms | {100 * sum(walls) / 1000 / (c['cores'] * o['span']):.1f} % | {1000 * o['pages_log_window'] / o['rows']:.1f} pages/1,000 rows | {o['gap']:.2f} s | {io} % | {len(o['xs']):,} samples | {pctrow(o['xs'])} |")
    print()
    print("#### cadence as an instrument (pass 1, `cores = 4`, 150 s cells, eight repeats each): checkpoint runs per unit time cut 4x by `checkpoint_interval_ms` 5,000 -> 20,000")
    print("| cadence | repeats | checkpoint runs per cell | rows/s median (min-max) | probe p50 median (min-max) | probe p99 median (min-max) | probe max median (min-max) | longest anchor gap median (min-max) |")
    print("|---|---|---|---|---|---|---|---|")
    for cad in ("20000", "5000"):
        os_ = [h7_one(f) for f in gl(f"{p1}/cell3c/bp-k4-cp{cad}-e*.json")]
        print(f"| {int(cad) // 1000} s | {len(os_)} repeats | {ul(sorted(set(len(o['runs']) for o in os_)), 'runs')} | {sp([o['rows'] / o['span'] for o in os_], f0, ' rows/s')} | "
              f"{sp([nr(o['xs'], 50) for o in os_], f2, ' ms')} | {sp([nr(o['xs'], 99) for o in os_], f1, ' ms')} | {sp([o['xs'][-1] for o in os_], f1, ' ms')} | {sp([o['gap'] for o in os_], f2, ' s')} |")
    print()
    return res


# ---------------------------------------------------------------- H9
def h9(p1, p2):
    def load(d, pat):
        return [lj(f) for f in sorted(glob.glob(os.path.join(d, pat)))]
    for label, d, pat in (("pass 2 (ccf9cfc1), counters fixed", p2 + "/h9", "inflight-k*-r*.json"), ("pass 1 (1592007e)", p1 + "/cell4", "inflight-k*-r*.json")):
        runs = load(d, pat)
        print(f"#### PURGE of 100,000 delete-marked rows, `relaxed`, {label}")
        print("| cores | run | session on core | load 1 min before | PURGE wall | PURGE rows/s (derived: 100,000 / wall) | PURGE server CPU | CPU per row | asks | misses | ids read per miss (pass 2 only) | `DELETE` of the same rows: wall / server CPU / asks |")
        print("|---|---|---|---|---|---|---|---|---|---|---|")
        for r in runs:
            p, dl = r["purge"], r["delete"]
            i = p["inflight"]
            ids = i.get("contention_inflight_miss_ids_read")
            idtxt = f"{ids / max(1, i['contention_inflight_misses']):.2f}" if ids is not None else "-"
            print(f"| {r['cores']} | r{r['run']} | core {r['session_core']} | {r['loadavg_before'].split()[0]} load | {p['wall_ms']} ms | {f0(100000 / (p['wall_ms'] / 1000))} rows/s | {p['server_cpu_ms']} ms | {1000 * p['server_cpu_ms'] / 100000:.2f} µs | "
                  f"{f0(i['contention_inflight_asks'])} asks | {f0(i['contention_inflight_misses'])} misses | {idtxt + ' ids/miss' if ids is not None else '-'} | {dl['wall_ms']} ms / {dl['server_cpu_ms']} ms / {dl['inflight'].get('contention_inflight_asks', 0)} asks |")
        print()
        print("| cores | PURGE CPU, median (min-max) | PURGE CPU, the two lowest runs | DELETE CPU, median (min-max) | k4 - k1, PURGE | k4 - k1, DELETE (no in-flight ask) |")
        print("|---|---|---|---|---|---|")
        by = {}
        for r in runs:
            by.setdefault(r["cores"], []).append(r)
        for k, rs in sorted(by.items()):
            pc = [r["purge"]["server_cpu_ms"] for r in rs]
            dc = [r["delete"]["server_cpu_ms"] for r in rs]
            print(f"| {k} | {sp(pc, f1, ' ms')} | {sorted(pc)[:2]} | {sp(dc, f1, ' ms')} | - | - |")
        if 1 in by and 4 in by:
            a = med([r["purge"]["server_cpu_ms"] for r in by[1]])
            b = med([r["purge"]["server_cpu_ms"] for r in by[4]])
            a2 = med([r["delete"]["server_cpu_ms"] for r in by[1]])
            b2 = med([r["delete"]["server_cpu_ms"] for r in by[4]])
            print(f"| | | | | {b - a:+.1f} ms ({100 * (b - a) / a:+.1f} %) | {b2 - a2:+.1f} ms ({100 * (b2 - a2) / a2:+.1f} %) |")
        print()
        print("Named-key `INSERT` loops (20,000 statements per loop, one session, a fresh table):")
        print("| cores | loop | runs | stmt/s median (min-max) | server CPU per stmt median (min-max) | p0 | p25 | p50 | p95 | p99 | max | in-flight asks | errors per run |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for k, rs in sorted(by.items()):
            for ph in ("named_fresh", "named_duplicate"):
                q = lambda key: med([r[ph]["lat"][key] for r in rs])
                print(f"| {k} | {'fresh keys' if ph == 'named_fresh' else 'the same keys again (duplicates)'} | {len(rs)} | {sp([r[ph]['stmt_per_s'] for r in rs], f0, ' stmt/s')} | "
                      f"{sp([r[ph]['server_cpu_us_per_stmt'] for r in rs], f1, ' µs')} | {q('p0_us')} µs | {q('p25_us')} µs | {q('p50_us')} µs | {q('p95_us')} µs | {q('p99_us')} µs | n/r | "
                      f"{sum(r[ph]['inflight']['contention_inflight_asks'] for r in rs)} asks | {ul([r[ph]['errors'] for r in rs], 'errors')} |")
        print()
        print("A named key meeting another session's undecided insert of the same key (20 waits of 200 ms per run):")
        print("| cores | in-flight asks per run | misses | ids read | session cores |")
        print("|---|---|---|---|---|")
        for k, rs in sorted(by.items()):
            vs = [r["named_vs_undecided"]["inflight"] for r in rs]
            print(f"| {k} | {ul([v['contention_inflight_asks'] for v in vs], 'asks')} | {ul([v['contention_inflight_misses'] for v in vs], 'misses')} | {ul([v.get('contention_inflight_miss_ids_read', v.get('contention_inflight_tables_read')) for v in vs], 'ids or tables read')} | {ul([r['session_core'] for r in rs], 'core')} |")
        print()


# ---------------------------------------------------------------- H10
ARMS = ["update-hot", "update-disjoint", "update-disjoint-again", "insert-omitted", "select-hot", "ping"]


def h10_load(d, pat="x-locks-*-r?.json", p1=False):
    out = {}
    for f in sorted(glob.glob(os.path.join(d, pat))):
        if f.endswith(".bench.json"):
            continue
        m = lj(f)
        b = lj(f[:-5] + ".bench.json")
        key = (m["durability"], m["cores"], "fence" if m["fence"] else "plain")
        out.setdefault(key, []).append((m, b))
    return out


def stmts_of(b):
    return b["sessions"] * b["ops_per_session_per_arm"] * 6


def h10(p1, p2):
    for label, d in (("pass 2 (ccf9cfc1)", p2 + "/h10"), ("pass 1 (1592007e)", p1 + "/cell5")):
        data = h10_load(d)
        print(f"#### {label}: lock cell, server CPU per statement (3 runs per row; r0, r1, r2) and the fence's cost, paired by run")
        print("| durability | cores | partitions | plain: CPU per stmt (r0, r1, r2) | fence: CPU per stmt (r0, r1, r2) | paired delta fence - plain (r0, r1, r2) | delta of medians | fence walks per stmt | latch acquisitions a walk makes | bench wall (plain / fence, median) |")
        print("|---|---|---|---|---|---|---|---|---|---|")
        for dur in ("relaxed", "group"):
            for k in (1, 4, 8):
                pl = data.get((dur, k, "plain"))
                fe = data.get((dur, k, "fence"))
                if not pl or not fe:
                    continue
                n = stmts_of(pl[0][1])
                pc = [m["server_cpu_s"] * 1e6 / n for m, b in pl]
                fc = [m["server_cpu_s"] * 1e6 / n for m, b in fe]
                pair = [f - p for p, f in zip(pc, fc)]
                walks = [(m["meta_after"].get("contention_fence_walks", 0) - m["meta_before"].get("contention_fence_walks", 0)) / n for m, b in fe]
                latches = 0 if k == 1 else 64 * k
                print(f"| `{dur}` | {k} | {64 * k} partitions | {[round(x, 1) for x in pc]} µs | {[round(x, 1) for x in fc]} µs | {[round(x, 1) for x in pair]} µs "
                      f"({[round(100 * x / p, 1) for x, p in zip(pair, pc)]} %) | {med(fc) - med(pc):+.1f} µs ({100 * (med(fc) - med(pc)) / med(pc):+.1f} %) | {med(walks):.2f} walks/stmt | "
                      f"{str(latches) + ' latches' if k > 1 else '64 reads, no latch'} | {med([m['bench_s'] for m, b in pl]):.1f} s / {med([m['bench_s'] for m, b in fe]):.1f} s |")
        print()
    # full percentile tables, pass 2
    data = h10_load(p2 + "/h10")
    for dur in ("relaxed", "group"):
        print(f"#### pass 2: `{dur}`, the statement distributions per arm (median over 3 runs of each percentile; max is the largest of the runs; the p99 spread is min-max over runs)")
        print("| cores | fence | arm | runs | stmt/s median (min-max) | p0 | p25 | p50 | p95 | p99 (min-max) | max | errors per run |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|")
        for k in (1, 4, 8):
            for mode in ("plain", "fence"):
                rs = data.get((dur, k, mode))
                if not rs:
                    continue
                for arm in ARMS:
                    A = [b["arms"]["B"][arm] for m, b in rs]
                    q = lambda key: med([a[key] for a in A])
                    p99s = [a["p99_us"] for a in A]
                    print(f"| {k} | {mode} | {arm} | {len(A)} runs | {sp([a['qps'] for a in A], f0, ' stmt/s')} | {f1(q('p0_us'))} µs | {f1(q('p25_us'))} µs | {f1(q('p50_us'))} µs | {f1(q('p95_us'))} µs | "
                          f"{f1(med(p99s))} µs ({f0(min(p99s))}-{f0(max(p99s))}) | {f0(max(a['max_us'] for a in A))} µs | {ul([a['errors'] for a in A], 'errors')} |")
        print()
    print("#### pass 2: fence - plain, per arm, relaxed (ratio of medians over the 3 runs; the spread column is the same arm repeated inside one run, `update-disjoint` vs `update-disjoint-again`)")
    print("| cores | arm | throughput plain -> fence | p99 plain -> fence | per-run throughput ratio fence/plain (r0, r1, r2) |")
    print("|---|---|---|---|---|")
    for k in (1, 4, 8):
        pl, fe = data[("relaxed", k, "plain")], data[("relaxed", k, "fence")]
        for arm in ARMS:
            qp = [b["arms"]["B"][arm]["qps"] for m, b in pl]
            qf = [b["arms"]["B"][arm]["qps"] for m, b in fe]
            pp = [b["arms"]["B"][arm]["p99_us"] for m, b in pl]
            pf = [b["arms"]["B"][arm]["p99_us"] for m, b in fe]
            print(f"| {k} | {arm} | {f0(med(qp))} -> {f0(med(qf))} stmt/s ({100 * (med(qf) / med(qp) - 1):+.1f} %) | {f0(med(pp))} -> {f0(med(pf))} µs ({100 * (med(pf) / med(pp) - 1):+.1f} %) | "
                  f"{[round(f / p, 3) for p, f in zip(qp, qf)]} |")
    print()
    print("#### pass 2: waits the lock cell can name (per statement; SHOW META deltas over the cell)")
    print("| durability | cores | fence | partition latch waits per stmt | partition latch wait per stmt | window latch waits | structural reruns | memo drops per run | sessions' cores (distinct, of cores) |")
    print("|---|---|---|---|---|---|---|---|---|")
    for dur in ("relaxed", "group"):
        for k in (1, 4, 8):
            for mode in ("plain", "fence"):
                rs = data.get((dur, k, mode))
                if not rs:
                    continue
                n = stmts_of(rs[0][1])
                d = lambda key: [m["meta_after"].get("contention_" + key, 0) - m["meta_before"].get("contention_" + key, 0) for m, b in rs]
                pw, pwu = d("lock_partition_waits"), d("lock_partition_wait_us")
                print(f"| `{dur}` | {k} | {mode} | {med(pw) / n:.2e} waits/stmt | {med(pwu) / n:.3f} µs | {ul(d('window_waits'), 'waits')} | {sum(d('structural_reruns'))} reruns | {ul(d('catalog_memo_drops'), 'drops')} | "
                      f"{ul([len(b['pools']['B']['cores']) for m, b in rs], 'cores')} of {k} |")
    print()


# ---------------------------------------------------------------- host
def host(p1, p2):
    print("#### pass 1 host stamps (one per cell, from `hoststamps.log`)")
    print("| cell | UTC | load 1 / 5 / 15 min | cpu pressure `some` avg10 / avg60 | competing compile/test processes |")
    print("|---|---|---|---|---|")
    txt = open(p1 + "/hoststamps.log").read()
    for blk in txt.split("=== ")[1:]:
        lines = blk.strip().split("\n")
        head = lines[0]
        m = re.match(r"(.*) (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) UTC", head)
        label, ts = (m.group(1), m.group(2)) if m else (head, "")
        la = re.search(r"loadavg: (\S+) (\S+) (\S+)", blk)
        cp = re.search(r"cpu pressure: some avg10=(\S+) avg60=(\S+)", blk)
        comp = re.search(r"competing: (.*)", blk).group(1).strip() or "none"
        print(f"| {label} | {ts} | {la.group(1)} / {la.group(2)} / {la.group(3)} load | {cp.group(1) if cp else '-'} / {cp.group(2) if cp else '-'} % | {comp[:80]} |")
    print()
    print("#### pass 2 host stamps (one per cell, from `hoststamps.log`; the cell's driver wrote its own before/after load and competing list)")
    print("| cell | UTC at start | load 1 / 5 / 15 min | fs |")
    print("|---|---|---|---|")
    for ln in open(p2 + "/hoststamps.log"):
        m = re.match(r"(\S+ \S+) (h7|h9|h10) (.*) load=(\S+) (\S+) (\S+) df=(\S+)", ln)
        if m:
            print(f"| {m.group(2)} {m.group(3)} | {m.group(1)} | {m.group(4)} / {m.group(5)} / {m.group(6)} load | {m.group(7)} |")
    print()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--p1", required=True)
    ap.add_argument("--p2", required=True)
    ap.add_argument("--section", required=True, choices=["h6", "h7", "h9", "h10", "host"])
    a = ap.parse_args()
    {"h6": lambda: (h6_growth(a.p1), h6_cabin(a.p1), h6_reresolve(a.p1), h6_pass2(a.p2)),
     "h7": lambda: h7(a.p1, a.p2), "h9": lambda: h9(a.p1, a.p2), "h10": lambda: h10(a.p1, a.p2),
     "host": lambda: host(a.p1, a.p2)}[a.section]()
