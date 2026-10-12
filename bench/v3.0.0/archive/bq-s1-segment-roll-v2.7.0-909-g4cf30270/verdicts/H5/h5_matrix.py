"""Per roll x per reader: was the reader stalled to that roll's end?
A reader is marked at a roll when one of its samples overlaps the roll and
completes within [end - 5 ms, end + 30 ms]. Also: per reader, its slow samples
NOT aligned to any roll, split 1-10 ms and >= 10 ms (the inserter's or walker's
core shows short non-roll stalls; core 0 outside a roll is idle).
usage: python3 -I h5_matrix.py <out_dir> <durability> <cell...>"""
import glob
import json
import sys

sys.path.insert(0, '/home/cdkbs/bench-runs/bq/verdicts/H5')
from h5 import rolls  # noqa: E402

PRE, POST = 5_000_000, 30_000_000


def main():
    out, dur, cells = sys.argv[1], sys.argv[2], sys.argv[3:]
    for cell in cells:
        for js in sorted(glob.glob(f'{out}/bq/{dur}/{cell}/r*/*.json')):
            d = json.load(open(js))
            lo, hi = d['m0_mono_ns'], d['m1_mono_ns']
            rs = [r for r in rolls(js[:-5] + '.s.log') if r['end_ns'] > lo and r['start_ns'] < hi]
            readers = [c for c in d['clients'] if c['role'] in ('reader', 'writer')]
            print(f"\n{dur} {cell} {js.split('/')[-2]}: rolls={len(rs)} roller cores={[r['core'] for r in rs]}")
            per_roll = []
            for c in readers:
                bits = []
                for r in rs:
                    hit = any(s[0] < r['end_ns'] and s[0] + s[1] > r['start_ns'] and
                              r['end_ns'] - PRE <= s[0] + s[1] <= r['end_ns'] + POST for s in c['slow'])
                    bits.append('X' if hit else '.')
                per_roll.append(bits)
                na = [s for s in c['slow'] if not any(
                    s[0] < r['end_ns'] and s[0] + s[1] > r['start_ns'] and
                    r['end_ns'] - PRE <= s[0] + s[1] <= r['end_ns'] + POST for r in rs)]
                short = sum(1 for s in na if s[1] < 10_000_000)
                long_ = sum(1 for s in na if s[1] >= 10_000_000)
                print(f"  {c['role'][0]}{c['index']} {c['core']:.0f}->{c['core_end']:.0f} {''.join(bits)} "
                      f"non-roll slow: 1-10ms={short} >=10ms={long_}")
            counts = [sum(1 for b in per_roll if b[i] == 'X') for i in range(len(rs))]
            print(f"  stalled clients per roll: {counts}")


main()
