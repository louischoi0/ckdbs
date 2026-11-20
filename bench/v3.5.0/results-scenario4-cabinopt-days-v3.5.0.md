# Scenario 4 (cabin optimizer over business days) at `v3.5.0`

**One line.** **The `on` arm no longer measures a served Cabin, and that is the contract, not a regression**: since CX-S3b (`2d297055`, in `v3.4.13`), a Cabin the controller made serves a read only through an enabled administrator's plan that names it (`cabin-controller.md` PO11, CX-Q6 (a)). This driver names none, so no statement's step probes the controller's Cabins. In the `on` arm the controller still creates them on heat (five in run A, six with two drops in run B), but each holds one page, records no lookup (`coverage_miss_pct` and `hint_fail_pct` NULL in every snapshot) and is never extended (`extends` 0). `on` runs at the speed of `off`: 0.98x to 1.02x on `board` and `tape10k`, against 1.90x to 2.51x at `v3.3.0-38`, before the rule. Its server CPU on the boards is the `off` arm's (2.0 s against 2.0 s; `v3.3.0-38` 0.8 s). The `v3.4.14` binary shows the same on this host, as PO11 predicts. The `declared` arm (a Cabin declared up front, which the ladder still tries) serves as before: 1.46x to 1.92x over the walk on `board` and `tape10k`. The 18-statement byte comparison across the three arms passed on every day of every run. **The driver's `on` arm needs a `CREATE PLAN ... ACCESS <relation> CABIN (<column>)` per probed shape to measure the controller again**; that is not built.

Driver: `tools/scenario4_cabinopt_days.py` of the tag, unmodified, `--suffix a`, three servers (`off`, `on`, `declared`) with their PIDs for the CPU readings, two complete runs: **run A** at the server defaults (`s4`) and **run B** with `cabin_optimizer_cooldown_half_lives = 2` (`cd2-s4`), both with `decay_half_life = 5` and `cabin_optimizer_snapshot_interval_ms = 500`, as at `v3.3.0-38`. Comparator: **`results-scenario4-cabinopt-days-v3.3.0-38-ga40882af.md`, measured 2026-10-20 on another host** (AMD EPYC 9V74), so a ratio of the two runs is engine plus host; scenario 4 was not part of the `v3.4.14` run. The engine reading is the one-run `v3.4.14`-binary control on this host (section 6).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-20; run A 18:18:53 to 18:23:40 UTC, run B 18:23:40 to 18:28:27 UTC; the `v3.4.14`-binary control 18:39:40 to 18:44:28 UTC |
| Version, commit, binary, host, device, budget | stamped once in `summary-scenarios-v3.5.0.md`, section Stamp |
| Server config | the common config plus `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500`, and in run B `cabin_optimizer_cooldown_half_lives = 2`; `cores = 1`, `durability = group`; ports 15651 to 15653 |
| Driver | exit 0 in all three runs; `client_errors` 0, `slot_overruns` 0; elapsed 275.7 s each |

## 2. What was run

Per day, three days: an open (240 inserts into the day's board), a paced 45 s trading session (2,400 hot-board probes, 396 probes on each of three tapes of 200, 1,000 and 10,000 rows, 240 pk controls, per arm, in 12 blocks), a close (`COUNT` / `SUM` / `GROUP BY`, three rounds) and a 45 s idle night. qps is 1,000,000 / mean latency. Cut: the old-and-new interleaved A/B of `v3.3.0-38` (six runs; one `v3.4.14`-binary run was added instead, after the first two runs showed the controller's Cabins unprobed) and PostgreSQL (no server).

## 3. Throughput and latency by arm, run A

p50 and derived qps per group, day and arm; the ratios are of mean latencies (`on over off` = off mean / on mean, above 1 is faster); the last three columns are now over `v3.3.0-38` per arm (another host):

