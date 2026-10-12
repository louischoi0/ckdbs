p = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/doc2_c.md"
s = open(p).read()


def rep(a, b):
    global s
    assert a in s, a[:80]
    s = s.replace(a, b)


rep("2026-10-12, 17:42 to 18:20 UTC: build 17:42-17:47; backup re-takes 17:48-17:54; restores 17:55-18:07; the kill test 18:08-18:13; the 1,179 MiB-window comparison 18:14-18:19",
    "2026-10-12, 17:46 to 18:11 UTC: build 17:46-17:47; backup re-takes 17:47-17:51; restores 17:51-18:04; the kill tests 18:04-18:08; the 1,179 MiB-window comparison 18:08-18:11")
rep("mtime 17:47:02 UTC, after the commit;", "mtime 17:47:02 UTC, after the commit (17:40:36 UTC);")
rep("was 1,179 MiB (start LSN 3,019,927,616); with none it was 4,059 MiB", "was 1,179 MiB (start LSN 3,019,927,616, 17:47-17:49); with none it was 4,059 MiB")
rep("was because the previous", "was because the previous") if "was because the previous" in s else None
rep("this window applies 80,292 of 1,633,761 records, because the copied data file\n  was written back after most of them.", "this window applies 80,292 of 1,633,761 records, presumably because the copied data file\n  was written back after most of them (not checked).")
rep("The completion checkpoint is now 7.1 s of the mount: it writes the dirty pages before it begins, at\n  about 19 MiB/s of page writes it did not do before. For this window the alternative was a refusal.",
    "The completion checkpoint is now 7.1 s of the mount: it writes the dirty pages before it begins (how many pages\n  was not recorded). For this window the alternative was a refusal.")
rep("54.6-88.5 s for 5,222 MiB (59 MiB/s cold, 85-92 MiB/s warm)", "54.6-88.5 s for 5,222 MiB (59 MiB/s cold, 85-96 MiB/s warm)")
rep("Total restore time of the 3.96 GiB window is 67-99 s.", "Total restore time of the 3.96 GiB window is 65-99 s.")
rep("the 540,000-row ballast loaded alone (87.9 s, load 3.11\nbefore), `kill -9` at once", "the 540,000-row ballast loaded alone (87.9 s; the 1-minute load was not\nread before this run, it read 3.11 at the end of the first run of the test and 2.11 before the next cell), `kill -9` at once")
rep("B's first attempt is an outlier of unknown cause (the same attempt at the\nother three cells did not reproduce it), so two attempts per binary say only that C is not slower here: both completion\ncheckpoints are 2.3-3.0 s, because both write the pages (B writes them inside the checkpoint's run when the table fits the\nring). Redo applied 0 of 474,728 records in both.",
    "B's first attempt is an outlier of unknown cause (its second attempt, 4.1 s, and its\nserving run, 4.0 s, were not), so two attempts per binary say only that C is not slower here: the completion checkpoints are\n2.3-3.0 s in the serving runs; what B's wrote was not examined. Redo applied 0 of 474,728 records in both.")
rep("BR-Q3's\n  question (a checkpoint at `backup_start`) is now about restore time, not restorability.", "BR-Q3's\n  question (a checkpoint at `backup_start`) is, for this window, now about restore time and not about whether it opens.")
open(p, "w").write(s)

t = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/doc2.tpl.md"
d = open(t).read()
if "## 11. Re-measured at 90cdf918" not in d:
    d = d.rstrip("\n") + "\n" + s
d = d.replace("- **B** = `a8110b05` (`v2.7.0-913-ga8110b05`), BR closed in code. Binary `af25c71f…`.\n",
              "- **B** = `a8110b05` (`v2.7.0-913-ga8110b05`), BR closed in code. Binary `af25c71f…`.\n- **C** = `90cdf918` (`v2.7.0-915-g90cdf918`), which writes the mount's dirty pages before its completion checkpoint. Section 11 only: the\n  last row of the table above (the refused restore) opens there. Binary `bd98b0ea…`.\n", 1) if "- **C** = `90cdf918`" not in d else d
open(t, "w").write(d)
print("ok")
