import json,glob,re,statistics as st
B='/home/cdkbs/bench-runs/bq/out-4cf30270/bq/relaxed/c3'
waits=[]
for rd in sorted(glob.glob(B+'/r*')):
    d=json.load(open(glob.glob(rd+'/*.json')[0])); lo,hi=d['measure_from_mono_ns'],d['stop_at_mono_ns']
    R=[dict(re.findall(r'(\w+)=(\S+)',l)) for l in open(glob.glob(rd+'/*.s.log')[0],errors='replace') if '[wal] roll ' in l]
    W=[r for r in R if int(r['start_ns'])>=lo and int(r['end_ns'])<=hi]
    hit=[int(r['hold_us']) for r in W if r['body']=='hit']; mis=[int(r['hold_us']) for r in W if r['body']!='hit']
    srt=sorted((int(r['hold_us']),r['body']) for r in W)
    waits+= [int(r['body_wait_us']) for r in W if r['body']!='hit']
    gs=sum(int(r['sync_gate_us']) for r in W); H=sum(int(r['hold_us']) for r in W)
    print(rd[-2:], 'hit p50',st.median(hit),'hit mean',round(st.mean(hit)),'miss mean',round(st.mean(mis)),'gate share',round(100*gs/H,2),'top3',srt[-3:], 'miss-minus-wait share', round(100*(sum(mis)-sum(int(r['body_wait_us']) for r in W if r['body']!='hit'))/H,2))
print(sorted(waits), st.median(waits), sum(w>30000 for w in waits))