| group | day | off p50 / qps | on p50 / qps | declared p50 / qps | on over off | declared over off | off now over v3.3.0-38 | on now over v3.3.0-38 | declared now over v3.3.0-38 |
|---|---|---|---|---|---|---|---|---|---|
| board | 1 | 364.5 µs / 2,607 qps | 375.0 µs / 2,577 qps | 141.7 µs / 3,794 qps | 0.99x | 1.46x | 1.02x | 0.53x | 0.95x |
| board | 2 | 369.5 µs / 2,583 qps | 376.3 µs / 2,572 qps | 139.8 µs / 3,928 qps | 1.00x | 1.52x | 1.00x | 0.51x | 0.89x |
| board | 3 | 376.8 µs / 2,541 qps | 378.4 µs / 2,543 qps | 133.2 µs / 4,686 qps | 1.00x | 1.84x | 1.00x | 0.44x | 0.91x |
| tape200 | 1 | 86.7 µs / 10,672 qps | 87.9 µs / 9,804 qps | 85.0 µs / 10,515 qps | 0.92x | 0.99x | 1.06x | 0.97x | 1.06x |
| tape200 | 2 | 90.1 µs / 9,320 qps | 88.7 µs / 9,990 qps | 86.2 µs / 10,299 qps | 1.07x | 1.11x | 0.89x | 0.92x | 0.90x |
| tape200 | 3 | 87.8 µs / 10,204 qps | 87.7 µs / 10,060 qps | 86.8 µs / 9,921 qps | 0.99x | 0.97x | 0.98x | 0.95x | 0.91x |
| tape1k | 1 | 102.0 µs / 9,294 qps | 103.5 µs / 8,496 qps | 84.6 µs / 9,862 qps | 0.91x | 1.06x | 1.10x | 0.87x | 1.06x |
| tape1k | 2 | 104.2 µs / 8,518 qps | 102.6 µs / 9,416 qps | 85.8 µs / 9,980 qps | 1.11x | 1.17x | 0.98x | 0.88x | 0.91x |
| tape1k | 3 | 102.7 µs / 8,993 qps | 103.5 µs / 8,850 qps | 86.3 µs / 9,843 qps | 0.98x | 1.09x | 1.07x | 0.85x | 0.91x |
| tape10k | 1 | 322.4 µs / 2,948 qps | 326.7 µs / 2,940 qps | 91.4 µs / 4,498 qps | 1.00x | 1.53x | 1.04x | 0.51x | 0.96x |
| tape10k | 2 | 331.1 µs / 2,890 qps | 329.5 µs / 2,924 qps | 86.6 µs / 5,546 qps | 1.01x | 1.92x | 1.01x | 0.42x | 0.91x |
| tape10k | 3 | 323.4 µs / 2,946 qps | 325.9 µs / 2,934 qps | 92.2 µs / 4,975 qps | 1.00x | 1.69x | 1.04x | 0.41x | 0.91x |
| pk | 1 | 61.7 µs / 14,493 qps | 61.8 µs / 13,889 qps | 60.6 µs / 14,815 qps | 0.96x | 1.02x | 1.09x | 1.06x | 1.17x |
| pk | 2 | 62.7 µs / 13,812 qps | 62.6 µs / 14,327 qps | 61.6 µs / 15,504 qps | 1.04x | 1.12x | 1.01x | 1.02x | 1.05x |
| pk | 3 | 63.4 µs / 13,736 qps | 61.3 µs / 14,815 qps | 62.3 µs / 13,889 qps | 1.08x | 1.01x | 1.03x | 1.11x | 0.99x |
| open | 1 | 483.2 µs / 1,998 qps | 479.2 µs / 1,985 qps | 468.5 µs / 2,081 qps | 0.99x | 1.04x | 2.70x | 2.34x | 2.67x |
| open | 2 | 479.4 µs / 1,996 qps | 468.9 µs / 2,092 qps | 498.0 µs / 1,924 qps | 1.05x | 0.96x | 2.36x | 2.62x | 2.37x |
| open | 3 | 484.9 µs / 1,921 qps | 471.0 µs / 2,023 qps | 469.4 µs / 2,018 qps | 1.05x | 1.05x | 2.33x | 2.41x | 2.45x |
| close | 1 | 829.1 µs / 747 qps | 840.7 µs / 749 qps | 839.4 µs / 722 qps | 1.00x | 0.97x | 0.96x | 0.94x | 0.90x |
| close | 2 | 902.0 µs / 706 qps | 892.7 µs / 716 qps | 886.1 µs / 730 qps | 1.02x | 1.03x | 0.85x | 0.89x | 1.04x |
| close | 3 | 883.8 µs / 748 qps | 895.3 µs / 723 qps | 860.3 µs / 716 qps | 0.97x | 0.96x | 0.92x | 0.92x | 0.93x |

Run B (`cabin_optimizer_cooldown_half_lives = 2`) reads the same: `board` on over off 0.98x to 1.00x, declared over off 1.49x to 1.81x; `tape10k` 0.95x to 1.02x and 1.49x to 1.96x. Its full table is `archive/run-v3.5.0/tables/s4-cd2-s4.md`.

