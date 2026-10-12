"""Do the bar's slow samples line up with rolls? For each run, the share of
reader/writer samples at or above THRESH ms whose [due, due+lat] interval
overlaps a roll's [start_ns, end_ns], and whose completion lands within
TAIL ms after a roll's end, against the share of the window rolls cover;
the same against checkpoint runs ("[checkpoint] run: ... start_ns end_ns").
usage: python3 -I overlap.py <out_dir>"""
import bisect, glob, json, os, re, sys

OUT = sys.argv[1]
THRESH_MS = 50
TAIL_MS = 20
ROLL = re.compile(r'\[wal\] roll .*start_ns=(\d+) end_ns=(\d+)')
CKPT = re.compile(r'\[checkpoint\] run: .*start_ns=(\d+) end_ns=(\d+)')


def windows(log, rx):
    w = []
    for line in open(log):
        m = rx.search(line)
        if m:
            w.append((int(m.group(1)), int(m.group(2))))
    return sorted(w)


def overlaps(ws, a, b):
    return any(s < b and e > a for s, e in ws)


def lands_after(ws, t):
    ends = sorted(e for _, e in ws)
    i = bisect.bisect_right(ends, t) - 1
    return i >= 0 and t - ends[i] <= TAIL_MS * 1e6


def cover(ws, lo, hi):
    return sum(max(0, min(e, hi) - max(s, lo)) for s, e in ws) / (hi - lo)


for dur in ('relaxed', 'group'):
    for cell in ('c1-segregated-no0', 'c1-batch0', 'c1-spread', 'c2'):
        for js in sorted(glob.glob(f'{OUT}/bq/{dur}/{cell}/r*/*.json')):
            d = json.load(open(js))
            log = js[:-5] + '.s.log'
            rw, cw = windows(log, ROLL), windows(log, CKPT)
            lo, hi = d['measure_from_mono_ns'], d['stop_at_mono_ns']
            role = 'reader' if d['readers'] else 'writer'
            ins = [c['core'] for c in d['clients'] if c['role'] == 'inserter']
            n = ov_r = after_r = ov_c = ov_either = 0
            for c in d['clients']:
                if c['role'] != role or c['core'] in ins:
                    continue
                for due, lat in c['slow']:
                    if lat < THRESH_MS * 1e6:
                        continue
                    n += 1
                    a, b = due, due + lat
                    r = overlaps(rw, a, b)
                    k = overlaps(cw, a, b)
                    ov_r += r
                    ov_c += k
                    ov_either += r or k
                    after_r += lands_after(rw, b)
            pc = lambda x: f'{100 * x / n:.0f}%' if n else '-'
            print(f'{dur:7} {cell:18} {js.split("/")[-2]} NB {role} samples>={THRESH_MS}ms={n:5d} '
                  f'overlap roll={pc(ov_r)} end<= {TAIL_MS}ms after a roll end={pc(after_r)} '
                  f'overlap ckpt={pc(ov_c)} either={pc(ov_either)} | window covered: rolls={100 * cover(rw, lo, hi):.0f}% '
                  f'ckpt={100 * cover(cw, lo, hi):.0f}%')
