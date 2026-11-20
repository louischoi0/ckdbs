#!/usr/bin/env python3
"""Pass 2 of the DL close: every cell run alone, each only while the host is quiet.

Before a cell: wait until the last QUIET_S seconds of the sampler show no foreign build/suite/server.
After a cell: if any sampler line inside the cell's window shows a foreign process, or any row carries
busy/other_servers != 0, the cell's rows are discarded (kept in rejected.jsonl) and it is retried,
up to TRIES times.  Waiting is logged to hold.log (outside the budget); running time to run.log.
"""
import json, os, subprocess, sys, time

D = "/home/louis/bench-runs/dl-close"
S = "/tmp/claude-1000/-home-louis-ckdbs/e799d8bb-d13e-4b45-a458-1ebc557349e3/scratchpad"
QUIET_S = 20
TRIES = 4
ORDER = ["select1", "select8", "fk1", "select8off", "cabin8", "join8i", "evict8", "storm8",
         "stormA8", "pool8x", "join8ix"]
SAMP = f"{D}/sampler2.log"
PAT = "cc1plus|cmake --build|ctest|kds_|\\.py"


def foreign():
    out = subprocess.run(
        f"ps -eo args | grep -E '{PAT}' | grep -v -E 'grep|sampler|pass2|dl-close/kds_|dl_close|build-sandbox|sandbox_server|networkd|unattended'",
        shell=True, capture_output=True, text=True).stdout.strip().splitlines()
    return len(out)


def sample_loop_once():
    n = foreign()
    with open(SAMP, "a") as f:
        f.write(f"{time.time():.1f} {time.strftime('%T', time.gmtime())} load={open('/proc/loadavg').read().split()[0]} foreign={n}\n")
    return n


def log(name, msg):
    with open(f"{D}/{name}", "a") as f:
        f.write(time.strftime("%FT%TZ ", time.gmtime()) + msg + "\n")


def wait_quiet(cell):
    t0 = time.time()
    clean_since = None
    while True:
        n = sample_loop_once()
        if n == 0:
            clean_since = clean_since or time.time()
            if time.time() - clean_since >= QUIET_S:
                break
        else:
            clean_since = None
        with open(f"{D}/pass2.status", "w") as f:
            f.write(time.strftime("%FT%TZ ", time.gmtime()) + f"{cell} holding {int(time.time()-t0)}s foreign={n}\n")
        time.sleep(2)
    held = int(time.time() - t0)
    log("hold.log", f"{cell} held {held}s")
    return held


def main():
    port = 29000
    for cell in ORDER:
        for attempt in range(1, TRIES + 1):
            wait_quiet(cell)
            out = f"{D}/p2-{cell}-try{attempt}.jsonl"
            port += 40
            t0 = time.time()
            p = subprocess.Popen(
                ["python3", f"{S}/dl_close.py", "--bins",
                 f"a={D}/kds_a,b={D}/kds_b,x={D}/kds_x", "--tools", "/home/louis/ckdbs/tools",
                 "--wd", f"{D}/run2", "--out", out, "--status", f"{D}/run.status",
                 "--cells", cell, "--port", str(port)],
                stdout=open(f"{D}/p2-{cell}-try{attempt}.log", "w"), stderr=subprocess.STDOUT, cwd=S)
            dirty = 0
            while p.poll() is None:
                if sample_loop_once() > 0:
                    dirty += 1
                with open(f"{D}/pass2.status", "w") as f:
                    f.write(time.strftime("%FT%TZ ", time.gmtime()) +
                            f"{cell} try {attempt} running dirty_samples={dirty} {open(D + '/run.status').read().strip()}\n")
                time.sleep(2)
            t1 = time.time()
            rows = [json.loads(l) for l in open(out)] if os.path.exists(out) else []
            rowdirty = sum(1 for r in rows if r["busy"] not in ("0", "") or r["other_servers"] not in ("0", ""))
            ok = p.returncode == 0 and rows and dirty == 0 and rowdirty == 0
            log("run.log", f"{cell} try {attempt} rows={len(rows)} secs={int(t1-t0)} dirty_samples={dirty} dirty_rows={rowdirty} -> {'ACCEPT' if ok else 'REJECT'}")
            if ok:
                os.replace(out, f"{D}/p2-{cell}.jsonl")
                break
            os.replace(out, f"{D}/rejected-{cell}-try{attempt}.jsonl") if os.path.exists(out) else None
        else:
            log("run.log", f"{cell} NOT CLEAN after {TRIES} tries")
            # keep the last try's rows for the record
    open(f"{D}/pass2.done", "w").write(time.strftime("%FT%TZ", time.gmtime()))


main()
