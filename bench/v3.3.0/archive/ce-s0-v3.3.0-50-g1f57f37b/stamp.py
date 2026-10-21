import json, sys, datetime, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cellvalid
RUN = cellvalid.RUN
S = list(cellvalid.samples())
cells = [json.loads(l) for l in open(f"{RUN}/rowcost/cells.jsonl")]
for c in cells:
    n, b, h, mi, who = cellvalid.verdict(c['t0'], c['t1'], S)
    if mi or b or h:
        print("minor/other:", c['shape'], c['mode'], c['rep'], n, b, h, mi, who)
print(len(cells), 'valid cells')
lo = min(c['t0'] for c in cells); hi = max(c['t1'] for c in cells)
print(datetime.datetime.fromtimestamp(lo, datetime.timezone.utc), datetime.datetime.fromtimestamp(hi, datetime.timezone.utc))
for t in ['s1-c1-g', 's3-c1-n10000-none', 'r2-s1-c1-g', 'r2-s3-c1-n10000-none', 'r3-s1-c1-g', 'r3-s3-c1-n10000-none']:
    t0 = float(open(f'{RUN}/logs/{t}.t0').read()); t1 = float(open(f'{RUN}/logs/{t}.t1').read())
    print(t, cellvalid.verdict(t0, t1, S), datetime.datetime.fromtimestamp(t0, datetime.timezone.utc).strftime('%H:%M:%S'), int(t1 - t0), 's')
m = json.load(open(f'{RUN}/json/s1-c1-g.json'))['meta']
print({k: m.get(k) for k in ('host', 'port', 'durability', 'seed', 'clustered')})
print(m.get('server_meta', '')[:600])
