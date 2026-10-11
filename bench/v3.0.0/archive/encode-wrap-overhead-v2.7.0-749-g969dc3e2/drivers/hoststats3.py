import glob, re, ast, sys, collections
dirs = sys.argv[1:]
c_before = collections.Counter(); c_during = collections.Counter()
n_flag_excl2 = 0; n = 0
for f in sorted(g for d in dirs for g in glob.glob(f"/home/cdkbs/bench-runs/ew/{d}/n*.host.txt")):
    t = open(f).read()
    bf = ast.literal_eval(re.search(r"cpu_busy_during=(\{.*\})", t).group(1))
    qb = ast.literal_eval(re.search(r"cpu_busy_1s_before=(\{.*\})", t).group(1))
    n += 1
    for k, v in qb.items():
        if k not in (6, 7) and v > 0.5:
            c_before[k] += 1
    for k, v in bf.items():
        if k not in (6, 7) and v > 0.5:
            c_during[k] += 1
    if any(v > 0.5 for k, v in list(qb.items()) + list(bf.items()) if k not in (2, 6, 7)):
        n_flag_excl2 += 1
print("runs", n, "CPUs >0.5 before:", dict(c_before), "during:", dict(c_during), "runs flagged excluding CPU 2:", n_flag_excl2)
