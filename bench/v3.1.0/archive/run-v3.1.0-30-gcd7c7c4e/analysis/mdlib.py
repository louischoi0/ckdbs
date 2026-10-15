import json, os, re, statistics, datetime

RUN = "/home/cdkbs/bench-runs/v3.1.0-30-gcd7c7c4e"
OLD = "/tmp/claude-1000/-home-cdkbs-ckdbs/ff2cbc3f-cf1f-478c-a618-6507b57727ba/scratchpad/old/bench/v3.0.0/archive"
OLDJ = {1: f"{OLD}/scenario1-v2.7.0-531-g9a0525d/json", 2: f"{OLD}/scenario2-v2.7.0-531-g9a0525d/json",
        3: f"{OLD}/scenario3-v2.7.0-531-g9a0525d/json", 4: f"{OLD}/scenario4-v2.7.0-531-g9a0525d/json"}
OLDL = {2: f"{OLD}/scenario2-v2.7.0-531-g9a0525d/logs", 3: f"{OLD}/scenario3-v2.7.0-531-g9a0525d/logs"}
J = f"{RUN}/json"
L = f"{RUN}/logs"


def load(tag, old=None):
    base = OLDJ[old] if old else J
    return json.load(open(f"{base}/{tag}.json"))


def ph(d, name):
    for p in d["phases"]:
        if p["phase"] == name:
            return p
    return None


def qps_from_mean(p):
    return 1e6 / p["mean_us"]


def fmt_us(x):
    return f"{x:,.1f} µs"


def pct_row(p):
    return (f"{p['ops']:,} ops | {fmt_us(p['p0_us'])} | {fmt_us(p['p25_us'])} | {fmt_us(p['p50_us'])} | "
            f"{fmt_us(p['p95_us'])} | {fmt_us(p['p99_us'])} | {fmt_us(p['max_us'])} | "
            f"{p['errors']} error{'s' if p['errors'] != 1 else ''}")


def precheck(tag):
    t = open(f"{L}/{tag}.precheck").read()
    tm = re.search(r"time: (\S+)", t).group(1)
    la = " / ".join(re.search(r"loadavg: (.*)", t).group(1).split()[:3])
    gw = re.search(r"gate wait s: (\d+)", t)
    procs = "none" if "  none" in t else "SEE FILE"
    return tm, la, int(gw.group(1)) if gw else 0, procs


def ts(s):
    return datetime.datetime.strptime(s, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc)


def timeline_windows():
    """tag -> (start_after_gate, end) from precheck time and the timeline end line."""
    ends = {}
    starts = {}
    for l in open(f"{RUN}/timeline"):
        m = re.match(r"== (\S+) end (\S+) rc=(\d+) ==", l)
        if m:
            ends[m.group(1)] = (m.group(2), m.group(3))
        m = re.match(r"== (\S+) start (\S+) ==", l)
        if m:
            starts[m.group(1)] = m.group(2)  # last start wins
    return starts, ends


BUILD_RE = re.compile(r"cc1plus|/cc1|\bas -|\bld\b|collect2|cmake|ctest|kds_tests|c\+\+|g\+\+|ninja|make")


def monitor_samples():
    out = []
    for l in open(f"{RUN}/monitor.log"):
        m = re.match(r"(\S+) load=(\S+) foreign=(.*)", l.rstrip())
        if not m:
            continue
        procs = []
        if m.group(3).strip() != "none":
            for e in m.group(3).split(" | "):
                e = e.strip()
                if not e:
                    continue
                parts = e.split(":", 2)
                if len(parts) == 3:
                    try:
                        procs.append((parts[0], float(parts[1]), parts[2]))
                    except ValueError:
                        pass
        out.append((ts(m.group(1)), float(m.group(2)), procs))
    return out


def validity(tag, samples, t0, t1):
    """returns (n_samples, build_samples, heavy_samples, minor_samples, detail)"""
    n = b = h = mi = 0
    detail = set()
    for t, load, procs in samples:
        if t < t0 or t > t1:
            continue
        n += 1
        isb = ish = ism = False
        for pid, cpu, cmd in procs:
            if BUILD_RE.search(cmd):
                isb = True
                detail.add(cmd.split()[0].split("/")[-1])
            elif cpu >= 50:
                ish = True
                detail.add(cmd.split()[0].split("/")[-1])
            else:
                ism = True
        b += isb
        h += ish
        mi += ism
    return n, b, h, mi, sorted(detail)
