#!/usr/bin/env python3
"""Named tables for the BK close document: reads run/analysis/*.json, writes run/analysis/tbl/<name>.md.
usage: assemble_bk.py <analysis dir>
"""
import json, os, random, statistics, sys

D = sys.argv[1]
T = os.path.join(D, "tbl")
os.makedirs(T, exist_ok=True)
SHAPES = [("insert", "INSERT, pk omitted"), ("insert-again", "INSERT again (replicate)"),
          ("insert-named", "INSERT, pk named (keys below 2^40)"), ("select-pk", "SELECT by pk"),
          ("update-pk", "UPDATE by pk"), ("update-hot", "UPDATE of one hot row"), ("delete-pk", "DELETE by pk"),
          ("range100", "COUNT/MIN/MAX over 100 ids (walks the relation)"), ("ping", "SHOW META (control)")]
SIZES = (200, 1000, 10000)
rng = random.Random(1)


def load(n):
    p = os.path.join(D, n)
    return json.load(open(p)) if os.path.exists(p) else None


def emit(name, text):
    open(os.path.join(T, name + ".md"), "w").write(text.rstrip() + "\n")


def matrix(js, fn, sizes=SIZES, shapes=SHAPES):
    if js is None:
        return "not run"
    out = ["| shape | " + " | ".join(f"{n:,} rows" for n in sizes) + " |", "|---|" + "---|" * len(sizes)]
    for s, desc in shapes:
        cells = []
        for n in sizes:
            c = js.get(f"{n}|{s}")
            cells.append(fn(c, s) if c else "not run")
        out.append(f"| {desc} | " + " | ".join(cells) + " |")
    return "\n".join(out)


def th(c, s):
    return f"{1e6 / c['pa']:,.0f} → {1e6 / c['pb']:,.0f} /s ({c['dth']:+.1f} %)"


def d(c, s, bold=True):
    t = f"{c['dmed']:+.2f} µs [{c['ci'][0]:+.2f}, {c['ci'][1]:+.2f}]"
    return f"**{t}**" if (c.get("resolved") and bold) else t


def dn(c, s):
    return d(c, s, False)


def boot_diff(x, y, n=4000):
    bs = sorted(statistics.median(rng.choices(x, k=len(x))) - statistics.median(rng.choices(y, k=len(y))) for _ in range(n))
    return statistics.median(x) - statistics.median(y), bs[int(0.025 * n)], bs[int(0.975 * n)]


c1, c2, nl, nl2, st, fl = (load(x) for x in ("c1.json", "c2.json", "null.json", "null2.json", "strict.json", "flip.json"))
p1, p2 = load("c1p1.json"), load("c1p2.json")
for nm, js in (("c1", c1), ("c2", c2)):
    emit(f"th-{nm}", matrix(js, th))
    emit(f"d-{nm}", matrix(js, d))
    emit(f"p50-{nm}", matrix(js, lambda c, s: f"{c['pa']:.1f} → {c['pb']:.1f} µs"))
    emit(f"cpu-{nm}", matrix(js, lambda c, s: f"{c['cpuA']:.2f} → {c['cpuB']:.2f} µs (Δ {c['dcpu']:+.2f} [{c['cicpu'][0]:+.2f}, {c['cicpu'][1]:+.2f}])"))
    emit(f"floor-{nm}", matrix(js, lambda c, s: f"{c['floor']:.2f} µs"))
emit("null-c1", matrix(nl, dn))
emit("null-c2", matrix(nl2, dn))
emit("pass1", matrix(p1, dn))
emit("pass2", matrix(p2, dn))
emit("strict-p50", matrix(st, lambda c, s: f"{c['pa']:.0f} → {c['pb']:.0f} µs"))
emit("strict-d", matrix(st, d))
emit("flip-d", matrix(fl, dn, sizes=(1000, 10000)))


