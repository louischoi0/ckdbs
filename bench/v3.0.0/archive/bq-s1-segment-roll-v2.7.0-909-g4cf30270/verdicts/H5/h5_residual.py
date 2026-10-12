"""The reader stalls >= 10 ms NOT aligned to a roll end: where do they fall?
Against the checkpoint runs' [start_ns, end_ns] (the "[checkpoint] run:" lines)
and against the rolls widened by 100 ms after their end.
usage: python3 -I h5_residual.py <out_dir> <durability> <cell...>"""
import glob
import json
import re
import sys

sys.path.insert(0, '/home/cdkbs/bench-runs/bq/verdicts/H5')
from h5 import align, kv, rolls  # noqa: E402

CK = re.compile(r'\[checkpoint\] run: (.*)$')


def main():
    out, dur, cells = sys.argv[1], sys.argv[2], sys.argv[3:]
    for cell in cells:
        for js in sorted(glob.glob(f'{out}/bq/{dur}/{cell}/r*/*.json')):
            d = json.load(open(js))
            log = js[:-5] + '.s.log'
            lo, hi = d['m0_mono_ns'], d['m1_mono_ns']
            rs = [r for r in rolls(log) if r['end_ns'] > lo and r['start_ns'] < hi]
            cks = [kv(m.group(1)) for m in (CK.search(x) for x in open(log)) if m]
            cks = [c for c in cks if c['end_ns'] > lo and c['start_ns'] < hi]
            sm = [tuple(s) for c in d['clients'] if c['role'] in ('reader', 'writer') for s in c['slow']]
            al = align(sm, rs)
            res = [s for s, a in zip(sm, al) if not a and s[1] >= 10_000_000]
            in_ck = [s for s in res if any(c['start_ns'] < s[0] + s[1] and c['end_ns'] > s[0] for c in cks)]
            near = [s for s in res if any(r['start_ns'] < s[0] + s[1] and r['end_ns'] + 100_000_000 > s[0]
                                          for r in rs)]
            ck_desc = [(c['core'], round((c['end_ns'] - c['start_ns']) / 1e6)) for c in cks]
            print(f"{dur} {cell} {js.split('/')[-2]}: residual>=10ms={len(res)} "
                  f"sum={sum(s[1] for s in res) / 1e9:.2f}s in_checkpoint_run={len(in_ck)} "
                  f"near_roll(+100ms)={len(near)} checkpoints(core,ms)={ck_desc}")


main()
