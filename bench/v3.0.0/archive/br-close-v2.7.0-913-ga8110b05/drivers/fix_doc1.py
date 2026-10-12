p = "/home/cdkbs/bench-runs/br-close-v2.7.0-913-ga8110b05/doc1.tpl.md"
s = open(p).read()


def rep(a, b):
    global s
    assert a in s, a[:70]
    s = s.replace(a, b)


rep("minus A (the commit BR opened from) reads `+0.00` to `+0.25` µs on the four pk shapes at 41-60 µs a statement (at most 0.5 %),",
    "minus A (the commit BR opened from) reads `-0.20` to `+0.25` µs on the four pk shapes at 41-60 µs a statement (at most 0.5 %),")
rep("the page-write phase of a 8,546-page checkpoint run.", "the page-write phase of an 8,546-page checkpoint run.")
rep("| replicate INSERT p50 delta, 3 sizes | +0.00 to +0.10 µs, intervals inside [-0.20, +0.20] µs |", "| replicate INSERT p50 delta, 3 sizes | +0.00 to +0.10 µs, intervals inside [-0.30, +0.20] µs |")
rep("- **Throughput.** `-109` to `+232` /s on 16,000-22,600 /s (at most 1.1 %)", "- **Throughput.** `-109` to `+232` /s on 15,700-22,600 /s (at most 1.1 %)")
rep("on 13.5-20 µs of server CPU.", "on 13.4-20.4 µs of server CPU.")
rep("""at 10,000 rows (`+0.41` [`+0.19`, `+0.55`] against `-0.41` [`-0.69`, `+0.17`] in A/A, the two intervals not overlapping, the
  difference of the medians 0.8 µs of which the A/A cell supplies half).""", """at 10,000 rows (`+0.41` [`+0.19`, `+0.55`] against `-0.41` [`-0.69`, `+0.17`] in A/A: the two intervals do not overlap, but the
  same shape with the same binary on both sides moves as far, in the other direction).""")
rep("throughput `-22` to `+19` /s on 780-840 /s (every interval contains zero), p99 `-917` to `+745` µs on a 5,000 µs p99\n  with intervals of the same width.",
    "throughput `-22` to `+19` /s on 770-845 /s (every interval contains zero), p99 `-917` to `+745` µs on 4,700-5,700 µs\n  with intervals of the same width.")
rep("B's reply carries 14 more fields (`wal_lineage`, `wal_durable_lsn`, the backup block and\n  the archive block), and it reads `+0.40` to `+0.60` µs slower at `cores = 1`, `+0.45` to `+0.65` µs at `cores = 4` (every\n  interval excludes zero); in the A/A cells it reads `+0.05` to `+0.15` µs.",
    "B's reply carries seven more fields (`wal_lineage`, `wal_durable_lsn`, `backup_state`,\n  `backup_log_bytes_copied`, `backup_fence_writes_waited`, `backups_completed`, `backups_failed`), and it reads `+0.40` to `+0.60` µs\n  slower at `cores = 1`, `+0.45` to `+0.65` µs at `cores = 4` (every interval excludes zero); in the A/A cells it reads `-0.20` to `+0.20` µs.")
rep("| server CPU (`sched_foreground_polled_us` per statement) | `SHOW META` around the blocks | 13.7 µs (33 %) | 17.4 µs (31 %) | 15.5 µs (35 %) | 13.7 µs (1.3 %, the relaxed INSERT's) |",
    "| server CPU (`sched_foreground_polled_us` per statement) | `SHOW META` around the blocks | 13.7 µs (33 %) | 17.4 µs (31 %) | 15.5 µs (35 %) | 28.1 µs (2.7 %) |")
rep("| client, socket round trip, framing | p50 minus server CPU | 27.8 µs (67 %) | 38.8 µs (69 %) | 28.5 µs (65 %) | 28 µs (2.7 %) |",
    "| client, socket round trip, framing | p50 minus server CPU | 27.8 µs (67 %) | 38.8 µs (69 %) | 28.5 µs (65 %) | 27.8 µs (2.7 %, the relaxed INSERT's) |")
rep("| durability wait (the commit's `fsync`) | `group` INSERT p50 minus `relaxed` INSERT p50 | - | - | - | 1,001 µs (96 %) |",
    "| durability wait (the commit's `fsync`) | what is left of the `group` INSERT p50 (1,001 µs is its excess over the `relaxed` INSERT) | - | - | - | 987 µs (94.6 %) |")
rep("| a checkpoint run inside the arm | `checkpoint_runs` | none in 192 of 192 server-runs | none | none | exactly one per server-run, in the tail: p99 5,264 µs against p50 1,043 µs |",
    "| a checkpoint run inside the arm | `checkpoint_runs` | none in 192 of 192 server-runs | none | none | exactly one per server-run; the arm's p99 is 5,264 µs against its p50 of 1,043 µs, the `fsync` tail, not shown to be the checkpoint |")
rep("a wide\nrelation of 40,000 rows (`id int64, v int64, payload varchar(1900)`, 100 rows an `INSERT`, about 8,500 pages)", "a wide\nrelation of 40,000 rows (`id int64, v int64, payload varchar(1900)`, 100 rows an `INSERT`)")
rep("Equal work: every run flushed exactly 8,546 pages.", "Equal work: the run read flushed exactly 8,546 pages in all sixteen.")
open(p, "w").write(s)
print("ok")
