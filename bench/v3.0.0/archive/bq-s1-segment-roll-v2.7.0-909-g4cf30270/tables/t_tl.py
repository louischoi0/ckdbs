import glob, re, sys
D = __import__("os").environ.get("BQ_OUT", "/home/cdkbs/bench-runs/bq/out-4cf30270") + "/bq"
SEG = 64 * 1024 * 1024
ap = re.compile(r"anchor published: core=(\d) checkpoint_lsn=(\d+) redo_start=(\d+) durable_lsn=(\d+) segment=(\d+)")
run = re.compile(r"run: core=(\d) completed=(\d) resumed=(\d) start_ns=(\d+) end_ns=(\d+) wall_us=(\d+).*?dirty_at_start=(\d+).*?gate_parks=(\d+).*?log_gate_run_n=(\d+)")
dur, cell, limit = sys.argv[1], sys.argv[2], int(sys.argv[3])
print('| run | core | starts at | wall | dirty pages at start | log-gate waits inside the run | redo start published (segment) | instance redo start, the fold minimum (segment) |')
print('|---|---|---|---|---|---|---|---|')
for f in sorted(glob.glob(f"{D}/{dur}/{cell}/r*/*.s.log")):
    rn = f.split('/')[-2]
    fold = {}
    started = False
    t0 = None
    pend = None
    n = 0
    last = None
    for line in open(f):
        if "created table 'bl_ins'" in line:
            started = True
        if "stopping on signal" in line:
            break
        m = ap.search(line)
        if m:
            c, ck, rs, du, sg = map(int, m.groups())
            fold[c] = rs
            mn = min(fold.values()) if len(fold) == 4 else 0
            last = (c, rs, mn)
        m = run.search(line)
        if m and started:
            c, comp, res, s_, e, w, dirty, parks, lgr = m.groups()
            t0 = t0 or int(s_)
            if n < limit and last and last[0] == int(c):
                print(f'| {rn} | {c} | {(int(s_) - t0) / 1e9:.2f} s | {int(w) / 1e6:.2f} s | {int(dirty):,} | {lgr} | {last[1] / SEG:.2f} | {last[2] / SEG:.2f} |')
            n += 1
            last = None
