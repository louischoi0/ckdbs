p = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/doc2.tpl.md"
s = open(p).read()


def rep(a, b, count=None):
    global s
    assert a in s, a[:80]
    s = s.replace(a, b)


# thesis
rep("a base backup costs the workload its tail at `cores = 1` and little else; and a\nrestore is 17-22 s for a 1.1 GiB volume with a 200-330 MiB log window and 2 min 18 s for the same volume replayed through 9.9\nGiB of archive. One restore did not open: a backup whose log window is 3.6 GiB refuses at its completion checkpoint, and the\nsame refusal reproduces on the commit BR opened from, so it is the engine's.**",
    "a base backup costs the workload 4-13 % of its service rate and, at `cores = 1`, four times its p99 for the\nfew seconds the copy runs; and a restore is 17-22 s for a 1.1 GiB volume with a 200-330 MiB log window and 2 min 18 s for the\nsame volume replayed through 5.9 GiB of archive past its stop. One restore did not open: a backup whose log window is 3.4 GiB\nrefuses at its completion checkpoint, and the same refusal reproduces on the commit BR opened from, so it is the engine's.**")
rep("| restore, 1.1 GiB volume, 200-330 MiB window | 17.1-22.0 s (layout 8.7-11.3 s, mount 8.4-10.7 s) | replay (analysis + redo) 316-375 MiB/s of log |",
    "| restore, 1.1 GiB volume, 200-330 MiB window | 17.1-22.0 s (layout 8.7-11.3 s, mount 8.4-10.7 s) | replay (analysis + redo) 376 and 405 MiB/s of log |")
rep("| restore through 9.9 GiB of archive (PITR to `latest`) | 138.5 s (layout 93.5 s, mount 45 s) | |",
    "| restore through the archive past the stop (PITR to `latest`), 9.2 GiB of log replayed | 138.5 s (layout 93.5 s, mount 45 s) | replay 318 MiB/s |")
rep("| restore of a backup with a 3.6 GiB window |", "| restore of a backup with a 3.4 GiB window |")
rep("| archive on, small statements (`INSERT`, `SELECT`, `UPDATE` by pk) | -2.0 % to +0.5 % | inside the spread of the rounds (up to +-11.7 % on `UPDATE`, whose rounds range 18,900-22,100 /s) |",
    "| archive on, small statements (`INSERT`, `SELECT`, `UPDATE` by pk) | -2.0 % to +0.5 % (medians) | inside the spread of the rounds (up to +-11.7 % on `UPDATE`, whose rounds range 18,900-22,100 /s) |")
# stamp times
rep("M1a (archive) 16:19-16:26 and the tmpfs diagnostic 16:23-16:25; M1b (backup) 16:29-16:38;", "M1a (archive) 16:17-16:23 and the tmpfs diagnostic 16:23-16:26; M1b (backup) 16:29-16:39;")
# section 2
rep("Their p50 is unchanged to 0.3 µs, their p99 moves by +1.3 to +2.2 µs inside\nthe round-to-round range, and the paired throughput differences all contain zero except `INSERT` at `cores = 4` (`-0.9` %\n[`-1.4`, `-0.1`]). At 1.5 MB/s of log a `relaxed` `INSERT` stream seals a segment every 40 s, so the archiver copies nothing\ninside a 3,000-statement arm; the cost is paid, if anywhere, in the wide phase.",
    "Their p50 is unchanged to 0.3 µs, their p99 moves by -0.1 to +3.0 µs inside\nthe round-to-round range, and the paired throughput differences (median [min-max] over the five rounds) all span zero except\n`INSERT` at `cores = 4` (`-0.9` % [`-1.4`, `-0.1`], 0.9 points). Each small arm lasts 0.1-0.2 s against a 64 MiB segment, so the\narchiver has almost nothing to copy inside one; the cost is paid, if anywhere, in the wide phase.")
rep("which is exactly the statement rate, so the archive is not making each byte of log dearer; it is taking time the log\nwould have used.",
    "which is the same ratio as the statement rate (0.72 and 0.71), so the archive is not making each byte of log dearer; it is\ntaking time the log would have used.")
rep("**what the engine itself spends on archiving a segment\n(the floor-seal bookkeeping, the log device's held detached segments, the archiver thread's CPU) is about 3 %; the rest is\nbandwidth the log and the archive fight for.** The first is BR-R9's claim (\"the archiver never runs under the stream latch,\nso BR adds nothing to the hold\") holding on the measurement it can be held against; the second is a deployment fact:",
    "**what the engine itself spends on archiving (the archiver thread's CPU, the detached\nsegments it holds, its extra `fsync`s) is about 3 %; the rest is bandwidth the log and the archive fight for.** The first is\nconsistent with BR-R9 (\"the archiver never runs under the stream latch\", so a seal adds nothing to the hold) and is not a\nmeasurement of the latch; the second is a deployment fact:")
