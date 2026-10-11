#!/usr/bin/env python3
"""Quiet-gated runner for the BK close matrix: one driver invocation per (cell, rows, run), started only after the
host has been quiet for `--quiet-windows` consecutive 3 s windows, and a finished run that meets a discard rule that
needs no cell reference (build/test process, other-CPU busy, sibling busy, errors, counts, probes) is moved to
<dir>/discarded/ and re-run at the same run index, up to --retries times.

usage: runner.py <cell> [<cell> ...]     cells: c1 c1r null c2 c2r strict
"""
import argparse, json, os, shutil, subprocess, sys, time

SP = "/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad"
R = f"{SP}/run"
BIN = f"{R}/bin"
sys.path.insert(0, SP)
import analyze_bk as ab  # noqa
import bk_overhead_ab as drv  # noqa

CELLS = {
    # name: (dir, cores, pin-server, binA, binB, durability, ops, rows, run range, new_is_old, workdir)
    "c1": ("overhead-c1", 1, "2", "A", "B", "relaxed", 3000, (200, 1000, 10000), (0, 16), False),
    "c1r": ("overhead-c1", 1, "2", "A", "B", "relaxed", 3000, (200, 1000, 10000), (16, 32), False),
    "null": ("null-c1", 1, "2", "A", "A2", "relaxed", 3000, (200, 1000, 10000), (0, 16), "null"),
    "c2": ("overhead-c2", 2, "2,3", "A", "B", "relaxed", 3000, (200, 1000, 10000), (0, 16), False),
    "c2r": ("overhead-c2", 2, "2,3", "A", "B", "relaxed", 3000, (200, 1000, 10000), (16, 32), False),
    "strict": ("strict-c1", 1, "2", "A", "B", "strict", 600, (200, 1000, 10000), (0, 6), False),
    "null2": ("null-c2", 2, "2,3", "A", "A2", "relaxed", 3000, (200, 1000, 10000), (0, 16), "null"),
    "nullB": ("nullB-c1", 1, "2", "B", "B2", "relaxed", 3000, (10000,), (0, 16), "nullnew"),
    "nullA": ("nullA-c1", 1, "2", "A", "A3", "relaxed", 3000, (10000,), (0, 16), "null"),
    "fresh": ("fresh-c1", 1, "2", "A3", "B2", "relaxed", 3000, (1000, 10000), (0, 16), False),
    "flip": ("flip-c1", 1, "2", "B", "A", "relaxed", 3000, (1000, 10000), (0, 16), "flip"),
}
BAD = ("cc1plus", "cmake", "ld", "collect2", "make", "ninja", "cc1")   # ctest and kds_tests: judged by the CPU-busy gate (amended)


def bad_procs():
    out = []
    for n in BAD:
        r = subprocess.run(["pgrep", "-x", n], capture_output=True, text=True)
        if r.stdout.strip():
            out.append(n)
    return out


def quiet_wait(self_cpus, thr, windows, max_wait):
    t0 = time.time()
    ok = 0
    while time.time() - t0 < max_wait:
        a = drv.cpu_times(); time.sleep(3); b = drv.cpu_times()
        bf = drv.busy_frac(a, b)
        others = [v for c, v in bf.items() if c not in self_cpus]
        m = sum(others) / len(others)
        bp = bad_procs()
        if m <= thr and not bp:
            ok += 1
            if ok >= windows:
                return True, round(time.time() - t0)
        else:
            ok = 0
    return False, round(time.time() - t0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cells", nargs="+")
    ap.add_argument("--thr", type=float, default=0.04)
    ap.add_argument("--quiet-windows", type=int, default=3)
    ap.add_argument("--max-wait", type=float, default=7200)
    ap.add_argument("--retries", type=int, default=6)
    a = ap.parse_args()
    for name in a.cells:
        d, cores, pin, ba, bb_, dur, ops, rows_l, (r_lo, r_hi), new_is_old = CELLS[name]
        out = f"{R}/{d}"
        os.makedirs(out, exist_ok=True)
        # 6,7: another session's pinned benchmark (analyze_bk.py rule 2); 1: at cores = 2 the engine pins its second reactor there
        self_cpus = {int(x) for x in pin.split(",")} | {4, 6, 7} | ({1} if cores == 2 else set())
        for rows in rows_l:
            for run in range(r_lo, r_hi):
                tag = f"c{cores}-n{rows}-r{run}"
                for attempt in range(a.retries + 1):
                    if os.path.exists(f"{out}/{tag}.json"):
                        break
                    ok, waited = quiet_wait(self_cpus, a.thr, a.quiet_windows, a.max_wait)
                    print(f"{time.strftime('%H:%M:%S')} {name} {tag} attempt {attempt} quiet={ok} waited={waited}s", flush=True)
                    cmd = ["taskset", "-c", "4", "python3", f"{SP}/bk_overhead_ab.py", "--rows", str(rows), "--runs", "1",
                           "--run-start", str(run), "--ops", str(ops), "--cores", str(cores), "--durability", dur,
                           "--pin-server", pin, "--bin-a", f"{BIN}/kds_server-{ba}", "--bin-b", f"{BIN}/kds_server-{bb_}",
                           "--port-a", "32011", "--port-b", "32012", "--workdir", f"{R}/w-{name}", "--out", out,
                           "--max-other-busy", "0.06", "--max-wait", "0"]   # the driver's own one-window gate; the wait is above
                    r = subprocess.run(cmd, capture_output=True, text=True)
                    print(r.stdout.strip()[-400:], r.stderr.strip()[-300:], flush=True)
                    if not os.path.exists(f"{out}/{tag}.json"):
                        continue
                    j = json.load(open(f"{out}/{tag}.json"))
                    j["_raw"] = f"{out}/{tag}.raw.json.gz"
                    rs = ab.reasons(j, float("inf"), new_is_old)  # new_is_old is the mode: False = main, 'null', 'flip'
                    if rs:
                        dd = f"{out}/discarded"
                        os.makedirs(dd, exist_ok=True)
                        for ext in (".json", ".raw.json.gz"):
                            shutil.move(f"{out}/{tag}{ext}", f"{dd}/{tag}-a{attempt}{ext}")
                        with open(f"{dd}/{tag}-a{attempt}.reasons.txt", "w") as f:
                            f.write("\n".join(rs) + "\n")
                        print(f"   discarded: {'; '.join(rs)[:160]}", flush=True)
    print("runner done", flush=True)


if __name__ == "__main__":
    sys.exit(main())
