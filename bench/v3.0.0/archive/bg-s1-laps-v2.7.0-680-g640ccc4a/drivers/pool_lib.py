import os, subprocess, sys, threading, time, signal
sys.path.insert(0, "/home/cdkbs/ckdbs/.claude/worktrees/bg-s1-laps/tools")
from ckdbs_cli import ServerConnection  # noqa

R = "/home/cdkbs/bench-runs/bg-s1"
BIN = {"L": f"{R}/bin/kds_server-L"}
PAD = "x" * 100


def meta(conn):
    text = conn.send_command("SHOW META").replace("\\n", " ")
    out = {}
    for tok in text.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                out[k] = float(v)
            except ValueError:
                pass
    return out


class Server:
    def __init__(self, arm, wd, port, frames, durability="relaxed", cores=1, pin=None):
        self.arm, self.wd, self.port = arm, wd, port
        os.makedirs(wd, exist_ok=True)
        self.conf = f"{wd}/{arm}.conf"
        with open(self.conf, "w") as f:
            f.write(f"data_file = {wd}/{arm}.db\nport = {port}\ncores = {cores}\ndurability = {durability}\n"
                    f"buffer_pool_frames = {frames}\nlog_file = {arm}.log\nlog_dir = {wd}\nlog_level = warn\n")
        self.pin = pin
        self.proc = None
        self.rss_samples = []
        self._stop = threading.Event()

    def start(self):
        cmd = (["taskset", "-c", str(self.pin)] if self.pin is not None else []) + [BIN[self.arm], "--config", self.conf]
        self.err = open(f"{self.wd}/{self.arm}.stderr", "ab")
        self.proc = subprocess.Popen(cmd, stdout=self.err, stderr=subprocess.STDOUT)
        t0 = time.time()
        while True:
            try:
                self.conn = ServerConnection("127.0.0.1", self.port)
                self.conn._conn.sock.settimeout(3600)
                break
            except OSError:
                if self.proc.poll() is not None:
                    raise RuntimeError(f"server exited {self.proc.returncode}: " + open(f"{self.wd}/{self.arm}.stderr").read()[-500:])
                if time.time() - t0 > 60:
                    raise RuntimeError("no listen")
                time.sleep(0.1)
        return self

    def rss(self):
        vals = {}
        with open(f"/proc/{self.proc.pid}/status") as f:
            for l in f:
                if l.startswith(("VmRSS", "VmHWM")):
                    k, v = l.split(":")
                    vals[k] = int(v.split()[0])
        return vals

    def sample_start(self, interval=0.1):
        self.rss_samples = []
        self._stop.clear()

        def run():
            while not self._stop.is_set():
                try:
                    self.rss_samples.append((time.time(), self.rss()["VmRSS"]))
                except OSError:
                    return
                time.sleep(interval)
        self._th = threading.Thread(target=run, daemon=True)
        self._th.start()

    def sample_stop(self):
        self._stop.set()
        self._th.join()
        v = [x[1] for x in self.rss_samples]
        return {"samples": len(v), "min_mib": min(v) / 1024, "max_mib": max(v) / 1024, "last_mib": v[-1] / 1024}

    def stop(self):
        try:
            self.conn.close()
        except Exception:
            pass
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(120)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.err.close()

    def q(self, sql):
        return self.conn.send_command(sql)


def row(i):
    return f"({i}, {i // 4096}, {i % 1000}, '{PAD}')"
