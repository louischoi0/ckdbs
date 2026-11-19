import json, os, sys, statistics as st, random
D = "/home/louis/ckdbs/.claude/worktrees/de-page-read-off-the-reactor/bench/v3.4.14/archive/de-close-v3.4.14-6-g1942cb9a"
S = "/tmp/claude-1000/-home-louis-ckdbs/783802aa-0ec8-4cfd-866b-862e378009ca/scratchpad"
OUT = S + "/tables"
os.makedirs(OUT, exist_ok=True)

def L(c, a, r):
    p = f"{D}/de-{c}-{a}-r{r}.json"
    return json.load(open(p)) if os.path.exists(p) else None

def us(x):
    return f"{x:,.1f} µs"

def pc(d):
    return " | ".join([us(d[k]) for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us", "p99.9_us", "max_us")] + [f"{d['n']:,} ops"])

def ms(x):
    return f"{x/1000:,.2f} ms"

def pcms(d):
    return " | ".join([ms(d[k]) for k in ("p0_us", "p25_us", "p50_us", "p95_us", "p99_us", "p99.9_us", "max_us")] + [f"{d['n']:,} ops"])

CELLS = ["i", "ii", "iii", "iv", "v", "vscan", "iii4"]
HDR = "| cell | round | arm | p0 | p25 | p50 | p95 | p99 | p99.9 | max | n |\n|---|---|---|---|---|---|---|---|---|---|---|\n"

def table(key, cells, rounds=(0, 1)):
    s = HDR
    for c in cells:
        for r in rounds:
            for a in "AB":
                d = L(c, a, r)
                if d and d.get(key):
                    f = pcms if (c == "iv" and a == "A" and key == "hot_from_schedule") else pc
                    s += f"| {c} | r{r} | {a} | {f(d[key])} |\n"
    return s

w = lambda n, t: open(f"{OUT}/{n}.md", "w").write(t)
w("hot_sched", table("hot_from_schedule", CELLS))
w("hot_svc", table("hot_service", CELLS))
w("cold_svc", table("cold_service", ["ii", "iii", "v", "iii4"]))
w("cold_sched", table("cold_from_schedule", ["ii", "iii", "v", "iii4"]))

# headline iii, iv, rounds 0-3
s = "| cell | round | arm | p50 (schedule) | p95 (schedule) | p99 (schedule) | p50 (service) | p95 (service) | p99 (service) | n completed of 20,004 due |\n|---|---|---|---|---|---|---|---|---|---|\n"
for c in ("iii", "iv"):
    for r in range(4):
        for a in "AB":
            d = L(c, a, r)
            h, v = d["hot_from_schedule"], d["hot_service"]
            f = (lambda x: ms(x)) if (c == "iv" and a == "A") else us
            s += f"| {c} | r{r} | {a} | {f(h['p50_us'])} | {f(h['p95_us'])} | {f(h['p99_us'])} | {ms(v['p50_us']) if c=='iv' and a=='A' else us(v['p50_us'])} | {ms(v['p95_us']) if c=='iv' and a=='A' else us(v['p95_us'])} | {ms(v['p99_us']) if c=='iv' and a=='A' else us(v['p99_us'])} | {h['n']:,} ops |\n"
w("headline", s)

# host
s = "| cell | round | arm | 1-min load before | 1-min load after | `wait_s` | competing before | competing after |\n|---|---|---|---|---|---|---|---|\n"
for c in CELLS:
    for r in range(4):
        for a in "AB":
            d = L(c, a, r)
            if not d: continue
            s += f"| {c} | r{r} | {a} | {d['host_before']['loadavg'].split()[0]} | {d['host_after']['loadavg'].split()[0]} | {d['wait_s']} s | {d['host_before']['competing'] or 'none'} | {d['host_after']['competing'] or 'none'} |\n"
w("host", s)

# waits
s = ("| cell | round | arm | `cpu.on_cpu` | `io.page_read` count | `io.page_read` total | mean per read | `read_share_of_one_core` | `on_cpu_share_of_one_core` | `sched.run_queue` count / total | core idle block (`meta_delta`) | `pool_would_wait` | `miss_reruns` |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
for c in CELLS:
    for r in range(4):
        for a in "AB":
            d = L(c, a, r)
            if not d: continue
            wd = d["waits_delta"]; n = d["reads_in_window"]; t = d["read_us_in_window"]
            rq = wd.get("sched.run_queue", {})
            m = d["meta_delta"]
            s += (f"| {c} | r{r} | {a} | {wd['cpu.on_cpu']['total_us']/1e6:.3f} s | {n:,} reads | {t/1e6:.3f} s | {(f'{t/n:.1f} µs' if n else '-')} | {d['read_share_of_one_core']*100:.2f} % | {d['on_cpu_share_of_one_core']*100:.2f} % | "
                  f"{rq.get('count',0):,} / {rq.get('total_us',0)/1e6:.3f} s | {m['sched_idle_block_us']/1e6:.2f} s | {d['pool_would_wait'] if a=='B' else 'absent (0)'} | {d['miss_reruns'] if a=='B' else 'absent (0)'} |\n")
w("waits", s)

# pool
s = ("| cell | round | arm | `reads_in_window` | pool misses | misses per counted read | pool hits | hits per counted read | `read_bytes` | `device_bytes_per_read` |\n|---|---|---|---|---|---|---|---|---|---|\n")
for c in CELLS:
    for r in range(4):
        for a in "AB":
            d = L(c, a, r)
            if not d: continue
            m = d["meta_delta"]; n = d["reads_in_window"]
            s += (f"| {c} | r{r} | {a} | {n:,} reads | {m['pool_misses']:,.0f} misses | {(f'{m['pool_misses']/n:.3f}' if n else '-')} | {m['pool_hits']:,.0f} hits | {(f'{m['pool_hits']/n:.2f}' if n else '-')} | {d['proc_io_delta']['read_bytes']:,} B | {(f'{d['device_bytes_per_read']:,.1f} B' if d['device_bytes_per_read'] else 'no reads counted')} |\n")
w("pool", s)

# histogram
s = "| cell | round | arm | reads by bucket k (bucket k = [2^k, 2^(k+1)) µs): count | total |\n|---|---|---|---|---|\n"
for c in ("ii", "iii", "iv", "iii4"):
    for r in range(4):
        for a in "AB":
            d = L(c, a, r)
            if not d: continue
            h = d["io_hist_delta"].get("io.page_read", {})
            ks = sorted(h, key=int)
            s += f"| {c} | r{r} | {a} | {'; '.join(f'{k}: {h[k]:,}' for k in ks)} | {sum(h.values()):,} reads |\n"
w("hist", s)

# resident spread
s = ("| cell | basis | percentile | A r0 | A r1 | B r0 | B r1 | mean B vs mean A | ranges overlap |\n|---|---|---|---|---|---|---|---|---|\n")
for c in ("i", "ii", "v", "vscan", "iii4"):
    for key, nm in (("hot_from_schedule", "schedule"), ("hot_service", "service")):
        for p, pn in (("p50_us", "p50"), ("p95_us", "p95"), ("p99_us", "p99")):
            a = [L(c, "A", r)[key][p] for r in (0, 1)]; b = [L(c, "B", r)[key][p] for r in (0, 1)]
            ov = not (min(b) > max(a) or min(a) > max(b))
            s += f"| {c} | {nm} | {pn} | {us(a[0])} | {us(a[1])} | {us(b[0])} | {us(b[1])} | {(sum(b)/sum(a)-1)*100:+.1f} % ({sum(b)/2-sum(a)/2:+.1f} µs) | {'yes' if ov else '**no**'} |\n"
w("spread", s)

# excess
s = ("| arm | round | basis | p50: (iii) - (ii) | p95: (iii) - (ii) | p99: (iii) - (ii) |\n|---|---|---|---|---|---|\n")
for a in "AB":
    for r in (0, 1):
        for key, nm in (("hot_from_schedule", "schedule"), ("hot_service", "service")):
            t = L("iii", a, r)[key]; u = L("ii", a, r)[key]
            s += f"| {a} | r{r} | {nm} | " + " | ".join(f"{t[p]-u[p]:+,.1f} µs ({t[p]/u[p]:.2f}x)" for p in ("p50_us", "p95_us", "p99_us")) + " |\n"
w("excess", s)

# iv scanner
s = ("| arm | round | scanner pages/s (`reads_in_window` / 20 s) | derived MB/s at 8,192 B | kernel `read_bytes` MB/s | hot reads completed | hot reads/s |\n|---|---|---|---|---|---|---|\n")
for r in range(4):
    for a in "AB":
        d = L("iv", a, r)
        n = d["reads_in_window"] / d["window_s"]
        s += f"| {a} | r{r} | {n:,.0f} pages/s | {n*8192/1e6:.1f} MB/s | {d['proc_io_delta']['read_bytes']/d['window_s']/1e6:.1f} MB/s | {d['hot_from_schedule']['n']:,} of 20,004 | {d['hot_from_schedule']['n']/d['window_s']:,.1f} reads/s |\n"
w("scanner", s)

# vscan
s = ("| arm | round | pool hits in window | 64-page slices/s (derived: hits / 64 / 20 s) | `cpu.on_cpu` | `sched.run_queue` count |\n|---|---|---|---|---|---|\n")
for r in (0, 1):
    for a in "AB":
        d = L("vscan", a, r)
        h = d["meta_delta"]["pool_hits"]
        s += f"| {a} | r{r} | {h:,.0f} hits | {h/64/d['window_s']:,.0f} slices/s | {d['waits_delta']['cpu.on_cpu']['total_us']/1e6:.3f} s | {d['waits_delta']['sched.run_queue']['count']:,} |\n"
w("vscan", s)

# iii4 by core
s = "| round | arm | core | p0 | p25 | p50 | p95 | p99 | p99.9 | max | n |\n|---|---|---|---|---|---|---|---|---|---|---|\n"
for r in (0, 1):
    for a in "AB":
        d = L("iii4", a, r)
        for k, v in d["hot_by_core"].items():
            s += f"| r{r} | {a} | {int(float(k))} | {pc(v)} |\n"
w("iii4core", s)
print("ok")
