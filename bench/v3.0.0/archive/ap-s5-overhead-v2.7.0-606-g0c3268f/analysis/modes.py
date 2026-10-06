import json, glob, os
os.chdir('/home/cdkbs/bench-runs/ap-s5')
for pf in ('', 'pin-'):
    tot = slow = 0
    ab = {'A': 0, 'B': 0}
    files = glob.glob(pf + 'point-n*-run*/result.json') + glob.glob(pf + 'scan-n*-run*/result.json')
    for f in files:
        j = json.load(open(f))
        for l in 'AB':
            tot += 1
            if j['arms'][l]['ping']['p50_us'] > 50:
                slow += 1
                ab[l] += 1
    print(repr(pf), tot, slow, ab)