# section 3
rep("45-87\n% of every window, baseline included, is spent inside statements slower than 1 ms (the longest statements are 1-4.5 s).",
    "11-87\n% of a window, baseline included (median about half), is spent inside statements slower than 1 ms (the longest statements are 1-4.5 s).")
rep("**The service rate\nexcluding statements over 1 ms** (18,000 /s) is stable to about 3 % and is what the table compares.",
    "**The service rate\nexcluding statements over 1 ms** (16,000-18,700 /s) is stable to about +-4 % and is what the table compares.")
rep("- **Copy speed.** 1.06 GiB in 4.2-5.3 s: median 249 MiB/s against the 256 MiB/s throttle, in 13 of 15 backups 249-254 MiB/s\n  (the others 204-231).",
    "- **Copy speed.** 1.06 GiB in 4.2-5.3 s: median 249 MiB/s against the 256 MiB/s throttle, nine of 15 backups at 249-255 MiB/s\n  (the other six at 204-237).")
rep("- **Service rate.** `-8.0` % [`-12.7`, `-4.6`] at `cores = 1`, `-13.2` % [`-14.2`, `-6.8`] with the copier off the reactor's CPU,\n  `-3.9` % [`-5.4`, `-3.7`] at `cores = 4`, paired against the baseline just before each copy. The baseline windows themselves\n  spread 17,400-18,700 /s (`cores = 1`) and 16,050-17,300 /s (`cores = 4`), about 3.7 %, so the `cores = 4` figure is at the floor.",
    "- **Service rate** (median [min-max] over the five backups, each copy paired with the baseline just before it): `-8.0` %\n  [`-12.7`, `-4.6`] at `cores = 1`, `-13.2` % [`-14.2`, `-6.8`] with the copier off the reactor's CPU, `-3.9` % [`-5.4`, `-3.7`] at\n  `cores = 4`. The baseline windows themselves span 17,400-18,700 /s (`cores = 1`) and 16,050-17,300 /s (`cores = 4`), +-3.6 %, so\n  the `cores = 4` figure is at the floor.")
rep("The p50 of every statement kind rises by 0.5-1.0 µs at `cores = 1` (0.9 µs paired) and 1-3.3 µs at `cores = 4`\n  (3.5 µs paired).", "The p50 of every statement kind rises by 0.5-1.0 µs at `cores = 1` (0.9 µs paired) and 1-3.3 µs at `cores = 4`\n  (3.5 µs paired).")
# M2
rep("The\n  device, not the clients, is the bound: six writers got 54 MiB/s where one got 77 in M1a, because the checkpoint and the\n  archiver now compete with a deeper queue.",
    "The\n  clients are not the bound: six writers with the archive on got 54 MiB/s, what one writer got with it on in M1a (55 MiB/s).")
rep("the lag climbing to 5 segments (the 5 s windows exceed 80 MiB/s for a fifth of the run). Its own rate with the load stopped is\n  128-160 MiB/s (a 64 MiB segment every 0.4-0.5 s at the end of the trace), which is the device's, since the copy is read plus\n  write plus `fsync`.",
    "the lag climbing to 5 segments (50 of the 176 five-second windows exceed 80 MiB/s, three exceed 100). Its own rate with the\n  load stopped is about 170 MiB/s (the last 12 segments in 4.5 s, one every 0.25-0.5 s), the device's, since the copy is a read, a\n  write and an `fsync`.")
rep("- **The outage.** Five failures were counted in the 20 s (`wal_archive_failures` 5, the first at 47 s, retries after 1, 2, 4, 8 s of\n  backoff);",
    "- **The outage.** Five failures were counted by 62 s (`wal_archive_failures`: 2 by 47 s, 3 by 51 s, 4 by 56 s, 5 by 62 s, the backoff\n  doubling from 1 s);")
# M3
rep("- **Layout is the data file's copy.** 8.7-9.8 s warm and 10.5-11.3 s cold for 1.06-1.10 GiB, a rate of 112-126 MiB/s warm: the\n  copy is made durable, and the device writes 165 MB/s.",
    "- **Layout is the data file's copy.** 8.7-9.8 s warm and 10.5-11.3 s cold for backups of 1,080-1,099 MiB (data file plus log\n  window), 113-124 MiB/s warm; the device writes 165 MB/s.")
