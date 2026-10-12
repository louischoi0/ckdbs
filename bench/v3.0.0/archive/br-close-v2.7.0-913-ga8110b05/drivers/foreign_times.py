import json, time
for r in (json.loads(l) for l in open("/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/logs/host-samples-stall.jsonl") if l.strip()):
    if r["foreign"]:
        print(time.strftime("%H:%M:%S", time.gmtime(r["t"])), r["load"], [f.split(None, 2)[1] + " " + f.split(None, 2)[2][:50] for f in r["foreign"]])
