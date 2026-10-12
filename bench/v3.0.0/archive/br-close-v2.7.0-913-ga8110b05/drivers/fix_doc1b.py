p = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/doc1.tpl.md"
s = open(p).read()


def rep(a, b):
    global s
    assert a in s, a[:80]
    s = s.replace(a, b)


rep("The page-write\nphase is 17 ms of a 1.13 s checkpoint run (the data sync and the publish are the rest), so even a ten-fold error would not show\nin a run's wall time.", "The page-write\nphase is 17 ms of a 1.13 s checkpoint run (the rest of the run is not broken down here).")
rep("consistent with a few tens of nanoseconds and with zero)", "consistent with zero)")
open(p, "w").write(s)
a = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/drivers/assemble.py"
t = open(a).read()
t = t.replace("[{min(av):.3f}-{max(av):.3f}]", "[{min(av):.3f} to {max(av):.3f}]").replace("[{min(bv):.3f}-{max(bv):.3f}]", "[{min(bv):.3f} to {max(bv):.3f}]")
t = t.replace("[{min(aw):.0f}-{max(aw):.0f}]", "[{min(aw):.0f} to {max(aw):.0f}]").replace("[{min(bw):.0f}-{max(bw):.0f}]", "[{min(bw):.0f} to {max(bw):.0f}]")
open(a, "w").write(t)
print("ok")
