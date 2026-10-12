import glob,os,statistics as st,math
exec(open('/home/cdkbs/bench-runs/bq/verdicts/H3-r1/r1.py').read().split('out={}')[0])
rows=[];ratios=[]
for cell in sorted(os.listdir(f'{B}/relaxed')):
  pooled=[]
  for rd in sorted(glob.glob(f'{B}/relaxed/{cell}/r*')):
    rs,_,_=load(rd); pooled+=rs
  for r in pooled:
    if r['waited_syncs']: rows.append(r['waited_sync_rest_us']/r['waited_syncs'])
  for r in p99set(pooled):
    if r['sync_gate_us']: ratios.append((cell,(r['waited_sync_tail_us']+r['waited_sync_rest_us'])/r['sync_gate_us']))
rows.sort(); print('per-roll avg rest n',len(rows),'p50',rows[len(rows)//2],'frac>10ms',sum(x>10000 for x in rows)/len(rows))
print(ratios)
