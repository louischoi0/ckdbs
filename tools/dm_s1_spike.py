#!/usr/bin/env python3
"""DM-S1's ceiling: U1's memo against the base on M1's and M3's cells.

`instructions/v3.2.0/workorder-dm-leaf-upper-bound.md` section 5 (DM-S1).
Needs a server built from the spike (branch `worktree-dm-s0-spike`, never
landed) whose memo `DM_MEMO_ENTRIES` sizes (0 is off: today's path). One
binary serves every arm, so an arm differs from another by the memo alone.

Warm (`--warm-cores`, a pool that holds the relation): the indexed insert and
the point update, closed loop, server CPU a statement; arms interleaved,
`--lifetimes` fresh servers an arm.
Cold (cores = 1, `--cold-rows` rows on `--cold-frames` frames, DM-S0's M3):
the same two shapes beside one point reader; the loaded data directory is
copied back before every lifetime, so every arm starts from the same bytes.

    dm_s1_spike.py --bin B --workdir D --out OUT.json [--arms 0,1024,16384]
"""
import argparse, hashlib, json, os, random, shutil, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm_s0_premise as s0  # noqa: E402


def env_for(entries):
    env = dict(os.environ)
    env["DM_MEMO_ENTRIES"] = str(entries)
    return env


def drop_page_cache(d):
    for name in os.listdir(d):
        path = os.path.join(d, name)
        fd = os.open(path, os.O_RDONLY)
        try:
            os.fsync(fd)
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        finally:
            os.close(fd)


def rss_kb(pid):
    for line in open(f"/proc/{pid}/status"):
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    return None


def cgroup_events(pid):
    # memory.high throttling events of the server's scope: the cap's own
    # cost, which a real-device cell must carry beside its numbers.
    try:
        rel = open(f"/proc/{pid}/cgroup").read().strip().split("::", 1)[1]
        out = {}
        for line in open(f"/sys/fs/cgroup{rel}/memory.events"):
            k, v = line.split()
            out[k] = int(v)
        return out
    except (OSError, IndexError, ValueError):
        return None


def shapes(n):
    return {
        "ins_ix1": lambda r: s0.ins("t1", r),
        "update_pk": lambda r: f"UPDATE t1 SET w = w + 1 WHERE id = {2 * r.randrange(1, n + 1)}",
    }


def warm(a, beat):
    out = []
    n = a.warm_rows
    for life in range(a.lifetimes):
        for cores in [int(x) for x in a.warm_cores.split(",")]:
            for arm in a.arms:
                srv = s0.Server(a, cores, f"warm_c{cores}_m{arm}", 131072, env=env_for(arm))
                try:
                    c = srv.conn()
                    s0.make(c, "t1", n, 1, random.Random(31))
                    c.close()
                    for name, gen in shapes(n).items():
                        res = s0.closed_loop(srv, cores * a.sessions_per_core, a.seconds, gen)
                        res.update({"life": life, "cores": cores, "arm": arm, "shape": name})
                        out.append(res)
                        beat(f"warm life={life} cores={cores} arm={arm} {name} "
                             f"cpu/w={res['server_cpu_us_per_write']}")
                finally:
                    srv.stop()
    return out


def cold(a, beat):
    n = a.cold_rows
    base = a.cold_base or os.path.join(a.workdir, "cold_base")
    if not a.cold_base:
        loader = s0.Server(a, 1, "cold_base", 131072, env=env_for(0))
        try:
            c = loader.conn()
            s0.make(c, "t1", n, 1, random.Random(41))
            c.close()
        finally:
            loader.stop()
    beat("cold loaded")
    out = []
    reader = lambda r: f"SELECT * FROM t1 WHERE id = {2 * r.randrange(1, n + 1)}"
    for life in range(a.lifetimes):
        for arm in a.arms:
            for name, gen in shapes(n).items():
                if a.shapes and name not in a.shapes.split(","):
                    continue
                tag = "cold_run"
                d = os.path.join(a.workdir, tag)
                shutil.rmtree(d, ignore_errors=True)
                shutil.copytree(base, d)
                wrap = None
                if a.mem_max:
                    # The real-device regime: the copy leaves the OS page cache,
                    # and the server runs under a memory cap below its RSS plus
                    # the data file, so a pool miss is a device read (the file
                    # is opened without O_DIRECT, `file_page_device.hpp`).
                    drop_page_cache(d)
                    wrap = ["systemd-run", "--user", "--scope", "-q", "-p",
                            f"MemoryHigh={a.mem_max}", "-p", "MemorySwapMax=0"]
                srv = s0.Server(a, 1, tag, a.cold_frames, fresh=False, env=env_for(arm),
                                wrap=wrap)
                try:
                    res = s0.closed_loop(srv, a.sessions_per_core, a.seconds, gen,
                                         readers=1, gen_reader=reader)
                    res["server_rss_kb"] = rss_kb(srv.proc.pid)
                    res["cgroup_memory_events"] = cgroup_events(srv.proc.pid)
                    res["mem_max"] = a.mem_max
                    res.update({"life": life, "cores": 1, "arm": arm, "shape": name})
                    out.append(res)
                    beat(f"cold life={life} arm={arm} {name} writes={res['writes']} "
                         f"rp99={res['reader_latency_us']['p99']}")
                finally:
                    srv.stop()
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--bin", required=True)
    p.add_argument("--workdir", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--port", type=int, default=15771)
    p.add_argument("--arms", default="0,1024,16384")
    p.add_argument("--lifetimes", type=int, default=3)
    p.add_argument("--warm-rows", type=int, default=50_000)
    p.add_argument("--warm-cores", default="1,4")
    p.add_argument("--sessions-per-core", type=int, default=4)
    p.add_argument("--seconds", type=float, default=6.0)
    p.add_argument("--cold-rows", type=int, default=1_000_000)
    p.add_argument("--cold-frames", type=int, default=2048)
    p.add_argument("--only", default="warm,cold")
    p.add_argument("--shapes", default=None, help="cold shapes to run, e.g. ins_ix1")
    p.add_argument("--mem-max", default=None, help="e.g. 96M: the cold cells' memory cap")
    p.add_argument("--cold-base", default=None, help="reuse a loaded cold_base directory")
    p.add_argument("--heartbeat", default=None)
    a = p.parse_args()
    a.arms = [int(x) for x in a.arms.split(",")]
    os.makedirs(a.workdir, exist_ok=True)

    def beat(line):
        msg = f"{time.strftime('%H:%M:%S', time.gmtime())} {line}"
        print(msg, flush=True)
        if a.heartbeat:
            with open(a.heartbeat, "a") as f:
                f.write(msg + "\n")

    report = {"bin": a.bin, "sha256": hashlib.sha256(open(a.bin, "rb").read()).hexdigest(),
              "started": time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime()),
              "host_before": s0.host_state(), "args": vars(a)}

    def save():
        with open(a.out, "w") as f:
            json.dump(report, f, indent=1)

    beat("start")
    only = a.only.split(",")
    if "warm" in only:
        report["warm"] = warm(a, beat)
        save()
    if "cold" in only:
        report["cold"] = cold(a, beat)
    report["host_after"] = s0.host_state()
    report["ended"] = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())
    save()
    beat("end")


if __name__ == "__main__":
    main()
