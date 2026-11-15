# CS-S0 premise: what an explicit transaction costs a client that only reads

An explicit transaction that issues only tuple-key SELECTs costs the client 15% to 91% of its group throughput against the same SELECTs in autocommit, and writes 64 WAL bytes and spends one transaction id per group where autocommit writes none and spends none. CS-Q1's bar (`instructions/v3.2.0/workorder-cs-read-only-transaction.md` section 4) is **met**: W is measurably costlier than A beyond the in-run spread in 23 of 36 cells, in every durability class, and at k = 1 and k = 4 nearly everywhere. At k = 16 most cells fall inside this run's noise. The noise floor here is wide (host load, below), so the cells that read "inside noise" are not a finding of equality: 35 of 36 have W's point estimate below A's.

| field | value |
|---|---|
| date/time executed | 2026-11-15 07:20-07:31 UTC |
| branch / commit | `worktree-cs-read-only-transaction` at `bbaff33a`; `git describe --tags` = `v3.4.11-3-gbbaff33a` |
| engine source | `git archive bbaff33a` exported to `/home/louis/bench-runs/cs-s0/src` (outside the worktree), so uncommitted edits in the worktree cannot be in the binary. No engine code differs from `32354dbe`. |
| binary | Release (`-DCMAKE_BUILD_TYPE=Release`), built 2026-11-15 07:20:35 UTC (after HEAD's commit time 07:12); measured copy `run/kds_server`, sha256 `93bfb49cee75c27de2fb1a54c99a4b7cdd1bc228446390ee4bb5b431bfebeae2` |
| device | data files and WAL under `/home/louis/bench-runs/cs-s0/data`, ext4 on `/dev/nvme0n1p1` (`df -T`), not tmpfs |
| server config | `cores` 1 and 4, `durability` strict / group / relaxed (config key, one fresh server and data file per pair), `buffer_pool_frames = 65536`, defaults otherwise; cores=1 server pinned to CPU 4, cores=4 to CPUs 4-7, clients to CPUs 0-3 |
| driver | `tools/cs_s0_premise.py` (archived under `scripts/`); text `BEGIN` / `COMMIT` statements; literal-text SELECTs |
| host state | **not quiet.** 8 CPUs; load average 11 to 21 during the run, 0 to 16 `cc1plus` processes (other agents' builds and a test suite) in every cell. Recorded per cell in `result.json`. |

## What was measured

Relation `cs_t (id int64, a int64, b int64) BTREE`, 10,000 rows, each read `SELECT * FROM cs_t WHERE id = <random 1..10000>`. A **group** is one unit: arm W sends `BEGIN`, k reads, `COMMIT` (k + 2 round trips); arm A sends the k reads in autocommit (k round trips). Arm A2 is a second, identical A arm: the noise control. Each cell is 2 repeats of 2.0 s per arm, the arm order rotated and alternately reversed per repeat. One client process per connection. Row count is fixed at 10,000: a tuple-key lookup does not scale with rows, and the 200 / 1K / 10K sweep of the benchmark rules was cut (below).

**Noise floor.** `spread` = (max - min) / mean over the four A and A2 runs of a cell. It runs 9% to 156%, median about 30%, because the host was loaded. A cell is read "W costlier" only if (A-mean - W-mean) / A-mean exceeds its own spread. A delta inside the spread is not reported as a result.

## Throughput, W against A

| cores | dur | conns | k | W groups/s | A groups/s | A2 groups/s | (W-A)/A | spread | reading |
|---|---|---|---|---|---|---|---|---|---|
| 1 | strict | 1 | 1 | 562 | 3344 | 2503 | -81% | 35% | W costlier |
| 1 | strict | 1 | 4 | 373 | 795 | 831 | -54% | 24% | W costlier |
| 1 | strict | 1 | 16 | 156 | 195 | 229 | -26% | 40% | inside noise |
| 1 | strict | 8 | 1 | 1557 | 11776 | 10060 | -86% | 28% | W costlier |
| 1 | strict | 8 | 4 | 1071 | 2832 | 2435 | -59% | 22% | W costlier |
| 1 | strict | 8 | 16 | 475 | 702 | 645 | -29% | 27% | W costlier |
| 1 | group | 1 | 1 | 635 | 3420 | 3059 | -80% | 39% | W costlier |
| 1 | group | 1 | 4 | 344 | 832 | 700 | -55% | 25% | W costlier |
| 1 | group | 1 | 16 | 156 | 190 | 191 | -18% | 28% | inside noise |
| 1 | group | 8 | 1 | 2061 | 10976 | 11221 | -81% | 18% | W costlier |
| 1 | group | 8 | 4 | 1208 | 2779 | 2869 | -57% | 43% | W costlier |
| 1 | group | 8 | 16 | 550 | 845 | 823 | -34% | 37% | inside noise |
| 1 | relaxed | 1 | 1 | 3122 | 10032 | 8147 | -66% | 52% | W costlier |
| 1 | relaxed | 1 | 4 | 1524 | 2709 | 1257 | -23% | 157% | inside noise |
| 1 | relaxed | 1 | 16 | 229 | 276 | 306 | -21% | 27% | inside noise |
| 1 | relaxed | 8 | 1 | 6831 | 17229 | 14074 | -56% | 29% | W costlier |
| 1 | relaxed | 8 | 4 | 3322 | 4415 | 4367 | -24% | 22% | W costlier |
| 1 | relaxed | 8 | 16 | 1621 | 2267 | 2559 | -33% | 47% | inside noise |
| 4 | strict | 1 | 1 | 849 | 8066 | 11110 | -91% | 84% | W costlier |
| 4 | strict | 1 | 4 | 407 | 2005 | 2556 | -82% | 67% | W costlier |
| 4 | strict | 1 | 16 | 191 | 547 | 534 | -65% | 92% | inside noise |
| 4 | strict | 8 | 1 | 1872 | 19432 | 18509 | -90% | 33% | W costlier |
| 4 | strict | 8 | 4 | 1378 | 4754 | 4642 | -71% | 12% | W costlier |
| 4 | strict | 8 | 16 | 484 | 1150 | 1170 | -58% | 22% | W costlier |
| 4 | group | 1 | 1 | 831 | 4187 | 7311 | -86% | 74% | W costlier |
| 4 | group | 1 | 4 | 410 | 1404 | 855 | -64% | 71% | inside noise |
| 4 | group | 1 | 16 | 236 | 518 | 509 | -54% | 19% | W costlier |
| 4 | group | 8 | 1 | 2943 | 18130 | 19767 | -84% | 18% | W costlier |
| 4 | group | 8 | 4 | 1960 | 4176 | 4691 | -56% | 23% | W costlier |
| 4 | group | 8 | 16 | 853 | 967 | 1091 | -17% | 22% | inside noise |
| 4 | relaxed | 1 | 1 | 3628 | 7217 | 4106 | -36% | 75% | inside noise |
| 4 | relaxed | 1 | 4 | 938 | 1292 | 1564 | -34% | 92% | inside noise |
| 4 | relaxed | 1 | 16 | 407 | 344 | 404 | +9% | 34% | inside noise |
| 4 | relaxed | 8 | 1 | 7802 | 18527 | 16856 | -56% | 30% | W costlier |
| 4 | relaxed | 8 | 4 | 3400 | 4803 | 4609 | -28% | 9% | W costlier |
| 4 | relaxed | 8 | 16 | 1146 | 1393 | 1307 | -15% | 32% | inside noise |

By durability class (cells beyond spread out of 12): strict 10, group 8, relaxed 5. By k: k=1 11, k=4 9, k=16 3. The k=16 cells fall inside the noise because 16 point reads dominate the group (about 2.5 to 5 ms at p50) and W's fixed addition is a smaller share of it; the point estimates still say W is 15% to 65% below A there, except one (cores=4, relaxed, 1 connection, +9%, inside a 34% spread).

W's extra cost has two parts this run does not separate: the engine's work (a transaction id, TXN_BEGIN, TXN_COMMIT, an in-flight slot and, under strict, a device sync) and **two extra client round trips** (`BEGIN`, `COMMIT`). Under `relaxed` at k = 1 the W group is 3 round trips against 1, so most of that class's delta may be the round trips. Strict against relaxed p50 at cores=1, 1 connection, k=1 (846 us against 193 us) says a strict sync adds about 650 us on this device; that part is the engine's.

## Latency per group, with WAL bytes

W pools its repeats; A pools A and A2. Microseconds. p0 / p25 / p50 / p75 / p99 per cell; the group count is the sample size.

| cores | dur | conns | k | arm | p0 | p25 | p50 | p75 | p99 (us) | groups | WAL B/group |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | strict | 1 | 1 | W | 498 | 705 | 846 | 1327 | 11039 | 2246 | 64 |
| 1 | strict | 1 | 1 | A | 61 | 78 | 89 | 132 | 5629 | 23388 | 0 |
| 1 | strict | 1 | 4 | W | 757 | 1023 | 1283 | 3447 | 12338 | 1491 | 64 |
| 1 | strict | 1 | 4 | A | 247 | 299 | 320 | 419 | 10880 | 6502 | 0 |
| 1 | strict | 1 | 16 | W | 1733 | 2713 | 5477 | 8516 | 18845 | 624 | 64 |
| 1 | strict | 1 | 16 | A | 1041 | 1231 | 2440 | 7279 | 21384 | 1697 | 0 |
| 1 | strict | 8 | 1 | W | 505 | 3067 | 4222 | 6404 | 14907 | 6227 | 64 |
| 1 | strict | 8 | 1 | A | 57 | 110 | 160 | 614 | 7497 | 87341 | 0 |
| 1 | strict | 8 | 4 | W | 809 | 4300 | 6386 | 9421 | 22024 | 4285 | 64 |
| 1 | strict | 8 | 4 | A | 258 | 638 | 1703 | 3990 | 15610 | 21066 | 0 |
| 1 | strict | 8 | 16 | W | 1912 | 11133 | 15845 | 21096 | 45892 | 1901 | 64 |
| 1 | strict | 8 | 16 | A | 1301 | 7933 | 11263 | 15181 | 31675 | 5386 | 0 |
| 1 | group | 1 | 1 | W | 480 | 693 | 785 | 1264 | 9723 | 2539 | 64 |
| 1 | group | 1 | 1 | A | 59 | 76 | 87 | 123 | 5446 | 25918 | 0 |
| 1 | group | 1 | 4 | W | 737 | 969 | 1136 | 4351 | 13822 | 1374 | 64 |
| 1 | group | 1 | 4 | A | 259 | 308 | 330 | 403 | 12583 | 6128 | 0 |
| 1 | group | 1 | 16 | W | 1691 | 2060 | 5337 | 8756 | 19899 | 624 | 64 |
| 1 | group | 1 | 16 | A | 1092 | 1594 | 3170 | 8037 | 22002 | 1526 | 0 |
| 1 | group | 8 | 1 | W | 565 | 1919 | 2381 | 5173 | 14946 | 8245 | 64 |
| 1 | group | 8 | 1 | A | 56 | 96 | 135 | 215 | 9987 | 88789 | 0 |
| 1 | group | 8 | 4 | W | 798 | 3042 | 5641 | 9511 | 19428 | 4831 | 64 |
| 1 | group | 8 | 4 | A | 261 | 446 | 671 | 3764 | 20023 | 22591 | 0 |
| 1 | group | 8 | 16 | W | 1830 | 9201 | 13379 | 18703 | 38235 | 2199 | 64 |
| 1 | group | 8 | 16 | A | 1091 | 3606 | 9018 | 13220 | 33628 | 6672 | 0 |
| 1 | relaxed | 1 | 1 | W | 135 | 179 | 193 | 212 | 4293 | 12486 | 64 |
| 1 | relaxed | 1 | 1 | A | 55 | 71 | 75 | 83 | 1196 | 72715 | 0 |
| 1 | relaxed | 1 | 4 | W | 295 | 400 | 446 | 534 | 4394 | 6096 | 64 |
| 1 | relaxed | 1 | 4 | A | 230 | 278 | 297 | 353 | 5973 | 15864 | 0 |
| 1 | relaxed | 1 | 16 | W | 1158 | 1992 | 2399 | 5983 | 23753 | 916 | 64 |
| 1 | relaxed | 1 | 16 | A | 1059 | 1305 | 1737 | 4685 | 15838 | 2327 | 0 |
| 1 | relaxed | 8 | 1 | W | 143 | 261 | 406 | 943 | 10021 | 27324 | 64 |
| 1 | relaxed | 8 | 1 | A | 56 | 97 | 166 | 262 | 6268 | 125213 | 0 |
| 1 | relaxed | 8 | 4 | W | 345 | 720 | 1036 | 3036 | 12690 | 13290 | 64 |
| 1 | relaxed | 8 | 4 | A | 247 | 417 | 670 | 1378 | 11513 | 35128 | 0 |
| 1 | relaxed | 8 | 16 | W | 1262 | 2237 | 4098 | 6455 | 19638 | 6485 | 64 |
| 1 | relaxed | 8 | 16 | A | 1250 | 2335 | 2904 | 3519 | 10665 | 19304 | 0 |
| 4 | strict | 1 | 1 | W | 499 | 696 | 764 | 879 | 6657 | 3395 | 64 |
| 4 | strict | 1 | 1 | A | 51 | 72 | 80 | 92 | 1030 | 76704 | 0 |
| 4 | strict | 1 | 4 | W | 795 | 1070 | 1357 | 2228 | 28482 | 1627 | 64 |
| 4 | strict | 1 | 4 | A | 219 | 294 | 319 | 355 | 3479 | 18245 | 0 |
| 4 | strict | 1 | 16 | W | 1529 | 2379 | 2837 | 4720 | 112316 | 765 | 65 |
| 4 | strict | 1 | 16 | A | 974 | 1482 | 1710 | 2047 | 6046 | 4323 | 0 |
| 4 | strict | 8 | 1 | W | 585 | 1334 | 2858 | 5843 | 18187 | 7486 | 64 |
| 4 | strict | 8 | 1 | A | 48 | 88 | 132 | 233 | 6249 | 151766 | 0 |
| 4 | strict | 8 | 4 | W | 705 | 1704 | 4182 | 8285 | 22855 | 5511 | 64 |
| 4 | strict | 8 | 4 | A | 216 | 324 | 507 | 1200 | 14708 | 37583 | 0 |
| 4 | strict | 8 | 16 | W | 1690 | 6002 | 9024 | 13486 | 698628 | 1938 | 64 |
| 4 | strict | 8 | 16 | A | 949 | 1863 | 4851 | 10176 | 28069 | 9281 | 0 |
| 4 | group | 1 | 1 | W | 409 | 604 | 696 | 896 | 8296 | 3323 | 64 |
| 4 | group | 1 | 1 | A | 57 | 72 | 77 | 90 | 3510 | 45991 | 0 |
| 4 | group | 1 | 4 | W | 699 | 1255 | 1582 | 2453 | 12333 | 1642 | 64 |
| 4 | group | 1 | 4 | A | 235 | 289 | 318 | 745 | 8271 | 9035 | 0 |
| 4 | group | 1 | 16 | W | 1520 | 2225 | 2916 | 5749 | 13327 | 943 | 64 |
| 4 | group | 1 | 16 | A | 931 | 1148 | 1203 | 1352 | 8652 | 4108 | 0 |
| 4 | group | 8 | 1 | W | 432 | 778 | 1150 | 3129 | 16955 | 11773 | 64 |
| 4 | group | 8 | 1 | A | 52 | 78 | 111 | 188 | 6736 | 151590 | 0 |
| 4 | group | 8 | 4 | W | 640 | 1333 | 2419 | 5384 | 22894 | 7842 | 64 |
| 4 | group | 8 | 4 | A | 223 | 335 | 524 | 1457 | 14674 | 35465 | 0 |
| 4 | group | 8 | 16 | W | 1583 | 4200 | 7593 | 12320 | 33152 | 3412 | 64 |
| 4 | group | 8 | 16 | A | 906 | 2542 | 5770 | 10669 | 31810 | 8230 | 0 |
| 4 | relaxed | 1 | 1 | W | 113 | 147 | 155 | 172 | 3252 | 14510 | 64 |
| 4 | relaxed | 1 | 1 | A | 56 | 72 | 77 | 91 | 4028 | 45290 | 0 |
| 4 | relaxed | 1 | 4 | W | 298 | 447 | 731 | 950 | 6378 | 3750 | 64 |
| 4 | relaxed | 1 | 4 | A | 235 | 290 | 337 | 888 | 4771 | 11424 | 0 |
| 4 | relaxed | 1 | 16 | W | 1046 | 1257 | 1380 | 2978 | 9677 | 1628 | 64 |
| 4 | relaxed | 1 | 16 | A | 966 | 1442 | 1662 | 3121 | 10215 | 2995 | 0 |
| 4 | relaxed | 8 | 1 | W | 129 | 218 | 320 | 663 | 10181 | 31208 | 64 |
| 4 | relaxed | 8 | 1 | A | 55 | 79 | 106 | 189 | 7230 | 141534 | 0 |
| 4 | relaxed | 8 | 4 | W | 313 | 520 | 844 | 2839 | 15475 | 13600 | 64 |
| 4 | relaxed | 8 | 4 | A | 225 | 346 | 557 | 1392 | 13953 | 37647 | 0 |
| 4 | relaxed | 8 | 16 | W | 1065 | 3190 | 5776 | 9340 | 26235 | 4585 | 64 |
| 4 | relaxed | 8 | 16 | A | 974 | 2215 | 3913 | 8247 | 25732 | 10801 | 0 |

## Cost breakdown (rule 3)

Not decomposed by wait type in this run. What was measured: the strict-minus-relaxed difference above (the sync share), WAL bytes, and ids. The `sys.wait_stats` decomposition was not read; the client round-trip share is not isolated (above).

## WAL bytes and ids per group

WAL bytes are the delta of `sys.meta`'s `wal_append_lsn` (the stream's byte position, read before and after each cell) over the cell's groups. **W writes 64 bytes per group in every cell (65 in one), A writes 0** (TXN_BEGIN and TXN_COMMIT, no data record); this holds at every k, durability, connection count and core count.

Ids: the engine exposes no cursor. The probe (`--id-probe`) reads `sys.lock_waits.holder_trx`: a held explicit transaction is made to block an autocommit UPDATE, and the holder's id is read; run before and after N groups of one connection. Each probe itself spends 3 ids (the baseline: `probe-only` 16797 -> 16800). **cores=1, all three durabilities:**

| arm | groups | ids spent over the groups (probe baseline 3 removed) | ids per group |
|---|---|---|---|
| A, k=16, strict | 190 | 0 | 0 |
| A, k=16, group | 267 | 0 | 0 |
| A, k=16, relaxed | 881 | 0 | 0 |
| W, k=1, strict | 571 | 571 | 1 |
| W, k=1, group | 868 | 868 | 1 |
| W, k=1, relaxed | 6,371 | 6,371 | 1 |
| W, k=16, strict | 219 | 219 | 1 |
| W, k=16, group | 362 | 362 | 1 |
| W, k=16, relaxed | 281 | 281 | 1 |

(Raw: e.g. W k=1 strict 16809 -> 17383 = 574 = 571 + 3.) **At cores=4 the probe is invalid**: each core carves its own id window and the probe's sessions land on different cores, so before/after ids are not comparable (`probes` in `result.json` shows them jumping); no ids-per-group is claimed there. The WAL byte count, which is core-independent, is 64 per W group at cores=4 as at cores=1, consistent with one TXN_BEGIN and one TXN_COMMIT.

## Reading of CS-Q1's bar

**Met.** W differs from A by more than the spread in a majority of cells at every durability class (strict 10/12, group 8/12, relaxed 5/12) and at k = 1 and k = 4; at k = 16 under group and relaxed the cells are mostly inside this run's noise, and the bar's wording ("less than the spread at every durability class and every k") is not satisfied for the build to stop. Per class: **strict**, clearly costlier at every k (k=16 inside spread at cores=1 one connection and cores=4 one connection, costlier at 8 connections); **group**, costlier at k = 1 and 4, mixed at k = 16; **relaxed**, costlier at k = 1 with 8 connections, and in the 1-connection k=4 and k=16 cells the spread (27% to 157%) swamps it. The reading is made on a loaded host and against client round trips that W also adds; neither weakens "met" for strict and for k = 1, 4, but a quiet-host re-run would be needed to size relaxed and k = 16.

What the run says about the engine: the read-only explicit transaction carries a fixed per-group cost (one id, 64 log bytes, a sync under strict) that is independent of k, which is the premise of CS section 1.2; it does not show how much of the relaxed-class delta is the id and log record rather than the two round trips.

## What was cut

- **Row sweep 200 / 1K / 10K** (rule 9): only 10,000 rows measured; a point lookup is row-independent but this run shows no evidence for it.
- Repeats: 2 repeats of 2.0 s per arm per cell (budget 20 minutes including a 3-minute build), so the spread is over 4 short runs.
- Wait-type decomposition (rule 3) not done; no `sys.wait_stats` read.
- Ids per group at cores=4 not measured (probe invalid across cores).
- PostgreSQL floor not measured for this shape; no prior kdbs number for this shape exists, so this file is the baseline.
- `bench/docs/` does not exist at this commit; the driver's docstring carries its flags and the command is: `python3 tools/cs_s0_premise.py --bin <copy> --workdir <dir> --out R.json --seconds 2 --reps 2 --id-probe`.
- Not run: the `tests/` suite (no engine code changed); no `--verify` mode exists, each read is checked to return one row.

Raw files: `bench/v3.4.11/archive/cs-s0-premise-v3.4.11-3-gbbaff33a/` (`result.json`, `status.log`, `driver.log`, `analyze.py`, `scripts/cs_s0_premise.py`).
