import json, glob, collections, statistics as st

S4 = '/home/cdkbs/ckdbs/.claude/worktrees/ba-open-marks/bench/v3.0.0/archive/ba-s4-census-v2.7.0-725-gc390a624'


def load(d):
    o = collections.defaultdict(list)
    for f in sorted(glob.glob(d + '/*.json')):
        r = json.load(open(f))
        o[(r['shape'], r['durability'], r['cores'], r['sessions'])].append(r)
    return o


B = load('/home/cdkbs/bench-runs/ba-s17/kds')
A = load(S4 + '/kds')
PG = collections.defaultdict(list)
for sub, cpus in (('pg-cpus0', '0'), ('pg-cpus0-2', '0,2')):
    for f in glob.glob(S4 + '/' + sub + '/*.json'):
        r = json.load(open(f))
        PG[(r['shape'], r['durability'], cpus, r['sessions'])].append(r['statements_per_s'])


def m(D, k):
    v = D.get(k)
    return st.median(r['statements_per_s'] for r in v) if v else None


print("| shape | durability | cores | engine | 1 session | 2 sessions | 4 sessions | 8 sessions | 16 sessions |")
print("|---|---|---|---|---|---|---|---|---|")
for shape in ('trade', 'insert1', 'insertN', 'recent'):
    for dur in ('group', 'strict'):
        for k in (1, 2, 4):
            for name, D in (('B', B), ('S4', A)):
                print("| %s | %s | %d | %s | " % (shape, dur, k, name) + " | ".join("%s stmt/s" % format(m(D, (shape, dur, k, s)), ',.0f') for s in (1, 2, 4, 8, 16)) + " |")
    for k, cpus in ((1, '0'), (2, '0,2')):
        print("| %s | PostgreSQL on | CPUs %s | BA-S4 | " % (shape, cpus) + " | ".join("%s stmt/s" % format(st.median(PG[(shape, 'on', cpus, s)]), ',.0f') for s in (1, 2, 4, 8, 16)) + " |")
