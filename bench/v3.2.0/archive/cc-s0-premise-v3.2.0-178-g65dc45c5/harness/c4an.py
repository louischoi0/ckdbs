import json, sys
d = json.load(open(sys.argv[1]))
W = {'int64': 8, 'int32': 4, 'varchar': 64}
rows = []
for r in d['rows']:
    tys = [x.split(' | ')[6] for x in r['describe']]
    rs = sum(W[t] for t in tys)
    tr = int(r['true_rows']); lv = int(r['leaves']); h = int(r['height'])
    capA = 8140 // (25 + rs)           # row_size counts the id column
    capB = 8140 // (25 + rs - 8)       # id carried only in the tuple header
    rows.append((r['name'], h, lv, tr, rs, capA, capB))
    prior = lv * capA * 0.75
    priorB = lv * capB * 0.75
    print(f"{r['name']:24s} h={h} leaves={lv:5d} rows={tr:7d} rows/leaf={tr/lv:7.1f} rowsize={rs:4d} capA={capA:3d} capB={capB:3d} "
          f"priorA={prior:9.0f} ratioA={prior/max(tr,1):5.2f} priorB={priorB:9.0f} ratioB={priorB/max(tr,1):5.2f}")
json.dump(rows, open(sys.argv[1] + '.table', 'w'))
