import sys, time
sys.path.insert(0, "/tmp/claude-1000/-home-cdkbs-ckdbs/00c1c148-cc8f-4b5f-ac05-d5dc4f399e1d/scratchpad")
import runner as rn
import bk_overhead_ab as drv
for i in range(3):
    a = drv.cpu_times(); time.sleep(3); b = drv.cpu_times()
    bf = drv.busy_frac(a, b)
    own = {2, 4, 6, 7}
    others = [v for c, v in bf.items() if c not in own]
    print({c: v for c, v in bf.items()}, "others mean %.3f" % (sum(others) / len(others)), "bad", rn.bad_procs())