rep("**Replay, analysis plus redo, runs at 375 MiB/s of log at `cores = 1`, 329\n  MiB/s at `cores = 4`** (199 MiB in 0.53 s; 329 MiB in 1.0 s), and at 316 MiB/s over the 9.2 GiB of the PITR run (12.1 s analysis\n  plus 17.7 s redo for 9.2 GiB between the redo start and the log's end).",
    "**Replay, analysis plus redo, runs at 376 MiB/s of log at `cores = 1` and 405\n  MiB/s at `cores = 4`** (199 MiB in 0.53 s; 329 MiB in 0.81 s), and at 318 MiB/s over the 9.2 GiB of the PITR run (12.1 s analysis\n  plus 17.7 s redo for the 9.2 GiB between the redo start and the log's end).")
i0 = s.index("- **Restore time is about 17 s plus")
i1 = s.index("- **A restore of the idle backup of this server does not open.**")
s = s[:i0] + s[i1:]
rep("The idle backup (section 4) has a 3.4 GiB window (start LSN\n  618,004,624, stop 4,256,699,640). Its restore ran 3 times (layout 38.1-38.4 s, mount 10.8-11.3 s) and each ended\n  `startup failed: recovery of core 0: completion checkpoint: WalStream: record of 1408648 bytes is larger than the ring`\n  and exit status 1.",
    "The idle backup (section 4) has a 3,470 MiB window (start LSN\n  618,004,624, stop 4,256,699,640). Its restore ran 3 times (layout 38.1-38.4 s, mount 10.8-11.3 s) and each exited with status 1\n  after 49 s; a debug rerun printed `startup failed: recovery of core 0: completion checkpoint: WalStream: record of 1408648\n  bytes is larger than the ring`.")
# section 6
rep("Redo of\na long window dirties every page the window touched: 135,000 pages for this ballast.", "Redo of\na long window dirties every page the window touched: of the order of 115,000 pages for this ballast (the two refusals below are\n1.38 and 1.41 MB).")
rep("The M1b backups were taken 6-9\ns after their loads' ends and had windows of 110-380 MiB;", "The M1b backups were taken 15 s or more\nafter their loads' ends and had windows of 113-382 MiB;")
rep("the M2 idle backup was taken 5 s after a 78 s load at `cores = 4` and\nhad 3.4 GiB.", "the M2 idle backup was taken 5 s after a 78 s load at `cores = 4` and\nhad 3,470 MiB.")
rep("A restore that goes through the archive past the stop opened here (rows below), probably because", "A restore that goes through the archive past the stop opened here (section 5), probably because")
# section 7
rep("so an archive fault ends in refused writes long before the engine has run out of anything else if free space is the scarce resource, and not at all if it is not.", "whether 4 segments is the right reserve depends on how fast free space falls once archiving fails, which is the generation rate: 106.7 MiB/s at the peak.")
rep("(the 3.4 GiB-window idle backup: copy 4.2 s, finish 33.9 s); the ones under load 14-16 s. 1 h is 95-250x.", "(the 3,470 MiB-window idle backup: copy 4.2 s, finish 33.9 s); the ones under load 14-17 s. 1 h is 95-250x.")
# section 8
rep("| a base backup, path B, 1.06 GiB | the paced copy | 4.2-5.3 s (30-35 % of the 14-16 s) | `BACKUP COPY` to `copying=0` |", "| a base backup, path B, 1.06 GiB | the paced copy | 4.2-5.3 s (about 30 % of the 14-17 s) | `BACKUP COPY` to `copying=0` |")
rep("| | the finish: data file sync, log window copied, manifest | 9.4-11.5 s (65-70 %) | `BACKUP STOP` to `last_id` set |", "| | the finish: data file sync, log window copied, manifest | 9.4-11.5 s (about 70 %) | `BACKUP STOP` to `last_id` set |")
rep("| a restore, 1.1 GiB, 200-330 MiB window | layout (copy + verify) | 8.7-11.3 s (51-52 %) | first stdout line |", "| a restore, 1.1 GiB, 200-330 MiB window | layout (copy + verify) | 8.7-11.3 s (about half) | first stdout line |")
rep("| | the mount | 8.4-10.7 s (48-49 %), of which analysis + redo + undo + checkpoint 0.5-0.8 s | second stdout line, `SHOW META` |", "| | the mount | 8.4-10.7 s (about half), of which analysis + redo + undo + checkpoint 0.5-0.8 s | second stdout line, `SHOW META` |")
rep("| | the data copy's `fsync`, relation checks, pool setup | the 8 s of the mount that the counters do not name | not measured: no sampler run |", "| | unattributed: the mount's time outside the recovery phases | 7.8-9.9 s | not measured: no sampler run |")
# section 9
rep("The copy is 30-35 % of a backup's time", "The copy is about 30 % of a backup's time")
open(p, "w").write(s)
print("ok")
