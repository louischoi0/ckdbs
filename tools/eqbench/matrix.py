#!/usr/bin/env python3
"""EN-S4's measurement: every scenario cell on KDS, PostgreSQL and Oracle,
rounds interleaved, one JSON per cell and round under --out.

Not timed itself: it starts servers, gates on a quiet host, runs
`eqbench` pinned to the harness CPUs, and records the host's load per cell
(bench/README.md rules 1-5). Re-running it with the same --out resumes:
a cell whose JSON exists is skipped. `watchdog.sh` re-launches it if its
status file stops moving and its pid is gone (CLAUDE.md, "A running
measurement is watched every minute").

    matrix.py --out DIR --kds-bin COPY --ora-home DIR [--rounds 3]
              [--only s1,s5] [--engines kds,pg,oracle] [--pass timed|profile]

The `profile` pass runs PostgreSQL alone with pg_stat_statements,
track_io_timing and the wait sampler (EN-Q8): KDS's and Oracle's
accounting is always on, so their breakdown comes from the timed pass.
"""

import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
EQ = os.path.join(HERE, "target", "release", "eqbench")
SERVERS = os.path.join(HERE, "servers.sh")
SERVER_CPUS = "0-7"
HARNESS_CPUS = "8-23"
KDS_PORT = 15792
PG_PORT = 15434

# (scenario, cell name, --set options, KDS durabilities). Every cell runs on
# PostgreSQL and Oracle once at their defaults, and on KDS once per rung.
CELLS = [
    ("s0", "one-trader", {}, ["group", "strict"]),
    ("s0", "four-traders", {"traders": "4"}, ["group"]),
    ("s1", "bars60480", {"bars": "60480"}, ["group", "strict"]),
    ("s1", "bars252", {"bars": "252"}, ["group"]),
    ("s1", "bars1008", {"bars": "1008"}, ["group"]),
    ("s1", "bars10080", {"bars": "10080"}, ["group"]),
    ("s2", "one-booker", {}, ["group", "strict"]),
    ("s2", "four-bookers", {"bookers": "4"}, ["group"]),
    ("s3", "loans200-unindexed", {"loans": "200", "indexed": "0"}, ["group"]),
    ("s3", "loans200-indexed", {"loans": "200", "indexed": "1"}, ["group"]),
    ("s3", "loans1000-unindexed", {"loans": "1000", "indexed": "0"}, ["group"]),
    ("s3", "loans1000-indexed", {"loans": "1000", "indexed": "1"}, ["group"]),
    ("s3", "loans10000-unindexed", {"loans": "10000", "indexed": "0"}, ["group"]),
    ("s3", "loans10000-indexed", {"loans": "10000", "indexed": "1"}, ["group"]),
    ("s4", "day1", {}, ["group"]),
    ("s5", "one-terminal", {"terminals": "1"}, ["group", "strict"]),
    ("s5", "four-terminals", {"terminals": "4"}, ["group", "strict"]),
]
# KDS-only arms of a cell: (cell suffix, extra --set) - scenario 4's Cabin arms.
# The `off` arm is the cell itself (no suffix), so report.py sets it beside
# PostgreSQL's and Oracle's `off`, the one arm they run.
KDS_ARMS = {"s4": [("", {"arm": "off"}), ("declared", {"arm": "declared"})]}
# A cell started this many times without leaving a JSON (a crash or a stall
# the watchdog killed) is recorded failed, so a resume does not loop on it.
MAX_ATTEMPTS = 3


def sh(*args, check=True, capture=True):
    r = subprocess.run(list(args), capture_output=capture, text=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"{args}: rc {r.returncode}: {r.stderr.strip()[-400:]}")
    return r.stdout.strip() if capture else ""


