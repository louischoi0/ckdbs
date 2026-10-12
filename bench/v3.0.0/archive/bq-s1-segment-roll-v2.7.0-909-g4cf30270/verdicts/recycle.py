"""Per cell: checkpoint and log-recycling movement over the window."""
import glob, json, os

OUT = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
first = True
for dur in ('relaxed', 'group'):
    for cell in ('c3', 'c1-segregated-no0'):
        for f in sorted(glob.glob(f'{OUT}/{dur}/{cell}/r*/bl-mixed-*.json')):
            d = json.load(open(f))
            m0, m1 = d['meta_m0'], d['meta_m1']
            if first:
                print('keys:', sorted(k for k in m1 if any(w in k for w in
                      ('recycl', 'segment', 'checkpoint', 'anchor', 'redo', 'durable', 'detach', 'log_'))))
                first = False
            def v(m, k):
                try:
                    return float(m.get(k, 0))
                except (TypeError, ValueError):
                    return 0.0
            ks = [k for k in sorted(m1) if any(w in k for w in ('recycl', 'segments_removed', 'checkpoint_runs',
                  'anchor_redo', 'redo_start', 'wal_live', 'wal_durable_lsn', 'wal_append_lsn', 'detach'))]
            print(f'{dur:8} {cell:20} {os.path.basename(os.path.dirname(f))} ' +
                  ' '.join(f'{k}:{v(m0, k):.0f}->{v(m1, k):.0f}' for k in ks)[:900])
