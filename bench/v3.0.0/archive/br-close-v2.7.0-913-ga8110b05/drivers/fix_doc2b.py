p = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/doc2.tpl.md"
s = open(p).read()


def rep(a, b):
    global s
    assert a in s, a[:80]
    s = s.replace(a, b)


rep("2026-10-12, 16:19 to 17:30 UTC.", "2026-10-12, 16:17 to 17:30 UTC.")
rep("with no foreign process at 20 % of a CPU (section 7).", "with no foreign process at 20 % of a CPU (the host record at the end).")
rep("the outage left 27 segments held after 20 s and 39 after the 12 s of backoff, whether 4 segments is the right reserve depends on how fast free space falls once archiving fails, which is the generation rate: 106.7 MiB/s at the peak.",
    "the outage left 27 segments held after 20 s and 39 after the 12 s of backoff that followed the repair; whether 4 segments is the right reserve depends on how fast free space falls once archiving fails, which is the generation rate (106.7 MiB/s at the peak).")
rep("The copy runs at 249 MiB/s median (204-254)", "The copy runs at 249 MiB/s median (204-255)")
rep("Restored through the archive to `latest` the same backup opens (rows below).", "Restored through the archive to `latest` the same backup opens (the last table above).")
rep("whose payload is the dirty-page table, 12 bytes a page.", "whose payload carries the dirty-page table (`wal.md` §8: the active-transaction table and the dirty-page table with recovery LSNs).")
rep("so a table of more than about 87,000 pages is a record \"larger than the ring\".", "so a dirty-page table of more than the order of 100,000 pages is a record \"larger than the ring\" (the two refusals below are 1.38 and 1.41 MB).")
rep("of the order of 115,000 pages for this ballast (the two refusals below are\n1.38 and 1.41 MB).", "of the order of 115,000 pages for this ballast.")
i = s.index("M1a and M2 were not sampled per second")
s = s[:i] + """M1a and M2 were not sampled per second: their rounds recorded `/proc/loadavg` at the start and end (M1a 1.7-3.4, M2 1.61 to
2.88) and no compiler or test binary was live in any `pgrep` list. The stall census's sampler file covers both passes
(17:15:42-17:29:22): the first pass (17:15-17:21) was discarded because its driver failed on A, and eight sampled seconds in it
(17:15:44-17:18:05) show another session's `ugrep` and `python3` jobs at 60-100 % of a CPU; in the second pass (17:21-17:29),
which is the table in section 3, two sampled seconds show a foreign process at 20 % (`ps` reports a lifetime average, so a
short-lived job reads 20 %). The census interleaves A and B, so a foreign job lands on both. The M3 and M1b windows had none.
"""
open(p, "w").write(s)

a = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/drivers/assemble.py"
t = open(a).read()
t = t.replace('m1b = m1b.replace("\\npaired copy - preceding baseline, ", "\\nPaired, each copy against the baseline just before it (median [min-max] over the backups): ")',
              'import re\nm1b = re.sub(r"(^#### .*\\n)(\\|)", r"\\1\\n\\2", m1b, flags=re.M)')
open(a, "w").write(t)
print("ok")
