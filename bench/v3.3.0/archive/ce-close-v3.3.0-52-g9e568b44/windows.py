import json, sys, datetime, glob, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cellvalid
S = list(cellvalid.samples())
tot = minor = bad = 0
who = set()
ts = []
for fn in sorted(glob.glob(cellvalid.RUN + '/rowcost/cells-*.jsonl')):
    for l in open(fn):
        c = json.loads(l)
        n, b, h, mi, w = cellvalid.verdict(c['t0'], c['t1'], S)
        tot += 1; minor += bool(mi); bad += bool(b or h); who |= set(w)
        ts += [c['t0'], c['t1']]
print('sweep cells', tot, 'with a minor foreign sample', minor, 'build/heavy', bad, sorted(who))
print('sweep window', datetime.datetime.fromtimestamp(min(ts), datetime.timezone.utc), datetime.datetime.fromtimestamp(max(ts), datetime.timezone.utc))
for t in sorted(glob.glob(cellvalid.RUN + '/logs/p*-s?.t0')):
    tag = os.path.basename(t)[:-3]
    t0 = float(open(t).read()); t1 = float(open(cellvalid.RUN + f'/logs/{tag}.t1').read())
    print(tag, cellvalid.verdict(t0, t1, S), int(t1 - t0), 's', datetime.datetime.fromtimestamp(t0, datetime.timezone.utc).strftime('%H:%M:%S'))
