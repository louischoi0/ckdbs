import glob, re, ast
rows = []
for f in sorted(g for d in ("res", "res-swap", "res-ctl", "res-ctl-swap")
                for g in glob.glob(f"/home/cdkbs/bench-runs/ew/{d}/n*.host.txt")):
    t = open(f).read()
    la = float(re.search(r"loadavg_start=([\d.]+)", t).group(1))
    bf = ast.literal_eval(re.search(r"cpu_busy_during=(\{.*\})", t).group(1))
    qb = ast.literal_eval(re.search(r"cpu_busy_1s_before=(\{.*\})", t).group(1))
    comp = re.search(r"competing_compile_or_ctest=(.*)", t).group(1)
    w = int(re.search(r"waited_s=(\d+)", t).group(1))
    rows.append((f, la, bf, qb, comp, w))
print("runs", len(rows), "loadavg(1m) at start min/max", min(r[1] for r in rows), max(r[1] for r in rows))
print("runs that started beside a compile/ctest:", sum(1 for r in rows if r[4] != "none"), " waited>0:", sum(1 for r in rows if r[5]))
oth = [max(v for k, v in r[2].items() if k not in (6, 7)) for r in rows]
oth_no2 = [max(v for k, v in r[2].items() if k not in (2, 6, 7)) for r in rows]
oth_before = [max(v for k, v in r[3].items() if k not in (6, 7)) for r in rows]
print("max busy of any CPU other than 6/7 during run: %.3f (excl CPU 2: %.3f); 1 s before start: %.3f" % (max(oth), max(oth_no2), max(oth_before)))
print("runs with another CPU > 0.5 (any CPU): %d ; excl CPU 2: %d" % (sum(1 for x in oth if x > 0.5), sum(1 for x in oth_no2 if x > 0.5)))
b = [r[2][6] for r in rows] + [r[2][7] for r in rows]
print("CPU 6/7 busy during runs min/max %.3f %.3f" % (min(b), max(b)))
print("which CPU was the max other:", sorted(set(max((v, k) for k, v in r[2].items() if k not in (6, 7))[1] for r in rows)))
