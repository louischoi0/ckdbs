import json, glob, os, statistics as st
os.chdir('/home/cdkbs/bench-runs/ap-s5')
for cell, n, arms in (('point', 1000, ('point-star', 'point-wide', 'update-pk', 'update-walk', 'ping')),
                      ('point', 10000, ('update-walk',)),
                      ('scan', 1000, ('scan-reject',)), ('scan', 10000, ('scan-reject',)), ('scan', 60000, ('scan-reject', 'ping'))):
    runs = [json.load(open(f)) for f in glob.glob(f'pin-{cell}-n{n}-run*/result.json')]
    for a in arms:
        out = []
        for l in 'AB':
            e = st.median([b['engine_us_per_op'] for r in runs for b in r['blocks'][l][a]])
            c = st.median([r['arms'][l][a]['p50_us'] for r in runs])
            out.append(f'{l} engine {e:.1f} client p50 {c:.1f}')
        print(cell, n, a, ' | '.join(out))
