import json, glob, re, math, statistics as st
from fractions import Fraction
B='/home/cdkbs/bench-runs/bq/out-4cf30270'
def nr(xs,p):
    xs=sorted(xs); r=max(1,math.ceil(Fraction(str(p))*len(xs)/100))-1; return xs[r]
RX=re.compile(r'\[wal\] roll (.*)')
CK=re.compile(r'\[checkpoint\] run: (.*)')
def kv(s):
    return {a:b for a,b in (t.split('=',1) for t in s.split() if '=' in t)}
def parse(log):
    rolls=[];cks=[]
    for line in open(log):
        m=RX.search(line)
        if m: rolls.append(kv(m.group(1)))
        m=CK.search(line)
        if m: cks.append(kv(m.group(1)))
    return rolls,cks
def corr(x,y):
    mx,my=st.mean(x),st.mean(y)
    sx=math.sqrt(sum((a-mx)**2 for a in x)); sy=math.sqrt(sum((b-my)**2 for b in y))
    return sum((a-mx)*(b-my) for a,b in zip(x,y))/(sx*sy)
def ovl(a0,a1,ivs):
    return any(s<a1 and e>a0 for s,e in ivs)
def cover(ivs,w0,w1):
    ivs=sorted((max(s,w0),min(e,w1)) for s,e in ivs if e>w0 and s<w1)
    tot=0;cs=ce=None
    for s,e in ivs:
        if cs is None: cs,ce=s,e
        elif s<=ce: ce=max(ce,e)
        else: tot+=ce-cs; cs,ce=s,e
    if cs is not None: tot+=ce-cs
    return tot/(w1-w0)
def main():
    out=[]
    def P(*a): out.append(' '.join(str(x) for x in a))
    for dur in ('relaxed','group'):
        P('=====',dur)
        pool=[]
        for cell in sorted(glob.glob(f'{B}/bq/{dur}/*')):
            cn=cell.rsplit('/',1)[1]
            cnts=[]
            for rd in sorted(glob.glob(cell+'/r*')):
                d=json.load(open(glob.glob(rd+'/bl-mixed-*.json')[0])); log=glob.glob(rd+'/*.s.log')[0]
                rolls,cks=parse(log)
                w0,w1=d['measure_from_mono_ns'],d['stop_at_mono_ns']
                inw=[r for r in rolls if int(r['start_ns'])>=w0 and int(r['end_ns'])<=w1]
                cnts.append(len(inw))
                if 'iso' not in cn: pool+= [dict(r,_cell=cn) for r in inw]
                # overlap analysis for mixed cells
                if 'iso' in cn or cn=='c3': continue
                role='writer' if cn=='c2' else 'reader'
                insc={c['core'] for c in d['clients'] if c['role']=='inserter'}
                samp=[s for c in d['clients'] if c['role']==role and c['core'] not in insc for s in c['slow'] if s[1]>=50_000_000]
                riv=[(int(r['start_ns']),int(r['end_ns'])) for r in rolls]
                civ=[(int(c['start_ns']),int(c['end_ns'])) for c in cks if 'start_ns' in c]
                n=len(samp)
                o=sum(ovl(a,a+l,riv) for a,l in samp)
                near=sum(any(abs((a+l)-e)<=20_000_000 or (s<=a+l<=e) for s,e in riv) for a,l in samp)
                oc=sum(ovl(a,a+l,civ) for a,l in samp)
                P(cn,rd[-2:],'n50ms',n,'ovl_roll %.0f%%'%(100*o/n if n else 0),'end<=20ms of roll end(or in roll) %.0f%%'%(100*near/n if n else 0),'ovl_ckpt %.0f%%'%(100*oc/n if n else 0),'roll cover %.0f%%'%(100*cover(riv,w0,w1)),'ckpt cover %.0f%%'%(100*cover(civ,w0,w1)))
            P(cn,'rolls in window',cnts,'sum',sum(cnts))
        h=[int(r['hold_us']) for r in pool]
        P('pooled rolls',len(h),'p50',nr(h,50),'p90',nr(h,90),'p99',nr(h,99),'max',max(h))
        tot=sum(h)
        for k in ('sync_gate_us','sync_us','create_us','body_wait_us','dirsync_us','header_us','seal_us','flush_us'):
            P(' share',k,'%.1f%%'%(100*sum(int(r[k]) for r in pool)/tot))
        P(' body',{b:sum(1 for r in pool if r['body']==b) for b in {r['body'] for r in pool}})
        P(' corr hold~sync_segs %.3f'%corr(h,[int(r['sync_segs']) for r in pool]), 'hold~waited_sync_tail %.3f'%corr(h,[int(r['waited_sync_tail_us']) for r in pool]))
        segs=[int(r['sync_segs']) for r in pool]
        P(' sync_segs range',min(segs),max(segs))
    open('/home/cdkbs/bench-runs/bq/verdicts/BAR-r1/rolls.out','w').write('\n'.join(out)+'\n'); print('\n'.join(out))
if __name__=='__main__': main()
