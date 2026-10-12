import json, glob, os, re, sys
B='/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
def rolls(log):
    out=[]
    for ln in open(log, errors='replace'):
        if '[wal] roll ' not in ln: continue
        kv=dict(re.findall(r'(\w+)=(\S+)', ln))
        out.append(kv)
    return out
def num(r,k): return int(r[k])
res={}
for dur in ['relaxed','group']:
  for cell in sorted(os.listdir(f'{B}/{dur}')):
    for rd in sorted(glob.glob(f'{B}/{dur}/{cell}/r*')):
      js=glob.glob(rd+'/*.json'); lg=glob.glob(rd+'/*.s.log')
      if not js or not lg: print('missing',rd); continue
      d=json.load(open(js[0])); R=rolls(lg[0])
      lo,hi=d['measure_from_mono_ns'],d['stop_at_mono_ns']
      W=[r for r in R if int(r['start_ns'])>=lo and int(r['end_ns'])<=hi]
      Wov=[r for r in R if int(r['end_ns'])>lo and int(r['start_ns'])<hi]
      def stats(S):
        H=sum(num(r,'hold_us') for r in S)
        M=sum(num(r,'hold_us') for r in S if r['body']!='hit')
        sd=sum(num(r,'body_wait_us')+num(r,'body_us') for r in S)
        cnt={}
        for r in S: cnt[r['body']]=cnt.get(r['body'],0)+1
        return H,M,sd,cnt
      H,M,sd,cnt=stats(W); Ha,Ma,sda,cnta=stats(R); Ho,Mo,sdo,cnto=stats(Wov)
      md=d['meta_delta']
      p=d.get('read_from_schedule') or d.get('writer_from_schedule') or {}
      res[(dur,cell,os.path.basename(rd))]=dict(H=H,M=M,sd=sd,cnt=cnt,p99=p.get('p99_us'),p999=p.get('p99.9_us'))
      print(f"{dur:7s} {cell:22s} {os.path.basename(rd)} n={len(W)} {cnt} miss%={100*M/H if H else 0:6.2f} sd%={100*sd/H if H else 0:5.2f} sd={sd} H={H} M={M} | all n={len(R)} {100*Ma/Ha if Ha else 0:6.2f} | overlap n={len(Wov)} {100*Mo/Ho if Ho else 0:6.2f} | meta rolls={md.get('wal_rolls')} hold={md.get('wal_roll_hold_us')} bw={md.get('wal_roll_body_wait_us')} bu={md.get('wal_roll_body_us')} hit/wt/in={md.get('wal_roll_body_hit')}/{md.get('wal_roll_body_waited')}/{md.get('wal_roll_body_inline')} p99={p.get('p99_us')} p999={p.get('p99.9_us')}")
      if cell=='c3':
        for r in W:
          if r['body']!='hit': print('   miss', r['seg'], 'hold',r['hold_us'],'gate',r['sync_gate_us'],'bw',r['body_wait_us'],'bu',r['body_us'],'create',r['create_us'])
import pickle; pickle.dump(res,open('/home/cdkbs/bench-runs/bq/verdicts/H4-r1/res.pkl','wb'))