class Run:
    def __init__(self, a):
        self.a = a
        self.out = a.out
        os.makedirs(self.out, exist_ok=True)
        self.status = os.path.join(self.out, "status")
        with open(os.path.join(self.out, "matrix.pid"), "w") as f:
            f.write(str(os.getpid()))

    def st(self, what):
        line = f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} {what}"
        with open(self.status, "w") as f:
            f.write(line + "\n")
        with open(os.path.join(self.out, "timeline"), "a") as f:
            f.write(line + "\n")

    def sh_beat(self, what, *args):
        """`sh`, writing the status every 10 s while it runs: a server's
        start (Oracle's takes minutes) is not a stall to the watchdog."""
        p = subprocess.Popen(list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        while True:
            try:
                out, err = p.communicate(timeout=10)
                break
            except subprocess.TimeoutExpired:
                self.st(what)
        if p.returncode != 0:
            raise RuntimeError(f"{args}: rc {p.returncode}: {err.strip()[-400:]}")
        return out.strip()

    def quiet_gate(self, tag):
        """No build or test process, and at most 8 % of the CPUs busy over
        3 s; the wait is recorded."""
        t0 = time.time()
        while True:
            builds = subprocess.run(["pgrep", "-f", "cc1plus|cmake --build|ctest|kds_tests|cargo build|rustc"],
                                    capture_output=True, text=True).stdout.split()
            busy = cpu_busy(3.0)
            if not builds and busy <= 8.0:
                break
            self.st(f"gate {tag}: builds={len(builds)} busy={busy:.1f}%")
            time.sleep(5)
        return time.time() - t0

    def host_record(self, path, gate_s):
        with open(path, "w") as f:
            f.write(time.strftime("%Y-%m-%dT%H:%M:%SZ\n", time.gmtime()))
            f.write(open("/proc/loadavg").read())
            f.write(f"gate_wait_s {gate_s:.1f}\n")
            procs = subprocess.run(["pgrep", "-a", "-f", "cc1plus|cmake --build|ctest|kds_tests"],
                                   capture_output=True, text=True).stdout.strip()
            f.write((procs or "no build or test process") + "\n")

    def engine_args(self, engine, durability):
        if engine == "kds":
            return ["--engine", "kds", "--kds", f"127.0.0.1:{KDS_PORT}", "--kds-durability", durability]
        if engine == "pg":
            return ["--engine", "pg", "--pg", f"host=127.0.0.1 port={PG_PORT} user=postgres dbname=bench"]
        return ["--engine", "oracle", "--env-file", self.a.ora_secret,
                "--ora", "bench/$ORA_PW@//127.0.0.1:1521/FREEPDB1",
                "--ora-admin", "system/$ORA_PW@//127.0.0.1:1521/FREEPDB1"]

    def cell(self, rnd, engine, scenario, name, sets, durability):
        tag = f"r{rnd}-{scenario}-{name}-{engine}" + (f"-{durability}" if engine == "kds" else "")
        out = os.path.join(self.out, self.a.pass_, tag + ".json")
        if os.path.exists(out):
            return
        os.makedirs(os.path.dirname(out), exist_ok=True)
        attempts_path = out[:-5] + ".attempts"
        attempts = int(open(attempts_path).read() or 0) if os.path.exists(attempts_path) else 0
        if attempts >= MAX_ATTEMPTS:
            with open(out, "w") as f:
                json.dump({"failed": True, "rc": f"abandoned after {attempts} attempts with no result"}, f)
            return
        with open(attempts_path, "w") as f:
            f.write(str(attempts + 1))
        gate = self.quiet_gate(tag)
        self.host_record(out[:-5] + ".host.txt", gate)
        env = dict(os.environ)
        kpid = None
        if engine == "kds":
            d = os.path.join(self.a.work, "kds")
            # A server a killed run left behind holds the port: stop it.
            try:
                old = open(os.path.join(d, "pid")).read().strip()
                if old and os.path.exists(f"/proc/{old}") and "kds_server" in open(f"/proc/{old}/cmdline").read():
                    subprocess.run(["kill", old])
                    time.sleep(1)
            except OSError:
                pass
            kpid = sh(SERVERS, "kds-up", self.a.kds_bin, d, str(KDS_PORT), durability, SERVER_CPUS)
            cpu = f"pid:{kpid}"
        elif engine == "pg":
            # A fresh PostgreSQL a cell, as KDS's is (S2 review, item 7): no
            # cell runs on another cell's warm cache. Oracle's start takes
            # minutes, so it is fresh a round, and the results say so.
            self.pg_fresh(rnd, tag)
            cpu = "cgroup:" + sh(SERVERS, "cgroup", "eq-pg")
        else:
            cpu = "cgroup:" + sh(SERVERS, "cgroup", "eq-ora")
            env["ORACLE_HOME"] = self.a.ora_home
            env["LD_LIBRARY_PATH"] = os.path.join(self.a.ora_home, "lib")
        args = ["taskset", "-c", HARNESS_CPUS, EQ] + self.engine_args(engine, durability) + [
            "--scenario", scenario, "--seed", "1", "--server-cpu", cpu, "--status", self.status + ".cell",
            "--json", out + ".part"]
        for k, v in {**sets, **dict(x.split("=", 1) for x in self.a.set)}.items():
            args += ["--set", f"{k}={v}"]
        self.st(f"cell {tag}")
        t0 = time.time()
        with open(out[:-5] + ".stderr", "w") as err:
            rc = subprocess.run(args, stdout=subprocess.DEVNULL, stderr=err, env=env).returncode
        if kpid:
            subprocess.run(["kill", kpid])
            for _ in range(100):
                if not os.path.exists(f"/proc/{kpid}"):
                    break
                time.sleep(0.1)
            else:
                # Still up after 10 s: the next cell's kds-up would find the
                # port held and stop the matrix.
                subprocess.run(["kill", "-9", kpid])
                time.sleep(1)
            log = os.path.join(self.a.work, "kds", "data", "kds.log")
            if os.path.exists(log):
                subprocess.run(["cp", log, out[:-5] + ".serverlog"])
        with open(out[:-5] + ".rc", "w") as f:
            f.write(f"{rc} {time.time() - t0:.1f}s\n")
        if os.path.exists(out + ".part"):
            os.rename(out + ".part", out)
        else:
            with open(out, "w") as f:
                json.dump({"failed": True, "rc": rc}, f)

    def pg_fresh(self, rnd, tag):
        d = os.path.join(self.out, self.a.pass_)
        sh(SERVERS, "pg-down")
        mode = ["profile"] if self.a.pass_ == "profile" else []
        v = self.sh_beat(f"{tag}: pg-up", SERVERS, "pg-up", str(PG_PORT), *mode)
        sh(SERVERS, "pin", "eq-pg", SERVER_CPUS)
        open(os.path.join(d, f"r{rnd}-pg-version.txt"), "w").write(v + "\n")

    def servers_up(self, rnd, engines):
        self.st(f"round {rnd}: servers")
        d = os.path.join(self.out, self.a.pass_)
        os.makedirs(d, exist_ok=True)
        if "oracle" in engines:
            sh(SERVERS, "ora-down")
            v = self.sh_beat(f"round {rnd}: ora-up", SERVERS, "ora-up", self.a.ora_secret)
            sh(SERVERS, "pin", "eq-ora", SERVER_CPUS)
            open(os.path.join(d, f"r{rnd}-oracle-version.txt"), "w").write(v + "\n")

    def round_done(self, rnd):
        return os.path.exists(os.path.join(self.out, self.a.pass_, f"r{rnd}.done"))

    def go(self):
        engines = self.a.engines.split(",")
        if self.a.pass_ == "profile":
            engines = ["pg"]
        only = set(self.a.only.split(",")) if self.a.only else None
        cells = [c for c in CELLS if not only or c[0] in only]
        for rnd in range(1, self.a.rounds + 1):
            if self.round_done(rnd):
                continue
            self.servers_up(rnd, engines)
            # Engine order rotates a round, so no engine always runs first
            # or always after another engine's cell.
            k = (rnd - 1) % len(engines)
            order = engines[k:] + engines[:k]
            for scenario, name, sets, durs in cells:
                for engine in order:
                    if engine == "kds":
                        arms = KDS_ARMS.get(scenario, [("", {})])
                        for d in durs:
                            for arm, extra in arms:
                                self.cell(rnd, "kds", scenario, name + (f"-{arm}" if arm else ""),
                                          {**sets, **extra}, d)
                    else:
                        self.cell(rnd, engine, scenario, name, sets, None)
            open(os.path.join(self.out, self.a.pass_, f"r{rnd}.done"), "w").write("")
        if "pg" in engines:
            sh(SERVERS, "pg-down")
        if "oracle" in engines:
            sh(SERVERS, "ora-down")
        self.st("ALL DONE")
        open(os.path.join(self.out, f"{self.a.pass_}.done"), "w").write("")


def cpu_busy(seconds):
    def snap():
        f = open("/proc/stat").readline().split()[1:]
        v = list(map(int, f))
        return sum(v), v[3] + v[4]
    t0, i0 = snap()
    time.sleep(seconds)
    t1, i1 = snap()
    return 100.0 * (1 - (i1 - i0) / max(1, t1 - t0))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", required=True)
    p.add_argument("--work", required=True, help="a directory on a block device for KDS's data file")
    p.add_argument("--kds-bin", required=True)
    p.add_argument("--ora-home", required=True)
    p.add_argument("--ora-secret", required=True)
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--only", default="")
    p.add_argument("--engines", default="kds,pg,oracle")
    p.add_argument("--set", action="append", default=[], help="k=v for every cell (a dry run's small scale)")
    p.add_argument("--pass", dest="pass_", choices=("timed", "profile"), default="timed")
    a = p.parse_args()
    Run(a).go()


if __name__ == "__main__":
    sys.exit(main())
