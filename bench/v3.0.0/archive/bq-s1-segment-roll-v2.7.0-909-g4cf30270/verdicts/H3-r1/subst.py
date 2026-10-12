import sys,statistics as st,math
sys.path.insert(0,'/home/cdkbs/bench-runs/bq/verdicts/H3-r1')
import glob,os
exec(open('/home/cdkbs/bench-runs/bq/verdicts/H3-r1/r1.py').read().split('out={}')[0])
for dur in ['relaxed','group']:
  rows=[];own=[]
  for cell in sorted(os.listdir(f'{B}/{dur}')):
    for rd in sorted(glob.glob(f'{B}/{dur}/{cell}/r*')):
      rs,_,_=load(rd)
      for r in rs:
        own.append((r['sync_us'],r['sync_tail_us'],r['sync_rest_us'],r['sync_segs'],cell))
        n=r['waited_syncs']
        if n==0: continue
        tot=(r['waited_sync_tail_us']+r['waited_sync_rest_us'])/n
        rest=r['waited_sync_rest_us']/n; segs=r['waited_sync_segs']/n
        for _ in range(n): rows.append((tot,rest,segs,r['waited_sync_tail_us']/n,cell))
  lo=[t for t,re,s,ta,c in rows if re<0.05*t]; hi=[t for t,re,s,ta,c in rows if re>0.30*t]
  print(dur,'syncs',len(rows),'lo<5% n',len(lo),'med',st.median(lo) if lo else None,'hi>30% n',len(hi),'med',st.median(hi) if hi else None)
  rests=sorted(re for t,re,s,ta,c in rows)
  q=lambda p: rests[min(len(rests)-1,math.ceil(p*len(rests))-1)]
  print('  rest/sync p50',q(.5),'p90',q(.9),'max',rests[-1],'frac>10ms',sum(1 for x in rests if x>10000)/len(rests))
  xs=[s for _,_,s,_,_ in rows]; ys=[re for _,re,_,_,_ in rows]
  print('  corr segs-rest',st.correlation(xs,ys))
  # tail vs rest anti-correlation: substitution check
  print('  corr tail-rest',st.correlation([ta for *_,ta,c in rows],ys))
  # by cell
  for cell in sorted(set(c for *_,c in rows)):
    R=[x for x in rows if x[4]==cell]
    print(f'   {cell:18} n={len(R)} med tot={st.median([x[0] for x in R]):.0f} med tail={st.median([x[3] for x in R]):.0f} med rest={st.median([x[1] for x in R]):.0f} med segs={st.median([x[2] for x in R]):.1f}')
  o=[x for x in own if x[0]<100000]
  print('  own sync med total',st.median([x[0] for x in own]),'med rest',st.median([x[2] for x in own]),'med segs',st.median([x[3] for x in own]),'med rest/(segs-1)',st.median([x[2]/(x[3]-1) for x in own if x[3]>1]))
  big=[x for x in own if x[2]>50000]; print('  own rest>50ms',big)
