t=open('/home/cdkbs/bench-runs/bh-close2/head.md').read()
B='/home/cdkbs/bench-runs/bh-close2/'
summ=open(B+'tables-summary.md').read().strip()
a,b=summ.split("\nA p50 per cell, µs:\n")
hdr,sep=a.split("\n")[:2]
summ=a+"\n\nA's p50 in each cell (median of the 16 per-run p50s):\n\n"+hdr+"\n"+sep+"\n"+b
pu=open(B+'tables-purge.md').read().strip()
t=t.replace("@@SUMMARY@@",summ).replace("@@C1@@",open(B+'tables-c1.md').read().strip()).replace("@@C2@@",open(B+'tables-c2.md').read().strip()).replace("@@PURGE@@",pu)
open('/home/cdkbs/ckdbs/.claude/worktrees/bh-purge-key/bench/v3.0.0/results-bh-close-v2.7.0-712-g4015736d.md','w').write(t+"\n")
