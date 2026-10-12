"""Per cell: the WAL writer's syncs per second and log bytes per sync, from
SHOW META's m0/m1 (keys absent from m0 read as 0)."""
import glob, json, os

OUT = '/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
for dur in ('relaxed', 'group'):
    for cell in ('c3', 'c2', 'c1-segregated-no0', 'c1-iso-segregated-no0'):
        for f in sorted(glob.glob(f'{OUT}/{dur}/{cell}/r*/bl-mixed-*.json')):
            d = json.load(open(f))
            m0, m1 = d['meta_m0'], d['meta_m1']
            secs = (d['m1_mono_ns'] - d['m0_mono_ns']) / 1e9
            def dl(k):
                try:
                    return float(m1.get(k, 0)) - float(m0.get(k, 0))
                except (TypeError, ValueError):
                    return None
            keys = [k for k in m1 if 'sync' in k and k.startswith('wal_')]
            lsn = dl('wal_append_lsn')
            syncs = dl('wal_syncs')
            print(f'{dur:8} {cell:22} {os.path.basename(os.path.dirname(f))} '
                  f'secs={secs:.1f} log_MBps={lsn / secs / 1e6:.1f} wal_syncs/s={syncs / secs:.1f} '
                  f'MB/sync={(lsn / syncs / 1e6) if syncs else float("nan"):.2f} '
                  + ' '.join(f'{k}={dl(k):.0f}' for k in sorted(keys) if dl(k) is not None)[:600])