def did(js, nulljs, label):
    if js is None or nulljs is None:
        return "not run"
    out = ["| shape | " + " | ".join(f"{n:,} rows" for n in SIZES) + " |", "|---|" + "---|" * len(SIZES)]
    for s, desc in SHAPES:
        cells = []
        for n in SIZES:
            a, b = js.get(f"{n}|{s}"), nulljs.get(f"{n}|{s}")
            if not a or not b:
                cells.append("not run")
                continue
            m, lo, hi = boot_diff(a["d50_runs"], b["d50_runs"])
            t = f"{m:+.2f} µs [{lo:+.2f}, {hi:+.2f}]"
            cells.append(f"**{t}**" if (lo > 0 or hi < 0) and round(abs(m), 2) > 0.6 else t)
        out.append(f"| {desc} | " + " | ".join(cells) + " |")
    return "\n".join(out)


emit("did-c1", did(c1, nl, "c1"))
emit("did-c2", did(c2, nl2, "c2"))

named = []
for nm, js in (("cores = 1", c1), ("cores = 2", c2)):
    if js is None:
        continue
    for n in SIZES:
        h = js.get(f"{n}|named-hi-minus-lo")
        x = js.get(f"{n}|named-hi-minus-A-named")
        if h:
            named.append(f"| {nm} | {n:,} | {h['lo']:.1f} µs | {h['hi']:.1f} µs | {h['dmed']:+.2f} µs [{h['ci'][0]:+.2f}, {h['ci'][1]:+.2f}] | "
                         f"{h['dmean']:+.2f} µs [{h['cimean'][0]:+.2f}, {h['cimean'][1]:+.2f}] | {h['dcpu']:+.2f} µs [{h['cicpu'][0]:+.2f}, {h['cicpu'][1]:+.2f}] | "
                         f"{x['dmed']:+.2f} µs [{x['ci'][0]:+.2f}, {x['ci'][1]:+.2f}] |")
emit("named", "| cores | rows | lo p50 | hi p50 | Δ p50, hi − lo | Δ mean | Δ server CPU per statement | hi against A's low-key named insert (cross-arm) |\n"
     "|---|---|---|---|---|---|---|---|\n" + "\n".join(named))

# ---- previous closes
pc = json.load(open("/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad/prev_close.json"))
rows_bj = []
for cell, js, label in (("c1", c1, "cores = 1"), ("c2", c2, "cores = 2")):
    for n in SIZES:
        for s, desc in (("update-pk", "UPDATE by pk"), ("insert-named", "INSERT, pk named"), ("select-pk", "SELECT by pk"),
                        ("delete-pk", "DELETE by pk"), ("ping", "SHOW META (control)")):
            k = f"BJ|{cell}|{n}|{s}|B"
            if k not in pc or js is None or f"{n}|{s}" not in js:
                continue
            bj = pc[k][0]
            a, b = js[f"{n}|{s}"]["pa"], js[f"{n}|{s}"]["pb"]
            rows_bj.append(f"| {label}, {n:,} rows | {desc} | {bj:.2f} µs | {a:.2f} µs ({a - bj:+.2f}) | {b:.2f} µs ({b - bj:+.2f}) |")
emit("prev-bj", "| cell | shape | BJ close B' (757), median of runs | this A (763), same binary | this B (767) |\n|---|---|---|---|---|\n" + "\n".join(rows_bj))
rows_bh = []
for cell, js, label in (("c1", c1, "cores = 1"), ("c2", c2, "cores = 2")):
    for n in SIZES:
        for s, desc in (("insert", "INSERT, pk omitted"), ("insert-again", "INSERT again"), ("select-pk", "SELECT by pk"),
                        ("update-pk", "UPDATE by pk"), ("delete-pk", "DELETE by pk"), ("range100", "COUNT/MIN/MAX over 100 ids"),
                        ("ping", "SHOW META (control)")):
            k = f"BH|{cell}|{n}|{s}|B"
            if k not in pc or js is None or f"{n}|{s}" not in js:
                continue
            bh = pc[k][0]
            a, b = js[f"{n}|{s}"]["pa"], js[f"{n}|{s}"]["pb"]
            rows_bh.append(f"| {label}, {n:,} rows | {desc} | {bh:.2f} µs | {a:.2f} µs ({a - bh:+.2f}) | {b:.2f} µs ({b - bh:+.2f}) |")
