import json, glob, sys
sys.path.insert(0,'/home/cdkbs/bench-runs/bq/verdicts/BAR-r1')
from rolls import parse
B='/home/cdkbs/bench-runs/bq/out-4cf30270'
for sha in ('3338cf0c','05cd011f'):
    for pl in ('segregated-no0','batch0','spread'):
        R=[json.load(open(glob.glob(rd+'/bl-mixed-*.json')[0])) for rd in sorted(glob.glob(f'{B}/bl/{sha}/{pl}/r*'))]
        print(sha,pl,'sched p99',[d['read_from_schedule']['p99_us'] for d in R],'p99.9',[d['read_from_schedule']['p99.9_us'] for d in R],
              'svc p99',[d['read_service']['p99_us'] for d in R],'rows/s',[d['inserter_rows_per_s'] for d in R],'secs',R[0]['seconds'],R[0]['durability'])
for dur in ('relaxed','group'):
    over1=over512=n=0; rates=[]
    for rd in sorted(glob.glob(f'{B}/bq/{dur}/*/r*')):
        if 'iso' in rd: continue
        d=json.load(open(glob.glob(rd+'/bl-mixed-*.json')[0]))
        rolls,_=parse(glob.glob(rd+'/*.s.log')[0])
        w0,w1=d['measure_from_mono_ns'],d['stop_at_mono_ns']
        rate=d['log_bytes_per_s']; rates.append(rate/2**20)
        for r in rolls:
            if int(r['start_ns'])>=w0 and int(r['end_ns'])<=w1:
                n+=1; b=rate*int(r['hold_us'])/1e6
                over1+= b>2**20; over512+= b>2**19
    print(dur,'rolls',n,'>1MiB',over1,'>512KiB',over512,'rate MiB/s %.1f-%.1f'%(min(rates),max(rates)),'fill1MiB ms %.1f-%.1f'%(1000/max(rates),1000/min(rates)))