**What is resolved.** The `pk` control touches no Cabin; its on-over-off ratio is 0.96x to 1.08x, so a ratio under about 1.10x is not a finding. `on` is under it on every group and day. `declared` is clear of it on `board` and `tape10k`.

**Against `v3.3.0-38` (another host).** `off` is 0.98x to 1.10x on the walks: level, with the host's faster scan core and slower Python core moving in opposite directions, as the `v3.4.14` summary found. `open` is 2.3x to 2.7x faster, which is the device's sync (559 µs against 1,294 µs). `on` is 0.41x to 0.53x on `board` and `tape10k`, because its Cabins are no longer tried (PO11).

## 4. Server CPU

| arm | server CPU on the boards, 3 days | on the tapes | v3.3.0-38 boards | v3.3.0-38 tapes |
|---|---|---|---|---|
| off | 2.0 s | 0.4 s | 2.2 s | 0.5 s |
| on | 2.0 s | 0.4 s | 0.8 s | 0.2 s |
| declared | 1.0 s | 0.2 s | 1.0 s | 0.2 s |

Run B: boards / tapes `off` 2.0 s / 0.4 s, `on` 2.0 s / 0.4 s, `declared` 1.1 s / 0.3 s (`v3.3.0-38`: 2.2 / 0.4, 0.8 / 0.2, 1.0 / 0.2 s).

## 5. The controller's lifecycle

Run A, final snapshot of the `on` arm: `managed 5, pages_committed 5, creates 5, extends 0, heals 0, drops 0, failures 0`. The Cabins are `board_a_a.symbol` ACTIVE, `board_b_a.symbol` DECAYING, and `tape_200_a`, `tape_1k_a` and `tape_10k_a` `.symbol` ACTIVE, **each of one page, last action CREATE**. Run B: `creates 6, drops 2`, four left of one page each. At `v3.3.0-38` the same snapshot read `extends 172`, `board_a_a` at 18 pages, and every entry carried a coverage-miss share and a hint-failure share; here both are NULL in every evidence snapshot of both runs, since a NULL share is a zero lookup count. This is PO11's statement made visible: "an unnamed Cabin is probed by no statement's step, so it observes no value, sights none for an EXTEND to seed from ... it holds its catalog row and its budget page until it is named or decays". The probes keep their filter scan, as PO11 says they do.

## 6. The `v3.4.14` binary on this host

One run of the driver at the defaults on the `v3.4.14` binary (sha256 `78b3aede...`), 18:39:40 to 18:44:28 UTC: on over off `board` 0.99x / 1.02x / 0.98x and `tape10k` 1.02x / 1.02x / 0.97x (days 1 to 3); final `extends 0`, five Cabins of one page, coverage NULL; byte comparison 0 mismatches on three days. **`v3.4.14` reads the same as `v3.5.0`**, so nothing in `35086008..07f4552a` (DI-S2's re-keying of the execution signal among it) moved the controller on this workload. This run was added at 18:39 UTC, after runs A and B, before PO11 was read as the reason.

## 7. Correctness

The driver compares 18 statements' replies byte for byte across the three arms each day: 0 mismatches in 9 of 9 days (three runs). A Cabin no step tries changes no answer (Cabins are droppable without changing a result, `cabin.md` C1).

## 8. What the run says about the engine

- **The controller's own effect is not measurable by this driver at `v3.4.13` or later**: its Cabins are read only through a plan that names them (PO11), and the driver writes none. The numbers in the `on` column are the walk's. The `v3.3.0-38` ratios (1.90x to 2.51x) priced a ladder that tried every Cabin, which the engine no longer has.
- A declared Cabin serves as before (1.46x to 1.92x over the walk).
- The walk itself, the pk control and the close are level with `v3.3.0-38` within what a cross-host reading can say; the durable `open` is 2.3x to 2.7x faster, which is this host's device.

## 9. Noise floor, contamination and host

Every quiet-gate wait was 0 s. Two foreign samples above 40 % fell in scenario 4's windows: another session's `claude` process at 42.0 % at 18:25:31 and 18:28:27 UTC, both in run B. Neither reached 50 %, and nothing was discarded.

## 10. Archive

`bench/v3.5.0/archive/scenario4-v3.5.0/` (per-cell JSON, driver stdout, prechecks, gate waits, server configs and logs) and `bench/v3.5.0/archive/run-v3.5.0/` (the run scripts, watchdogs, monitor log, timeline, binary hashes, the table generator `gen.py` and its output under `tables/`). No data file is kept.