emit("prev-bh", "| cell | shape | BH close B' (712), median of runs | this A (763) | this B (767) |\n|---|---|---|---|---|\n" + "\n".join(rows_bh))

# ---- waits: relaxed p50 against the server's CPU, and strict against relaxed
def waits():
    if c1 is None:
        return "not run"
    rows = ["| shape | relaxed p50 (A) | server CPU per statement (A) | outside the server's CPU (client, loopback, wake-up) | " +
            "strict p50 (A), rows as stated | commit wait: strict − relaxed p50 | commit wait as a share of the strict p50 |",
            "|---|---|---|---|---|---|---|"]
    for s_, desc in (("insert", "INSERT, pk omitted"), ("insert-named", "INSERT, pk named"), ("update-pk", "UPDATE by pk"),
                     ("update-hot", "UPDATE of one hot row"), ("delete-pk", "DELETE by pk"), ("select-pk", "SELECT by pk"),
                     ("range100", "COUNT/MIN/MAX over 100 ids"), ("ping", "SHOW META (control)")):
        c = c1.get(f"1000|{s_}")
        if not c:
            continue
        strict_txt, wait_txt, share_txt = "not run", "-", "-"
        if st:
            parts, wparts, sparts = [], [], []
            for n in SIZES:
                x, r_ = st.get(f"{n}|{s_}"), c1.get(f"{n}|{s_}")
                if x and r_:
                    parts.append(f"{x['pa']:.0f} µs ({n:,} rows)")
                    wparts.append(f"{x['pa'] - r_['pa']:+.0f} µs")
                    sparts.append(f"{(x['pa'] - r_['pa']) / x['pa'] * 100:.0f} %")
            if parts:
                strict_txt, wait_txt, share_txt = "; ".join(parts), "; ".join(wparts), "; ".join(sparts)
        rows.append(f"| {desc} | {c['pa']:.1f} µs | {c['cpuA']:.1f} µs | {c['pa'] - c['cpuA']:.1f} µs | {strict_txt} | {wait_txt} | {share_txt} |")
    return "\n".join(rows)


emit("waits", waits())


# ---- both orientations: (Delta in the main order + Delta with the slots exchanged) / 2, per shape, 1,000 and 10,000 rows
def orient():
    if c1 is None or fl is None:
        return "not run"
    sizes = (1000, 10000)
    out = ["| shape | " + " | ".join(f"{n:,} rows: A in slot A | B in slot A (slots exchanged) | average (95 % CI)" for n in sizes) + " |",
           "|---|" + "---|" * len(sizes)]
    for s_, desc in SHAPES:
        cells = []
        for n in sizes:
            a, b = c1.get(f"{n}|{s_}"), fl.get(f"{n}|{s_}")
            if not a or not b:
                cells.append("not run")
                continue
            x, y = a["d50_runs"], b["d50_runs"]
            bs = sorted((statistics.median(rng.choices(x, k=len(x))) + statistics.median(rng.choices(y, k=len(y)))) / 2 for _ in range(4000))
            m = (statistics.median(x) + statistics.median(y)) / 2
            lo, hi = bs[100], bs[3900]
            t = f"{m:+.2f} µs [{lo:+.2f}, {hi:+.2f}]"
            cells.append(f"{a['dmed']:+.2f} µs | {b['dmed']:+.2f} µs | " + (f"**{t}**" if (lo > 0 or hi < 0) and round(abs(m), 2) > 0.6 else t))
        out.append(f"| {desc} | " + " | ".join(cells) + " |")
    return "\n".join(out)


emit("orient", orient())

print("tables:", sorted(os.listdir(T)))
