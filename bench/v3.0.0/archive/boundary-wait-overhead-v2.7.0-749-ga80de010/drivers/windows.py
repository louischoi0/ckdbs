import json, glob, os, datetime
res = "/home/cdkbs/bench-runs/bw/res/"
for d in sorted(os.listdir(res)):
    if not os.path.isdir(res + d):
        continue
    hs = [json.load(open(f)) for f in glob.glob(res + d + "/*.host.json")]
    st = min(h["start_utc"] for h in hs)
    en = max(datetime.datetime.strptime(h["start_utc"], "%Y-%m-%d %H:%M:%S") + datetime.timedelta(seconds=h["duration_s"]) for h in hs)
    lds = [float(h["loadavg_start"].split()[0]) for h in hs]
    waited = sum(h["waited_s"] for h in hs)
    comp = sum(1 for h in hs if h["competing_start"] or h["competing_end"])
    print(f"{d:<12} runs={len(hs):<3} {st} .. {en.strftime('%H:%M:%S')}  load1 {min(lds):.2f}-{max(lds):.2f} waited_s={waited} competing_runs={comp}")
