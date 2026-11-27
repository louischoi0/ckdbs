import sys, json, subprocess
sys.argv = ["x"]; exec(open("/tmp/claude-1000/-home-ukjin-workspaces-ckdbs/4680a176-4bc0-4cdc-81db-65b9e15ed625/scratchpad/dv-close/vol.py").read().split('if __name__')[0])
S = "/tmp/claude-1000/-home-ukjin-workspaces-ckdbs/4680a176-4bc0-4cdc-81db-65b9e15ed625/scratchpad/dv-close"
res = {"cell3": {"A": [], "B": []}, "cell4": {}}
def hs(): return {"loadavg": open("/proc/loadavg").read().strip(), "busy": [l for l in subprocess.run(["pgrep","-a","-f","cc1plus|cmake --build|ctest"],capture_output=True,text=True).stdout.splitlines() if "pgrep" not in l]}
def save():
    json.dump(res, open(f"{S}/mounts.json", "w"), indent=1)
def hb(t): open(f"{S}/status", "w").write(f"{time.time():.0f} {t}\n")
which = sys.argv_real if False else None
import os
mode = os.environ.get("MODE", "3")
if mode == "3":
    vol = f"{R}/vol1.0"; res["cell3_host"] = hs()
    for i in range(12):
        for lab in (("A","B") if i % 2 == 0 else ("B","A")):
            hb(f"cell3 {i} {lab}"); res["cell3"][lab] += mount(f"c3{lab}", f"{R}/kds_server.{lab}", vol)
            save()
else:
    for tg in ("0.25", "1.0", "4.0"):
        vol = f"{R}/vol{tg}"; res["cell4"][tg] = {"off": [], "on": [], "host": hs()}
        for i in range(3):
            for k, extra in (("off", ""), ("on", "volume_census_at_mount = true\n")):
                hb(f"cell4 {tg} {i} {k}"); res["cell4"][tg][k] += mount(f"c4{k}", f"{R}/kds_server.B", vol, extra); save()
save(); hb("done")
