#!/usr/bin/env python3
# EP close A/B driver. Resumable: a run with a .json is skipped. Heartbeat: OUT/status.
import os, sys, subprocess, time, json, shutil
WT = "/home/ukjin/workspaces/ckdbs/.claude/worktrees"
RUN = f"{WT}/ep-run"; OUT = f"{WT}/ep-bench/out"; WORK = f"{WT}/ep-bench/work"
SERVERS = f"{WT}/ep-undo-chain-revisit/tools/eqbench/servers.sh"
EQ = f"{RUN}/eqbench.copy"; PORT = 17611
BIN = {"A": f"{RUN}/kds_server.base", "B": f"{RUN}/kds_server.close"}
sys.path.insert(0, f"{WT}/ep-undo-chain-revisit/tools")
S0 = "users=300 assets=100 seconds=6 traders=3 profit_users=20 txn=1 fk=1".split()
CELLS = {
    "s0txn": ("s0", "group", S0),
    "s0txnR": ("s0", "group", S0),
    "s0g4": ("s0", "group", "traders=4 users=1000 assets=1000 seconds=15 profit_users=20".split()),
    "s5s4": ("s5", "strict", "terminals=4 seconds=20 alone=60 warmup=5".split()),
}


def st(m):
    open(f"{OUT}/status", "w").write(f"{time.strftime('%H:%M:%S')} {m}\n")
    open(f"{OUT}/log", "a").write(f"{time.strftime('%H:%M:%S')} {m}\n")


def busy(sec):
    def snap():
        v = list(map(int, open("/proc/stat").readline().split()[1:]))
        return sum(v), v[3] + v[4]
    t0, i0 = snap(); time.sleep(sec); t1, i1 = snap()
    return 100 * (1 - (i1 - i0) / max(1, t1 - t0))


def gate():
    while True:
        b = subprocess.run(["pgrep", "-f", "cc1plus|cmake --build|ctest|kds_tests|cargo build|rustc"],
                           capture_output=True, text=True).stdout.split()
        x = busy(3.0)
        if not b and x <= 8.0:
            return x
        st(f"gate wait builds={len(b)} busy={x:.1f}"); time.sleep(5)


def meta(port):
    import kwp
    c = kwp.Connection("127.0.0.1", port, timeout=30)
    rows, tag, err = c.fetch("SELECT * FROM sys.meta")
    c.close()
    return rows, err


import threading, glob as _g


def sampler(pid, path, stop):
    f = open(path, "w")
    while not stop.is_set():
        row = []
        mhz = {}
        try:
            for l in open("/proc/cpuinfo"):
                if l.startswith("processor"):
                    c = int(l.split(":")[1])
                elif l.startswith("cpu MHz"):
                    mhz[c] = float(l.split(":")[1])
            for t in _g.glob(f"/proc/{pid}/task/*/stat"):
                x = open(t).read(); comm = x[x.index("(") + 1:x.rindex(")")]; rest = x[x.rindex(")") + 2:].split()
                psr = int(rest[36]); ut = int(rest[11]) + int(rest[12])
                if ut > 0: row.append(f"{comm}:{psr}@{mhz.get(psr,0):.0f}/{ut}")
        except Exception:
            pass
        f.write(f"{time.time():.0f} " + " ".join(sorted(row)) + "\n"); f.flush()
        stop.wait(1.0)


def one(cell, arm, i):
    tag = f"{cell}-{arm}-{i:02d}"; base = f"{OUT}/{tag}"
    if os.path.exists(base + ".json"):
        return
    sc, dur, sets = CELLS[cell]
    b = gate(); d = f"{WORK}/{tag}"; shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    open(base + ".host.txt", "w").write(time.strftime("%FT%TZ\n", time.gmtime()) + open("/proc/loadavg").read() + f"busy_pct_3s {b:.1f}\n")
    st(f"start {tag}")
    kpid = subprocess.run([SERVERS, "kds-up", BIN[arm], d, str(PORT), dur, "0-7"], capture_output=True, text=True, check=True).stdout.strip()
    args = ["taskset", "-c", "8-23", EQ, "--engine", "kds", "--kds", f"127.0.0.1:{PORT}", "--kds-durability", dur,
            "--scenario", sc, "--seed", "1", "--server-cpu", f"pid:{kpid}", "--status", f"{OUT}/status.cell",
            "--json", base + ".json.part"]
    for s in sets:
        args += ["--set", s]
    t0 = time.time()
    stop = threading.Event(); th = threading.Thread(target=sampler, args=(int(kpid), base + ".cpus", stop)); th.start()
    with open(base + ".stderr", "w") as e:
        rc = subprocess.run(args, stdout=subprocess.DEVNULL, stderr=e).returncode
    el = time.time() - t0
    stop.set(); th.join()
    try:
        rows, err = meta(PORT)
        open(base + ".meta.txt", "w").write("\n".join("|".join(map(str, r)) for r in rows) + (f"\nERR {err}" if err else "") + "\n")
    except Exception as ex:
        open(base + ".meta.txt", "w").write(f"ERR {ex}\n")
    subprocess.run(["kill", kpid])
    for _ in range(100):
        if not os.path.exists(f"/proc/{kpid}"):
            break
        time.sleep(0.1)
    else:
        subprocess.run(["kill", "-9", kpid]); time.sleep(1)
    lg = f"{d}/data/kds.log"
    if os.path.exists(lg):
        shutil.copy(lg, base + ".serverlog")
    open(base + ".rc", "w").write(f"{rc} {el:.1f}s\n")
    shutil.rmtree(d, ignore_errors=True)
    if os.path.exists(base + ".json.part"):
        os.rename(base + ".json.part", base + ".json")
    else:
        open(base + ".json", "w").write(json.dumps({"failed": True, "rc": rc}))
    st(f"done {tag} rc={rc} {el:.0f}s")


def main():
    os.makedirs(OUT, exist_ok=True); open(f"{OUT}/driver.pid", "w").write(str(os.getpid()))
    plan = json.loads(sys.argv[1])
    for i in range(1, max(plan.values()) + 1):
        for cell, n in plan.items():
            if i <= n:
                for arm in ("BA" if cell.endswith("R") else "AB"):
                    one(cell, arm, i)
    st("ALL DONE"); open(f"{OUT}/all.done", "w").write("")


main()
