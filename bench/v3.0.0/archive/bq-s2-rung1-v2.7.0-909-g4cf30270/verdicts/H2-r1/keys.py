import json,glob,sys
f=glob.glob('/home/cdkbs/bench-runs/bq/s2/out/relaxed/c1-segregated-no0/base/r1/*.json')[0]
j=json.load(open(f))
for k,v in j.items():
    s=json.dumps(v)
    print(k, s[:300])
