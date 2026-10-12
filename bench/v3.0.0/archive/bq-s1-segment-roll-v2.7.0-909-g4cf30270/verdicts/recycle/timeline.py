import glob, re, sys, json, os
D = "/home/cdkbs/bench-runs/bq/out-4cf30270/bq"
SEG = 64 * 1024 * 1024
ap = re.compile(r"anchor published: core=(\d) checkpoint_lsn=(\d+) redo_start=(\d+) durable_lsn=(\d+) segment=(\d+)")
run = re.compile(r"run: core=(\d) completed=(\d) resumed=(\d) start_ns=(\d+) end_ns=(\d+) wall_us=(\d+).*?dirty_at_start=(\d+).*?gate_parks=(\d+).*?log_gate_run_n=(\d+)")
roll = re.compile(r"\[wal\] roll seg=(\d+) core=\d start_ns=(\d+)")
cells = sys.argv[1:] or ["c3"]
for dur in ("relaxed", "group"):
    for cell in cells:
        for f in sorted(glob.glob(f"{D}/{dur}/{cell}/r*/*.s.log")):
            print("==", f.replace(D + "/", ""))
            fold = {}
            started = False
            t0 = None
            for line in open(f):
                if "created table 'bl_ins'" in line:
                    started = True
                if "stopping on signal" in line:
                    print("   -- stop"); break
                m = run.search(line)
                if m and started:
                    c, comp, res, s, e, w, dirty, parks, lgr = m.groups()
                    t0 = t0 or int(s)
                    print(f"   run core={c} t={(int(s)-t0)/1e9:6.2f}..{(int(e)-t0)/1e9:6.2f}s wall={int(w)/1e3:7.1f}ms dirty={dirty} parks={parks} log_gate_run_n={lgr}")
                m = ap.search(line)
                if m:
                    c, ck, rs, du, sg = map(int, m.groups())
                    fold[c] = rs
                    if started:
                        mn = min(fold.values())
                        print(f"   AP core={c} ckpt_seg={ck/SEG:6.2f} redo_seg={rs/SEG:6.2f} (redo={rs}) fold_min_redo_seg={mn/SEG:6.2f} per_core={[round(fold[k]/SEG,2) for k in sorted(fold)]}")
            js = glob.glob(os.path.dirname(f) + "/*.json")
            if js:
                j = json.load(open(js[0]))
                def find(o, k):
                    if isinstance(o, dict):
                        for kk, v in o.items():
                            if kk == k: return v
                            r = find(v, k)
                            if r is not None: return r
                    return None
                for side in ("meta_m0", "meta_m1"):
                    m_ = find(j, side)
                    if isinstance(m_, dict):
                        print("  ", side, {k: m_.get(k) for k in ("checkpoint_runs", "wal_segments_removed", "wal_append_lsn")})
