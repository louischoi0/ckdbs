"""Host-state helpers shared by the runners: per-CPU busy fractions, a 1 s sampler, a competing-program list.

`competing()` matches process NAMES (comm), not command lines, so another session's idle wait loop
that merely mentions `ctest` is not counted; `cmake` counts only with `--build` in its arguments.
"""
import subprocess, threading, time

COMPILE_COMM = {"cc1plus", "cc1", "ctest", "kds_tests", "ld", "collect2", "c++", "g++", "gcc", "ninja",
                "make", "python_test", "ckdbs-sim"}


def cpu_stat():
    out = {}
    with open("/proc/stat") as f:
        for l in f:
            if l.startswith("cpu") and l[3].isdigit():
                p = l.split()
                v = [int(x) for x in p[1:]]
                out[int(p[0][3:])] = (sum(v), v[3] + v[4])
    return out


def busy_frac(a, b):
    r = {}
    for c in a:
        dt = b[c][0] - a[c][0]
        r[c] = round(1 - (b[c][1] - a[c][1]) / dt, 3) if dt else 0.0
    return r


def competing():
    out = subprocess.run(["ps", "-eo", "pid,comm,args"], capture_output=True, text=True).stdout.splitlines()[1:]
    hits = []
    for l in out:
        parts = l.split(None, 2)
        if len(parts) < 2:
            continue
        pid, comm = parts[0], parts[1]
        args = parts[2] if len(parts) > 2 else ""
        if comm in COMPILE_COMM or (comm == "cmake" and "--build" in args):
            hits.append(f"{pid} {comm} {args[:80]}")
    return hits


class Sampler:
    """Samples per-CPU busy every `period` seconds on a thread; `stop()` returns mean and max per CPU."""

    def __init__(self, period=1.0):
        self.period = period
        self.samples = []
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        prev = cpu_stat()
        while not self._stop.wait(self.period):
            cur = cpu_stat()
            self.samples.append(busy_frac(prev, cur))
            prev = cur

    def start(self):
        self._t.start()
        return self

    def stop(self):
        self._stop.set()
        self._t.join()
        if not self.samples:
            return {"mean": {}, "max": {}, "n": 0, "comp_seen": []}
        cpus = sorted(self.samples[0])
        return {"mean": {c: round(sum(s[c] for s in self.samples) / len(self.samples), 3) for c in cpus},
                "max": {c: max(s[c] for s in self.samples) for c in cpus}, "n": len(self.samples)}
