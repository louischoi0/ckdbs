import json, glob, os, statistics, re
D = '/tmp/claude-1000/-home-cdkbs-ckdbs/4e50e866-8005-43e4-b524-1d53355b0c93/scratchpad/bench-xg4m/out'

cells = {}
for f in sorted(glob.glob(D + '/*.json')):
    name = os.path.basename(f)[:-5]
    m = re.match(r'(c\d+(?:-(?:pl|nopl))?)-b(\d+)-r(\d+)$', name)
    if not m:
        print("skip", name); continue
    cfg, b, r = m.group(1), m.group(2), m.group(3)
    cells.setdefault((cfg, b), []).append(json.load(open(f)))

def med(rows, get):
    v = [get(x) for x in rows]
    v = [x for x in v if x is not None]
    return statistics.median(v) if v else None

def floor(rows, get):
    v = [get(x) for x in rows]
    v = [x for x in v if x is not None]
    if not v or statistics.median(v) == 0:
        return None
    return 100 * (max(v) - min(v)) / statistics.median(v)

def legs(j):
    """Coordinator legs, summed over cores that walked any."""
    out = {}
    for core, d in j['per_core_delta'].items():
        for k, v in d.items():
            if k.startswith('xowner_'):
                out[k] = out.get(k, 0) + v
    return out

print("%-12s %-3s %8s %9s %9s %9s %7s %6s %6s" %
      ("config", "b", "tps", "book p50", "book p95", "book p99", "s/book", "tpsF%", "p50F%"))
order = sorted(cells, key=lambda k: (k[0], int(k[1])))
for key in order:
    rows = cells[key]
    cfg, b = key
    tps = med(rows, lambda j: j['tps'])
    p50 = med(rows, lambda j: j['phases']['booking'].get('p50_us'))
    p95 = med(rows, lambda j: j['phases']['booking'].get('p95_us'))
    p99 = med(rows, lambda j: j['phases']['booking'].get('p99_us'))
    spb = med(rows, lambda j: j['syncs_per_booking'])
    ftps = floor(rows, lambda j: j['tps'])
    fp50 = floor(rows, lambda j: j['phases']['booking'].get('p50_us'))
    print("%-12s %-3s %8.1f %9.1f %9.1f %9.1f %7.3f %6.1f %6.1f" %
          (cfg, b, tps, p50, p95, p99, spb,
           ftps if ftps is not None else -1, fp50 if fp50 is not None else -1))

print()
print("---- per-leg, cross-owner cells only (median of 3, mean us per commit) ----")
for key in order:
    cfg, b = key
    if 'pl' not in cfg or 'nopl' in cfg:
        continue
    rows = cells[key]
    L = [legs(j) for j in rows]
    def leg_mean(name):
        vals = []
        for l in L:
            n = l.get(name + '_n', 0)
            if n:
                vals.append(l.get(name + '_us', 0) / n)
        return statistics.median(vals) if vals else None
    def leg_n(name):
        return statistics.median([l.get(name + '_n', 0) for l in L])
    ro = statistics.median([sum(d.get('shipped_readonly_prepares', 0)
                                for d in j['per_core_delta'].values()) for j in rows])
    parts = []
    for nm, label in (('xowner_prepare', 'prep'), ('xowner_decision', 'decis'),
                      ('xowner_decide', 'decid'), ('xowner_commit', 'whole'),
                      ('xowner_part_prepare', 'p.prep'), ('xowner_part_ack', 'p.ack'),
                      ('xowner_part_durable', 'p.dur')):
        v = leg_mean(nm)
        parts.append("%s=%s" % (label, ("%.0f" % v) if v is not None else "-"))
    print("%-12s b=%-2s commits=%-6.0f readonly_prep=%-6.0f %s" %
          (cfg, b, leg_n('xowner_commit'), ro, " ".join(parts)))
