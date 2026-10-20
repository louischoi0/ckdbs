"""Exit 0 if the window [t0, t1] (epoch s) holds no build/test sample and no foreign process >= 50 % CPU in monitor.log.
usage: cellvalid.py t0 t1   (prints a one-line verdict)
       cellvalid.py --jsonl FILE  moves the cells of FILE whose window is contaminated to FILE.contaminated and
                                  rewrites FILE without them; exit 1 if any was moved"""
import sys, re, datetime, json
RUN = "/home/cdkbs/bench-runs/ce-s0-v3.3.0-50-g1f57f37b"
BUILD_RE = re.compile(r"cc1plus|/cc1|\bas -|\bld\b|collect2|cmake|ctest|kds_tests|c\+\+|g\+\+|ninja|make")


def samples():
    for l in open(f"{RUN}/monitor.log"):
        m = re.match(r"(\S+) load=(\S+) foreign=(.*)", l.rstrip())
        if not m:
            continue
        t = datetime.datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc).timestamp()
        procs = []
        if m.group(3).strip() != "none":
            for e in m.group(3).split(" | "):
                parts = e.strip().split(":", 2)
                if len(parts) == 3:
                    try:
                        procs.append((parts[0], float(parts[1]), parts[2]))
                    except ValueError:
                        pass
        yield t, procs


def verdict(t0, t1, S=None):
    n = b = h = mi = 0
    who = set()
    for t, procs in (S if S is not None else samples()):
        if t < t0 - 1 or t > t1 + 2:
            continue
        n += 1
        isb = ish = ism = False
        for pid, cpu, cmd in procs:
            if BUILD_RE.search(cmd):
                isb = True; who.add(cmd.split()[0].split("/")[-1])
            elif cpu >= 50:
                ish = True; who.add(cmd.split()[0].split("/")[-1])
            else:
                ism = True
        b += isb; h += ish; mi += ism
    return n, b, h, mi, sorted(who)


if __name__ == "__main__":
    if sys.argv[1] == "--jsonl":
        S = list(samples())
        keep, bad = [], []
        for l in open(sys.argv[2]):
            d = json.loads(l)
            n, b, h, mi, who = verdict(d["t0"], d["t1"], S)
            (bad if (b or h or n == 0) else keep).append((l, f"{d.get('shape')} {d.get('mode')} rep{d.get('rep')} n={n} b={b} h={h} {who}"))
        for _, msg in bad:
            print("CONTAMINATED", msg)
        if bad:
            with open(sys.argv[2] + ".contaminated", "a") as f:
                f.writelines(l for l, _ in bad)
            with open(sys.argv[2], "w") as f:
                f.writelines(l for l, _ in keep)
        sys.exit(1 if bad else 0)
    t0, t1 = float(sys.argv[1]), float(sys.argv[2])
    n, b, h, mi, who = verdict(t0, t1)
    print(f"window {t0:.0f}-{t1:.0f}: samples={n} build/test={b} heavy={h} minor={mi} who={who}")
    sys.exit(1 if (b or h or n == 0) else 0)
