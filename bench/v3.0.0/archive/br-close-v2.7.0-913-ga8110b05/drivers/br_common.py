"""Shared helpers for the BR close measurement drivers (M1, M2, M3). Measurement-stage scratch; tools/ untouched."""
import json, os, shutil, signal, subprocess, sys, time

TOOLS = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/tools"
sys.path.insert(0, TOOLS)
from bench_common import Phase  # noqa
from ckdbs_cli import ServerConnection  # noqa
import bb_overhead_benchmark as bb  # noqa

R = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05"
PAYLOAD_BASE = ("abcdefghijklmnopqrstuvwxyz0123456789" * 60)[:1800]
WIDTH = 1900


def payload(n):
    p = str(n)
    return p + PAYLOAD_BASE[: 1800 - len(p)]


def fields(reply):
    out = {}
    if " error=" in reply:
        reply, out["error"] = reply.split(" error=", 1)
    for token in reply.split():
        if "=" in token:
            k, v = token.split("=", 1)
            out[k] = v
    return out


class Server:
    """A kds_server from a copy of the binary, in its own directory, pinned by taskset."""

    def __init__(self, label, binary, root, port, cores=1, durability="relaxed", frames=262144,
                 archive_dir=None, taskset="0,2,4,6", reactor_cpus="4", ckpt_ms=5000, extra=()):
        self.label, self.binary, self.root, self.port = label, binary, root, port
        os.makedirs(root, exist_ok=True)
        self.data = os.path.join(root, "kds.db")
        self.conf = os.path.join(root, "kds.conf")
        lines = [f"data_file = {self.data}", f"port = {port}", f"cores = {cores}", f"durability = {durability}",
                 f"buffer_pool_frames = {frames}", f"checkpoint_interval_ms = {ckpt_ms}",
                 f"log_file = kds.log", f"log_dir = {root}", "log_level = warn"]
        if cores > 1 or reactor_cpus:
            lines.append(f"reactor_cpus = {reactor_cpus}")
        if archive_dir:
            os.makedirs(archive_dir, exist_ok=True)
            lines.append(f"archive_dir = {archive_dir}")
        lines += list(extra)
        with open(self.conf, "w") as f:
            f.write("\n".join(lines) + "\n")
        self.taskset, self.proc = taskset, None

    def start(self, args=()):
        cmd = (["taskset", "-c", self.taskset] if self.taskset else []) + [self.binary, "--config", self.conf, *args]
        self.out = open(os.path.join(self.root, "stdout.txt"), "w")
        self.proc = subprocess.Popen(cmd, stdout=self.out, stderr=subprocess.STDOUT)
        deadline = time.time() + 120
        while True:
            if self.proc.poll() is not None:
                raise SystemExit(f"{self.label}: server exited {self.proc.returncode}: "
                                 + open(os.path.join(self.root, 'stdout.txt')).read()[-2000:])
            try:
                c = ServerConnection("127.0.0.1", self.port, timeout=2)
                c.close()
                break
            except OSError:
                if time.time() > deadline:
                    raise SystemExit(f"{self.label}: no listen")
                time.sleep(0.1)
        return self

    def conn(self, timeout=120):
        return ServerConnection("127.0.0.1", self.port, timeout=timeout)

    def conn_on_core(self, want, timeout=120):
        for _ in range(300):
            c = self.conn(timeout)
            if bb.meta_all(c).get("core") == want:
                return c
            c.close()
        raise SystemExit(f"no session on core {want}")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(120)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()

    def kill(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


def must(conn, sql, prefix="OK"):
    r = conn.send_command(sql)
    if not r.startswith(prefix) and prefix not in ("",):
        raise SystemExit(f"{sql[:80]!r} -> {r[:300]!r}")
    return r


def host():
    return bb.host_state()


def load_wide(conn, table, first, count, batch=100):
    """INSERT `count` wide rows with named pks first..first+count-1 (id int64, v int64, payload varchar(1900))."""
    n = first
    end = first + count
    while n < end:
        hi = min(end, n + batch)
        sql = f"INSERT INTO {table} VALUES " + ",".join(f"({i}, {i}, '{payload(i)}')" for i in range(n, hi))
        r = conn.send_command(sql)
        if not r.startswith("INSERTED"):
            raise SystemExit(f"load_wide: {r[:300]}")
        n = hi


def load_hot(conn, table, rows):
    for lo in range(1, rows + 1, 500):
        hi = min(rows, lo + 499)
        must(conn, f"INSERT INTO {table} VALUES " + ",".join(f"({i}, {i}, {i})" for i in range(lo, hi + 1)), "INSERTED")


def pct(sl, p):
    if not sl:
        return 0.0
    k = max(0, min(len(sl) - 1, int(-(-p * len(sl) // 100)) - 1))
    return sl[k]


def lat_summary(lat_s, elapsed=None):
    sl = sorted(lat_s)
    d = {"ops": len(sl)}
    if sl:
        d.update({"p0_us": round(sl[0] * 1e6, 1), "p25_us": round(pct(sl, 25) * 1e6, 1), "p50_us": round(pct(sl, 50) * 1e6, 1),
                  "p95_us": round(pct(sl, 95) * 1e6, 1), "p99_us": round(pct(sl, 99) * 1e6, 1),
                  "p999_us": round(pct(sl, 99.9) * 1e6, 1), "max_us": round(sl[-1] * 1e6, 1),
                  "mean_us": round(sum(sl) / len(sl) * 1e6, 1)})
        slow = [x for x in sl if x > 1e-3]
        d["slow_gt_1ms_n"] = len(slow)
        d["slow_gt_1ms_s"] = round(sum(slow), 4)
        if elapsed:
            d["qps"] = round(len(sl) / elapsed, 1)
    return d
