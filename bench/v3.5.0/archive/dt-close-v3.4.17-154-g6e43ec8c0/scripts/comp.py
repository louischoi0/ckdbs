import json, glob, os
D = '/home/ukjin/workspaces/ckdbs/.claude/worktrees/dt-write-path-yield/bench/v3.5.0/archive/dt-close-v3.4.17-154-g6e43ec8c0/'
for f in sorted(glob.glob(D + 'dt-*-r*.json')):
    d = json.load(open(f))
    out = []
    for k in ('host_before', 'host_after'):
        real = [c for c in d[k]['competing'] if 'shell-snapshots' not in c]
        out.append((d[k]['loadavg'].split()[0], len(d[k]['competing']), [r[:60] for r in real]))
    print(os.path.basename(f)[:-5], out)
