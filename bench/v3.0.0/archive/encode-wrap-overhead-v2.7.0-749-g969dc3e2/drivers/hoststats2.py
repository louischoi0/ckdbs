import glob, re, ast, sys
dirs = sys.argv[1:]
rows = []
for f in sorted(g for d in dirs for g in glob.glob(f"/home/cdkbs/bench-runs/ew/{d}/n*.host.txt")):
    t = open(f).read()
    la = float(re.search(r"loadavg_start=([\d.]+)", t).group(1))
    bf = ast.literal_eval(re.search(r"cpu_busy_during=(\{.*\})", t).group(1))
    qb = ast.literal_eval(re.search(r"cpu_busy_1s_before=(\{.*\})", t).group(1))
    comp = re.search(r"competing_compile_or_ctest=(.*)", t).group(1)
    mb = max(v for k, v in qb.items() if k not in (6, 7))
    md = max(v for k, v in bf.items() if k not in (6, 7))
    rows.append((f.split("ew/")[1], la, mb, md, comp))
print("runs", len(rows), "loadavg(1m) at start min/max", min(r[1] for r in rows), max(r[1] for r in rows))
print("competing:", sum(1 for r in rows if r[4] != "none"))
print("max other CPU busy 1 s before start %.3f, during %.3f" % (max(r[2] for r in rows), max(r[3] for r in rows)))
for r in rows:
    if max(r[2], r[3]) > 0.5:
        print("FLAG", r)
