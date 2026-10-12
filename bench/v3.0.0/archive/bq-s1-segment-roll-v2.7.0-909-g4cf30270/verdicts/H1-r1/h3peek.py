import sys
sys.argv=['x','start']
exec(open('/home/cdkbs/bench-runs/bq/verdicts/H1-r1/r1.py').read().replace('\nmain()\n','\n'))
data,_=load('start')
H0=['c1-batch0','c1-segregated-no0','c1-spread','c2']
for dur in ('relaxed','group'):
    pool=[r for c in H0 for _,r in data[(dur,c)]]
    thr=p99_nr([r['hold_us'] for r in pool])
    for lab,rs in (('top',[r for r in pool if r['hold_us']>=thr]),('all',pool)):
        wt=sum(r['waited_sync_tail_us'] for r in rs); wr=sum(r['waited_sync_rest_us'] for r in rs)
        segs=[r['sync_segs'] for r in rs]
        print(dur,lab,len(rs),'waited tail %.0f rest %.0f ms'%(wt/1e3,wr/1e3),'own tail %.0f rest %.0f ms'%(sum(r['sync_tail_us'] for r in rs)/1e3,sum(r['sync_rest_us'] for r in rs)/1e3),'segs max',max(segs))
