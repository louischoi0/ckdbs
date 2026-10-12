import json,glob,re,statistics as st,sys
base='/home/cdkbs/bench-runs/bq/out-4cf30270/bq'
for dur in ('relaxed','group'):
  for cell in ('c3','c2','c1-batch0','c1-segregated-no0','c1-spread'):
    H=[];M=[]
    for rd in sorted(glob.glob(f'{base}/{dur}/{cell}/r*')):
      j=json.load(open(glob.glob(rd+'/bl-mixed-*.json')[0]))
      a=j['measure_from_mono_ns'];b=j['stop_at_mono_ns']
      for line in open(glob.glob(rd+'/*.s.log')[0]):
        if '[wal] roll' not in line: continue
        d=dict(re.findall(r'(\w+)=(\S+)',line))
        s,e=int(d['start_ns']),int(d['end_ns'])
        if s<a or e>b: continue
        r=(int(d['hold_us']),int(d['sync_gate_us']),int(d['body_wait_us'])+int(d['body_us']),int(d['waited_sync_tail_us'])+int(d['waited_sync_rest_us']),int(d['waited_syncs']),rd[-2:])
        (H if d['body']=='hit' else M).append(r)
    def s(L,i): return (st.mean(x[i] for x in L)/1000, st.median(x[i] for x in L)/1000) if L else None
    print(dur,cell,'hit n',len(H),'hold mean/med ms',s(H,0),'gate',s(H,1),'| miss n',len(M),'hold',s(M,0),'gate',s(M,1),'stepd',s(M,2))
    if cell=='c3' and dur=='relaxed':
      for x in sorted(M,key=lambda x:x[5]): print('  miss',x)
      print('  hits sorted holds ms', sorted(round(x[0]/1000) for x in H))
