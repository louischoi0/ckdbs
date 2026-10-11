import json, sys
A = "/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad/run/analysis/"
for nm in ("c1", "c2", "null", "null2", "strict", "flip"):
    try:
        js = json.load(open(A + nm + ".json"))
    except OSError:
        continue
    for n in (200, 1000, 10000):
        m = js.get(f"{n}|_meta")
        if not m:
            continue
        r = m["replicate"]
        p = js[f"{n}|ping"]
        print(f"{nm} n{n}: kept {m['n_kept']}/{m['n_all']} rep A med {r['A']['med']:+.2f} rsd {r['A']['rsd']:.2f} | rep B med {r['B']['med']:+.2f} rsd {r['B']['rsd']:.2f} | ping d {p['dmed']:+.2f} [{p['ci'][0]:+.2f}, {p['ci'][1]:+.2f}] | floor {js[f'{n}|insert']['floor']:.2f}")
