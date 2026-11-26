# Scenario 4 (cabin optimizer over business days) at `v3.4.17-159-g9f8f8b354`

**One line.** **The `on` arm is still the walk, as at `v3.5.0`, and that is the contract** (`cabin-controller.md` PO11): in four runs on two binaries the controller creates five or six Cabins on heat, each holds one page, records no lookup and is never extended, because no statement's step probes a controller-made Cabin without an administrator's plan naming it, and this driver writes none. The `declared` arm serves: 1.65x to 2.35x over the walk on `board` and 1.66x to 2.21x on `tape10k`. **On this host `on over off` is not level on `board`**: 1.08x to 1.25x in mean latency on all twelve days of the four runs, the `07f4552a` binary's among them, with the two arms' p50 within 5 % of each other in the measured binary's runs. It is the same on both binaries with no lookup recorded on either, so it is not a Cabin serving and not a change between the two engines; the `07f4552a` binary read 0.99x to 1.00x on the `v3.5.0` run's host (one run there, one here), so the ratio follows the host, and what on this host produces it is unverified. New over old on this host is one run against one and scatters 0.81x to 1.22x on the probe groups, without a claim. The 18-statement byte comparison passed on every day of every run.

Driver: `tools/scenario4_cabinopt_days.py` of the measured commit, unmodified (byte-identical to `07f4552a`'s), `--suffix a`, three servers (`off`, `on`, `declared`) with their PIDs for the CPU readings, both with `decay_half_life = 5` and `cabin_optimizer_snapshot_interval_ms = 500`: **run A** at the server defaults (`s4`, and again as `r2-s4` because the first overlapped a build; section 9) and **run B** with `cabin_optimizer_cooldown_half_lives = 2` (`cd2-s4`). Comparator run: `results-scenario4-cabinopt-days-v3.5.0.md` (2026-11-20, `07f4552a`, **another host**). The engine reading is one run of the `07f4552a` binary on this host (`ab-old1-s4`, section 6).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-26; run A 21:15:10 to 21:19:43 UTC; the `07f4552a`-binary control 21:42:01 to 21:46:34 UTC; run B 21:46:55 to 21:51:28 UTC; run A again 22:07:46 to 22:12:20 UTC |
| Version, commit, binary, host, device, budget | stamped once in `summary-scenarios-v3.4.17-159-g9f8f8b354.md`, section Stamp |
| Server config | the common config plus `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500`, and in run B `cabin_optimizer_cooldown_half_lives = 2`; `cores = 1`, `durability = group`; ports 15751 to 15753 |
| Driver | exit 0 in every run; `client_errors` 0, `slot_overruns` 0; elapsed 272.5 s to 273.1 s |

## 2. What was run

Per day, three days: an open (240 inserts into the day's board), a paced 45 s trading session (2,400 hot-board probes, 396 probes on each of three tapes of 200, 1,000 and 10,000 rows, 240 pk controls, per arm, in 12 blocks), a close (`COUNT` / `SUM` / `GROUP BY`, three rounds) and a 45 s idle night. qps is 1,000,000 / mean latency. Cut, as at `v3.5.0`: the old-and-new interleaved A/B of six runs (one `07f4552a`-binary run instead) and PostgreSQL (no server).

## 3. Throughput and latency by arm

p50 and derived qps per group, day and arm; the ratios are of mean latencies (`on over off` = off mean / on mean, above 1 is faster); the last two columns are the same two ratios in the `v3.5.0` run.

Run A (`s4`; overlapped another session's build, section 9):

| group | day | off p50 / qps | on p50 / qps | declared p50 / qps | on over off | declared over off | on over off, v3.5.0 run | declared over off, v3.5.0 run |
|---|---|---|---|---|---|---|---|---|
| board | 1 | 202.7 µs / 4,335 qps | 203.1 µs / 4,843 qps | 71.6 µs / 7,669 qps | 1.12x | 1.77x | 0.99x | 1.46x |
| board | 2 | 206.4 µs / 4,088 qps | 207.5 µs / 4,764 qps | 74.0 µs / 7,663 qps | 1.17x | 1.87x | 1.00x | 1.52x |
| board | 3 | 210.7 µs / 3,923 qps | 205.0 µs / 4,552 qps | 72.1 µs / 9,234 qps | 1.16x | 2.35x | 1.00x | 1.84x |
| tape200 | 1 | 50.3 µs / 20,040 qps | 50.3 µs / 19,724 qps | 43.7 µs / 21,552 qps | 0.98x | 1.08x | 0.92x | 0.99x |
| tape200 | 2 | 50.7 µs / 19,802 qps | 50.8 µs / 19,231 qps | 48.9 µs / 19,881 qps | 0.97x | 1.00x | 1.07x | 1.11x |
| tape200 | 3 | 48.9 µs / 20,202 qps | 46.1 µs / 19,380 qps | 47.4 µs / 22,124 qps | 0.96x | 1.10x | 0.99x | 0.97x |
| tape1k | 1 | 60.7 µs / 17,153 qps | 60.7 µs / 16,529 qps | 46.2 µs / 20,000 qps | 0.96x | 1.17x | 0.91x | 1.06x |
| tape1k | 2 | 61.2 µs / 16,529 qps | 61.4 µs / 16,000 qps | 49.0 µs / 19,048 qps | 0.97x | 1.15x | 1.11x | 1.17x |
| tape1k | 3 | 59.4 µs / 17,422 qps | 53.6 µs / 17,452 qps | 47.3 µs / 21,882 qps | 1.00x | 1.26x | 0.98x | 1.09x |
| tape10k | 1 | 179.2 µs / 5,627 qps | 181.3 µs / 5,534 qps | 47.7 µs / 10,111 qps | 0.98x | 1.80x | 1.00x | 1.53x |
| tape10k | 2 | 182.1 µs / 5,426 qps | 185.8 µs / 5,426 qps | 49.1 µs / 10,741 qps | 1.00x | 1.98x | 1.01x | 1.92x |
| tape10k | 3 | 185.6 µs / 5,258 qps | 181.9 µs / 5,348 qps | 48.0 µs / 10,604 qps | 1.02x | 2.02x | 1.00x | 1.69x |
| pk | 1 | 36.6 µs / 25,316 qps | 36.3 µs / 26,316 qps | 31.5 µs / 29,586 qps | 1.04x | 1.17x | 0.96x | 1.02x |
| pk | 2 | 36.5 µs / 27,100 qps | 36.4 µs / 26,178 qps | 36.1 µs / 26,738 qps | 0.97x | 0.99x | 1.04x | 1.12x |
| pk | 3 | 36.5 µs / 26,385 qps | 36.1 µs / 24,096 qps | 34.7 µs / 29,499 qps | 0.91x | 1.12x | 1.08x | 1.01x |
| open | 1 | 153.1 µs / 6,345 qps | 151.4 µs / 6,481 qps | 153.0 µs / 6,086 qps | 1.02x | 0.96x | 0.99x | 1.04x |
| open | 2 | 199.9 µs / 4,348 qps | 196.8 µs / 4,305 qps | 176.2 µs / 4,762 qps | 0.99x | 1.10x | 1.05x | 0.96x |
| open | 3 | 159.2 µs / 5,120 qps | 172.1 µs / 1,224 qps | 164.3 µs / 5,157 qps | 0.24x | 1.01x | 1.05x | 1.05x |
| close | 1 | 620.4 µs / 826 qps | 558.1 µs / 1,164 qps | 694.2 µs / 864 qps | 1.41x | 1.05x | 1.00x | 0.97x |
| close | 2 | 501.1 µs / 947 qps | 431.6 µs / 1,405 qps | 500.7 µs / 1,364 qps | 1.48x | 1.44x | 1.02x | 1.03x |
| close | 3 | 502.6 µs / 968 qps | 495.4 µs / 1,264 qps | 492.2 µs / 1,281 qps | 1.31x | 1.32x | 0.97x | 0.96x |

Run A again (`r2-s4`):

| group | day | off p50 / qps | on p50 / qps | declared p50 / qps | on over off | declared over off | on over off, v3.5.0 run | declared over off, v3.5.0 run |
|---|---|---|---|---|---|---|---|---|
| board | 1 | 202.2 µs / 4,031 qps | 204.1 µs / 4,883 qps | 73.1 µs / 6,817 qps | 1.21x | 1.69x | 0.99x | 1.46x |
| board | 2 | 194.4 µs / 3,914 qps | 203.9 µs / 4,227 qps | 68.5 µs / 6,477 qps | 1.08x | 1.65x | 1.00x | 1.52x |
| board | 3 | 216.8 µs / 3,748 qps | 211.5 µs / 4,669 qps | 73.5 µs / 8,525 qps | 1.25x | 2.27x | 1.00x | 1.84x |
| tape200 | 1 | 49.9 µs / 19,084 qps | 50.4 µs / 19,569 qps | 48.6 µs / 20,877 qps | 1.03x | 1.09x | 0.92x | 0.99x |
| tape200 | 2 | 41.3 µs / 19,841 qps | 43.5 µs / 14,728 qps | 39.9 µs / 23,310 qps | 0.74x | 1.17x | 1.07x | 1.11x |
| tape200 | 3 | 51.2 µs / 18,382 qps | 50.7 µs / 19,802 qps | 48.4 µs / 21,008 qps | 1.08x | 1.14x | 0.99x | 0.97x |
| tape1k | 1 | 60.3 µs / 17,153 qps | 60.9 µs / 16,639 qps | 48.9 µs / 19,380 qps | 0.97x | 1.13x | 0.91x | 1.06x |
| tape1k | 2 | 50.5 µs / 20,325 qps | 52.8 µs / 18,797 qps | 39.7 µs / 24,450 qps | 0.92x | 1.20x | 1.11x | 1.17x |
| tape1k | 3 | 62.1 µs / 16,103 qps | 61.5 µs / 16,447 qps | 48.4 µs / 20,790 qps | 1.02x | 1.29x | 0.98x | 1.09x |
| tape10k | 1 | 178.7 µs / 5,612 qps | 182.2 µs / 5,485 qps | 49.0 µs / 9,320 qps | 0.98x | 1.66x | 1.00x | 1.53x |
| tape10k | 2 | 172.9 µs / 5,402 qps | 180.7 µs / 4,933 qps | 40.3 µs / 11,468 qps | 0.91x | 2.12x | 1.01x | 1.92x |
| tape10k | 3 | 189.4 µs / 4,950 qps | 187.8 µs / 5,230 qps | 48.9 µs / 10,132 qps | 1.06x | 2.05x | 1.00x | 1.69x |
| pk | 1 | 35.9 µs / 24,752 qps | 36.3 µs / 26,667 qps | 35.6 µs / 28,736 qps | 1.08x | 1.16x | 0.96x | 1.02x |
| pk | 2 | 29.8 µs / 23,866 qps | 31.6 µs / 20,833 qps | 29.0 µs / 33,113 qps | 0.87x | 1.39x | 1.04x | 1.12x |
| pk | 3 | 36.7 µs / 22,124 qps | 36.4 µs / 26,316 qps | 35.7 µs / 25,381 qps | 1.19x | 1.15x | 1.08x | 1.01x |
| open | 1 | 146.2 µs / 6,357 qps | 167.8 µs / 5,981 qps | 154.1 µs / 6,013 qps | 0.94x | 0.95x | 0.99x | 1.04x |
| open | 2 | 157.1 µs / 4,933 qps | 161.2 µs / 5,280 qps | 165.7 µs / 5,051 qps | 1.07x | 1.02x | 1.05x | 0.96x |
| open | 3 | 161.2 µs / 5,907 qps | 158.6 µs / 6,184 qps | 163.4 µs / 5,609 qps | 1.05x | 0.95x | 1.05x | 1.05x |
| close | 1 | 582.9 µs / 900 qps | 620.0 µs / 1,045 qps | 484.2 µs / 1,338 qps | 1.16x | 1.49x | 1.00x | 0.97x |
| close | 2 | 625.2 µs / 835 qps | 572.9 µs / 1,162 qps | 537.2 µs / 1,199 qps | 1.39x | 1.44x | 1.02x | 1.03x |
| close | 3 | 524.1 µs / 908 qps | 497.7 µs / 1,261 qps | 426.4 µs / 981 qps | 1.39x | 1.08x | 0.97x | 0.96x |

Run B (`cd2-s4`, `cabin_optimizer_cooldown_half_lives = 2`):

| group | day | off p50 / qps | on p50 / qps | declared p50 / qps | on over off | declared over off | on over off, v3.5.0 run | declared over off, v3.5.0 run |
|---|---|---|---|---|---|---|---|---|
| board | 1 | 204.7 µs / 3,928 qps | 203.3 µs / 4,880 qps | 72.6 µs / 7,047 qps | 1.24x | 1.79x | 0.98x | 1.49x |
| board | 2 | 208.1 µs / 3,810 qps | 206.9 µs / 4,548 qps | 72.4 µs / 6,944 qps | 1.19x | 1.82x | 0.98x | 1.55x |
| board | 3 | 207.7 µs / 4,165 qps | 207.6 µs / 4,682 qps | 70.7 µs / 8,562 qps | 1.12x | 2.06x | 1.00x | 1.81x |
| tape200 | 1 | 50.5 µs / 19,048 qps | 50.3 µs / 20,161 qps | 48.7 µs / 20,747 qps | 1.06x | 1.09x | 0.94x | 1.00x |
| tape200 | 2 | 50.3 µs / 16,835 qps | 48.7 µs / 17,699 qps | 43.7 µs / 20,964 qps | 1.05x | 1.25x | 1.04x | 1.21x |
| tape200 | 3 | 50.7 µs / 19,646 qps | 49.8 µs / 17,699 qps | 44.0 µs / 21,231 qps | 0.90x | 1.08x | 1.02x | 1.01x |
| tape1k | 1 | 61.0 µs / 16,313 qps | 61.1 µs / 16,863 qps | 48.9 µs / 19,231 qps | 1.03x | 1.18x | 0.97x | 1.05x |
| tape1k | 2 | 59.5 µs / 17,331 qps | 55.9 µs / 17,606 qps | 46.9 µs / 21,322 qps | 1.02x | 1.23x | 1.05x | 1.24x |
| tape1k | 3 | 60.9 µs / 17,331 qps | 59.6 µs / 17,182 qps | 43.9 µs / 21,930 qps | 0.99x | 1.27x | 0.97x | 1.08x |
| tape10k | 1 | 181.5 µs / 5,476 qps | 182.8 µs / 5,685 qps | 49.1 µs / 9,363 qps | 1.04x | 1.71x | 0.95x | 1.49x |
| tape10k | 2 | 183.8 µs / 4,968 qps | 183.6 µs / 5,081 qps | 48.5 µs / 11,001 qps | 1.02x | 2.21x | 1.02x | 1.96x |
| tape10k | 3 | 182.1 µs / 5,659 qps | 181.8 µs / 5,516 qps | 47.6 µs / 10,695 qps | 0.97x | 1.89x | 0.98x | 1.64x |
| pk | 1 | 36.6 µs / 25,445 qps | 36.0 µs / 28,090 qps | 35.8 µs / 28,409 qps | 1.10x | 1.12x | 0.89x | 0.97x |
| pk | 2 | 36.3 µs / 20,408 qps | 35.0 µs / 20,877 qps | 34.4 µs / 27,248 qps | 1.02x | 1.34x | 1.02x | 1.11x |
| pk | 3 | 36.3 µs / 26,455 qps | 35.9 µs / 22,883 qps | 31.6 µs / 30,581 qps | 0.86x | 1.16x | 0.91x | 0.92x |
| open | 1 | 181.5 µs / 3,874 qps | 174.7 µs / 3,664 qps | 165.6 µs / 4,077 qps | 0.95x | 1.05x | 0.97x | 0.95x |
| open | 2 | 169.9 µs / 5,456 qps | 162.6 µs / 5,583 qps | 170.5 µs / 5,643 qps | 1.02x | 1.03x | 1.05x | 0.99x |
| open | 3 | 158.4 µs / 5,568 qps | 156.3 µs / 6,242 qps | 172.3 µs / 5,249 qps | 1.12x | 0.94x | 0.98x | 0.99x |
| close | 1 | 625.4 µs / 929 qps | 518.1 µs / 1,206 qps | 484.5 µs / 1,239 qps | 1.30x | 1.33x | 1.02x | 1.01x |
| close | 2 | 515.2 µs / 952 qps | 485.1 µs / 1,290 qps | 493.1 µs / 1,237 qps | 1.36x | 1.30x | 0.95x | 0.91x |
| close | 3 | 467.5 µs / 1,293 qps | 441.5 µs / 1,397 qps | 508.0 µs / 1,244 qps | 1.08x | 0.96x | 0.93x | 0.96x |

## 4. Server CPU

Run A (`s4`):

| arm | server CPU on the boards, 3 days | on the tapes | v3.5.0 run boards (other host) | v3.5.0 run tapes (other host) |
|---|---|---|---|---|
| off | 1.2 s | 0.2 s | 2.0 s | 0.4 s |
| on | 1.1 s | 0.2 s | 2.0 s | 0.4 s |
| declared | 0.5 s | 0.1 s | 1.0 s | 0.2 s |

Run A again (`r2-s4`):

| arm | server CPU on the boards, 3 days | on the tapes | v3.5.0 run boards (other host) | v3.5.0 run tapes (other host) |
|---|---|---|---|---|
| off | 1.3 s | 0.2 s | 2.0 s | 0.4 s |
| on | 1.1 s | 0.3 s | 2.0 s | 0.4 s |
| declared | 0.6 s | 0.1 s | 1.0 s | 0.2 s |

Run B:

| arm | server CPU on the boards, 3 days | on the tapes | v3.5.0 run boards (other host) | v3.5.0 run tapes (other host) |
|---|---|---|---|---|
| off | 1.3 s | 0.2 s | 2.0 s | 0.4 s |
| on | 1.1 s | 0.2 s | 2.0 s | 0.4 s |
| declared | 0.5 s | 0.1 s | 1.1 s | 0.3 s |

## 5. The controller's lifecycle

- Run A, final snapshot of the `on` arm: elapsed 273.1 s, client_errors 0, slot_overruns 0, first_error None; byte comparison: day 1 18 statements 0 mismatches, day 2 18 statements 0 mismatches, day 3 18 statements 0 mismatches; controller final (on): {"notes_busy": "0", "notes_full": "0", "managed": "5", "pages_committed": "5", "page_budget": "1024", "ticks": "541", "creates": "5", "extends": "0", "heals": "0", "drops": "0", "drops_restricted": "0", "deferred": "0", "failures": "0"}; entries: board_a_a.symbol ACTIVE pages 1 last CREATE; board_b_a.symbol DECAYING pages 1 last CREATE; tape_200_a.symbol ACTIVE pages 1 last CREATE; tape_1k_a.symbol ACTIVE pages 1 last CREATE; tape_10k_a.symbol ACTIVE pages 1 last CREATE
- Run A again: elapsed 273.1 s, client_errors 0, slot_overruns 0, first_error None; byte comparison: day 1 18 statements 0 mismatches, day 2 18 statements 0 mismatches, day 3 18 statements 0 mismatches; controller final (on): {"notes_busy": "0", "notes_full": "0", "managed": "5", "pages_committed": "5", "page_budget": "1024", "ticks": "541", "creates": "5", "extends": "0", "heals": "0", "drops": "0", "drops_restricted": "0", "deferred": "0", "failures": "0"}; entries: board_a_a.symbol ACTIVE pages 1 last CREATE; board_b_a.symbol DECAYING pages 1 last CREATE; tape_200_a.symbol ACTIVE pages 1 last CREATE; tape_1k_a.symbol ACTIVE pages 1 last CREATE; tape_10k_a.symbol ACTIVE pages 1 last CREATE
- Run B: elapsed 272.9 s, client_errors 0, slot_overruns 0, first_error None; byte comparison: day 1 18 statements 0 mismatches, day 2 18 statements 0 mismatches, day 3 18 statements 0 mismatches; controller final (on): {"notes_busy": "0", "notes_full": "0", "managed": "4", "pages_committed": "4", "page_budget": "1024", "ticks": "542", "creates": "6", "extends": "0", "heals": "0", "drops": "2", "drops_restricted": "0", "deferred": "0", "failures": "0"}; entries: board_a_a.symbol ACTIVE pages 1 last CREATE; tape_200_a.symbol ACTIVE pages 1 last CREATE; tape_1k_a.symbol ACTIVE pages 1 last CREATE; tape_10k_a.symbol ACTIVE pages 1 last CREATE

In all 27 evidence snapshots of each run, every managed Cabin holds one page, `extends` is 0, and `hint_fail_pct` and `coverage_miss_pct` are NULL on every entry (a NULL share is a zero lookup count). This is `cabin-controller.md` PO11 unchanged since `v3.5.0`: the controller creates a Cabin on heat, no statement's step probes it without an administrator's plan naming it, and this driver writes no `CREATE PLAN`.

## 6. The `07f4552a` binary on this host

One run of the driver at the defaults on the `07f4552a` binary (`ab-old1-s4`). The last two columns are the `v3.5.0` run's own run A (`s4`), the same binary and settings on the other host:

| group | day | off p50 / qps | on p50 / qps | declared p50 / qps | on over off | declared over off | on over off, v3.5.0 run | declared over off, v3.5.0 run |
|---|---|---|---|---|---|---|---|---|
| board | 1 | 218.3 µs / 3,954 qps | 202.0 µs / 4,744 qps | 71.8 µs / 7,294 qps | 1.20x | 1.84x | 0.99x | 1.46x |
| board | 2 | 205.8 µs / 3,897 qps | 203.8 µs / 4,753 qps | 74.1 µs / 7,174 qps | 1.22x | 1.84x | 1.00x | 1.52x |
| board | 3 | 210.8 µs / 4,132 qps | 209.3 µs / 4,634 qps | 73.7 µs / 8,726 qps | 1.12x | 2.11x | 1.00x | 1.84x |
| tape200 | 1 | 50.5 µs / 17,699 qps | 49.0 µs / 19,569 qps | 47.7 µs / 18,904 qps | 1.11x | 1.07x | 0.92x | 0.99x |
| tape200 | 2 | 50.8 µs / 18,797 qps | 50.3 µs / 17,921 qps | 48.5 µs / 21,186 qps | 0.95x | 1.13x | 1.07x | 1.11x |
| tape200 | 3 | 50.6 µs / 20,121 qps | 50.6 µs / 19,531 qps | 48.5 µs / 21,142 qps | 0.97x | 1.05x | 0.99x | 0.97x |
| tape1k | 1 | 62.1 µs / 16,529 qps | 59.4 µs / 17,452 qps | 48.2 µs / 20,080 qps | 1.06x | 1.21x | 0.91x | 1.06x |
| tape1k | 2 | 61.4 µs / 16,667 qps | 60.6 µs / 16,807 qps | 48.6 µs / 20,492 qps | 1.01x | 1.23x | 1.11x | 1.17x |
| tape1k | 3 | 61.4 µs / 17,036 qps | 62.0 µs / 16,155 qps | 48.5 µs / 20,661 qps | 0.95x | 1.21x | 0.98x | 1.09x |
| tape10k | 1 | 194.7 µs / 5,165 qps | 182.0 µs / 5,456 qps | 48.6 µs / 9,242 qps | 1.06x | 1.79x | 1.00x | 1.53x |
| tape10k | 2 | 189.9 µs / 5,316 qps | 181.2 µs / 5,192 qps | 48.8 µs / 11,507 qps | 0.98x | 2.16x | 1.01x | 1.92x |
| tape10k | 3 | 184.3 µs / 5,647 qps | 186.8 µs / 5,450 qps | 49.4 µs / 9,443 qps | 0.97x | 1.67x | 1.00x | 1.69x |
| pk | 1 | 36.1 µs / 27,248 qps | 35.4 µs / 26,455 qps | 34.4 µs / 30,030 qps | 0.97x | 1.10x | 0.96x | 1.02x |
| pk | 2 | 37.5 µs / 24,570 qps | 36.2 µs / 23,474 qps | 35.8 µs / 28,902 qps | 0.96x | 1.18x | 1.04x | 1.12x |
| pk | 3 | 36.6 µs / 27,248 qps | 36.9 µs / 26,455 qps | 36.0 µs / 25,575 qps | 0.97x | 0.94x | 1.08x | 1.01x |
| open | 1 | 147.7 µs / 6,443 qps | 168.4 µs / 5,225 qps | 159.4 µs / 5,741 qps | 0.81x | 0.89x | 0.99x | 1.04x |
| open | 2 | 160.1 µs / 5,834 qps | 160.8 µs / 5,931 qps | 162.0 µs / 5,963 qps | 1.02x | 1.02x | 1.05x | 0.96x |
| open | 3 | 171.7 µs / 4,606 qps | 171.2 µs / 5,322 qps | 162.8 µs / 5,907 qps | 1.16x | 1.28x | 1.05x | 1.05x |
| close | 1 | 494.8 µs / 1,165 qps | 515.9 µs / 1,214 qps | 512.4 µs / 890 qps | 1.04x | 0.76x | 1.00x | 0.97x |
| close | 2 | 569.1 µs / 958 qps | 402.4 µs / 1,016 qps | 473.1 µs / 1,293 qps | 1.06x | 1.35x | 1.02x | 1.03x |
| close | 3 | 716.5 µs / 910 qps | 559.0 µs / 1,150 qps | 527.4 µs / 1,155 qps | 1.26x | 1.27x | 0.97x | 0.96x |

| arm | server CPU on the boards, 3 days | on the tapes | v3.5.0 run boards (other host) | v3.5.0 run tapes (other host) |
|---|---|---|---|---|
| off | 1.2 s | 0.2 s | 2.0 s | 0.4 s |
| on | 1.1 s | 0.2 s | 2.0 s | 0.4 s |
| declared | 0.5 s | 0.1 s | 1.0 s | 0.2 s |

Its final snapshot: elapsed 272.5 s, client_errors 0, slot_overruns 0, first_error None; byte comparison: day 1 18 statements 0 mismatches, day 2 18 statements 0 mismatches, day 3 18 statements 0 mismatches; controller final (on): {"managed": "5", "pages_committed": "5", "page_budget": "1024", "ticks": "541", "creates": "5", "extends": "0", "heals": "0", "drops": "0", "drops_restricted": "0", "deferred": "0", "failures": "0"}; entries: board_a_a.symbol ACTIVE pages 1 last CREATE; board_b_a.symbol DECAYING pages 1 last CREATE; tape_200_a.symbol ACTIVE pages 1 last CREATE; tape_1k_a.symbol ACTIVE pages 1 last CREATE; tape_10k_a.symbol ACTIVE pages 1 last CREATE

New over old, mean latency per group, arm and day (old mean / new mean, above 1 is the new engine faster), run A (`s4`) against the control:

| group | arm | day 1 old / new mean | new over old | day 2 old / new mean | new over old | day 3 old / new mean | new over old |
|---|---|---|---|---|---|---|---|
| board | off | 252.9 µs / 230.7 µs | 1.10x | 256.6 µs / 244.6 µs | 1.05x | 242.0 µs / 254.9 µs | 0.95x |
| board | on | 210.8 µs / 206.5 µs | 1.02x | 210.4 µs / 209.9 µs | 1.00x | 215.8 µs / 219.7 µs | 0.98x |
| board | declared | 137.1 µs / 130.4 µs | 1.05x | 139.4 µs / 130.5 µs | 1.07x | 114.6 µs / 108.3 µs | 1.06x |
| tape200 | off | 56.5 µs / 49.9 µs | 1.13x | 53.2 µs / 50.5 µs | 1.05x | 49.7 µs / 49.5 µs | 1.00x |
| tape200 | on | 51.1 µs / 50.7 µs | 1.01x | 55.8 µs / 52.0 µs | 1.07x | 51.2 µs / 51.6 µs | 0.99x |
| tape200 | declared | 52.9 µs / 46.4 µs | 1.14x | 47.2 µs / 50.3 µs | 0.94x | 47.3 µs / 45.2 µs | 1.05x |
| tape1k | off | 60.5 µs / 58.3 µs | 1.04x | 60.0 µs / 60.5 µs | 0.99x | 58.7 µs / 57.4 µs | 1.02x |
| tape1k | on | 57.3 µs / 60.5 µs | 0.95x | 59.5 µs / 62.5 µs | 0.95x | 61.9 µs / 57.3 µs | 1.08x |
| tape1k | declared | 49.8 µs / 50.0 µs | 1.00x | 48.8 µs / 52.5 µs | 0.93x | 48.4 µs / 45.7 µs | 1.06x |
| tape10k | off | 193.6 µs / 177.7 µs | 1.09x | 188.1 µs / 184.3 µs | 1.02x | 177.1 µs / 190.2 µs | 0.93x |
| tape10k | on | 183.3 µs / 180.7 µs | 1.01x | 192.6 µs / 184.3 µs | 1.05x | 183.5 µs / 187.0 µs | 0.98x |
| tape10k | declared | 108.2 µs / 98.9 µs | 1.09x | 86.9 µs / 93.1 µs | 0.93x | 105.9 µs / 94.3 µs | 1.12x |
| pk | off | 36.7 µs / 39.5 µs | 0.93x | 40.7 µs / 36.9 µs | 1.10x | 36.7 µs / 37.9 µs | 0.97x |
| pk | on | 37.8 µs / 38.0 µs | 0.99x | 42.6 µs / 38.2 µs | 1.12x | 37.8 µs / 41.5 µs | 0.91x |
| pk | declared | 33.3 µs / 33.8 µs | 0.99x | 34.6 µs / 37.4 µs | 0.93x | 39.1 µs / 33.9 µs | 1.15x |
| open | off | 155.2 µs / 157.6 µs | 0.98x | 171.4 µs / 230.0 µs | 0.75x | 217.1 µs / 195.3 µs | 1.11x |
| open | on | 191.4 µs / 154.3 µs | 1.24x | 168.6 µs / 232.3 µs | 0.73x | 187.9 µs / 817.2 µs | 0.23x |
| open | declared | 174.2 µs / 164.3 µs | 1.06x | 167.7 µs / 210.0 µs | 0.80x | 169.3 µs / 193.9 µs | 0.87x |
| close | off | 858.5 µs / 1,210.6 µs | 0.71x | 1,043.3 µs / 1,055.9 µs | 0.99x | 1,098.5 µs / 1,033.5 µs | 1.06x |
| close | on | 824.0 µs / 858.8 µs | 0.96x | 984.5 µs / 711.5 µs | 1.38x | 869.2 µs / 791.1 µs | 1.10x |
| close | declared | 1,123.9 µs / 1,157.0 µs | 0.97x | 773.2 µs / 733.0 µs | 1.05x | 865.5 µs / 780.8 µs | 1.11x |

Run A again (`r2-s4`) against the control:

| group | arm | day 1 old / new mean | new over old | day 2 old / new mean | new over old | day 3 old / new mean | new over old |
|---|---|---|---|---|---|---|---|
| board | off | 252.9 µs / 248.1 µs | 1.02x | 256.6 µs / 255.5 µs | 1.00x | 242.0 µs / 266.8 µs | 0.91x |
| board | on | 210.8 µs / 204.8 µs | 1.03x | 210.4 µs / 236.6 µs | 0.89x | 215.8 µs / 214.2 µs | 1.01x |
| board | declared | 137.1 µs / 146.7 µs | 0.93x | 139.4 µs / 154.4 µs | 0.90x | 114.6 µs / 117.3 µs | 0.98x |
| tape200 | off | 56.5 µs / 52.4 µs | 1.08x | 53.2 µs / 50.4 µs | 1.06x | 49.7 µs / 54.4 µs | 0.91x |
| tape200 | on | 51.1 µs / 51.1 µs | 1.00x | 55.8 µs / 67.9 µs | 0.82x | 51.2 µs / 50.5 µs | 1.01x |
| tape200 | declared | 52.9 µs / 47.9 µs | 1.10x | 47.2 µs / 42.9 µs | 1.10x | 47.3 µs / 47.6 µs | 0.99x |
| tape1k | off | 60.5 µs / 58.3 µs | 1.04x | 60.0 µs / 49.2 µs | 1.22x | 58.7 µs / 62.1 µs | 0.95x |
| tape1k | on | 57.3 µs / 60.1 µs | 0.95x | 59.5 µs / 53.2 µs | 1.12x | 61.9 µs / 60.8 µs | 1.02x |
| tape1k | declared | 49.8 µs / 51.6 µs | 0.97x | 48.8 µs / 40.9 µs | 1.19x | 48.4 µs / 48.1 µs | 1.01x |
| tape10k | off | 193.6 µs / 178.2 µs | 1.09x | 188.1 µs / 185.1 µs | 1.02x | 177.1 µs / 202.0 µs | 0.88x |
| tape10k | on | 183.3 µs / 182.3 µs | 1.01x | 192.6 µs / 202.7 µs | 0.95x | 183.5 µs / 191.2 µs | 0.96x |
| tape10k | declared | 108.2 µs / 107.3 µs | 1.01x | 86.9 µs / 87.2 µs | 1.00x | 105.9 µs / 98.7 µs | 1.07x |
| pk | off | 36.7 µs / 40.4 µs | 0.91x | 40.7 µs / 41.9 µs | 0.97x | 36.7 µs / 45.2 µs | 0.81x |
| pk | on | 37.8 µs / 37.5 µs | 1.01x | 42.6 µs / 48.0 µs | 0.89x | 37.8 µs / 38.0 µs | 0.99x |
| pk | declared | 33.3 µs / 34.8 µs | 0.96x | 34.6 µs / 30.2 µs | 1.15x | 39.1 µs / 39.4 µs | 0.99x |
| open | off | 155.2 µs / 157.3 µs | 0.99x | 171.4 µs / 202.7 µs | 0.85x | 217.1 µs / 169.3 µs | 1.28x |
| open | on | 191.4 µs / 167.2 µs | 1.14x | 168.6 µs / 189.4 µs | 0.89x | 187.9 µs / 161.7 µs | 1.16x |
| open | declared | 174.2 µs / 166.3 µs | 1.05x | 167.7 µs / 198.0 µs | 0.85x | 169.3 µs / 178.3 µs | 0.95x |
| close | off | 858.5 µs / 1,111.4 µs | 0.77x | 1,043.3 µs / 1,197.7 µs | 0.87x | 1,098.5 µs / 1,101.6 µs | 1.00x |
| close | on | 824.0 µs / 956.9 µs | 0.86x | 984.5 µs / 860.8 µs | 1.14x | 869.2 µs / 793.0 µs | 1.10x |
| close | declared | 1,123.9 µs / 747.4 µs | 1.50x | 773.2 µs / 834.0 µs | 0.93x | 865.5 µs / 1,019.5 µs | 0.85x |

Each cell is one run against one run, so no cell carries a claim by this directory's spread rule.

## 7. Correctness

The 18-statement byte comparison across the three arms reported 0 mismatches on every day of every run, the `07f4552a`-binary run included (section 5 and 6's lines). No client error and no slot overrun in any run.

## 8. What the run says about the engine

- **PO11 holds at `9f8f8b354` exactly as at `07f4552a`**: `creates` 5 (run A, both times, and the old binary) or 6 with 2 drops (run B), `extends` 0, one page a Cabin, both lookup shares NULL in all 27 snapshots of every run. To measure the controller the driver would need a `CREATE PLAN ... ACCESS <relation> CABIN (<column>)` per probed shape, which is not built.
- **A declared Cabin serves**, at `board` 1.65x to 2.35x and at `tape10k` 1.66x to 2.21x over the walk across the measured binary's three runs (the `07f4552a` binary on this host: 1.84x to 2.11x and 1.67x to 2.16x). Server CPU on the boards is 1.2 s to 1.3 s for `off`, 1.1 s for `on` and 0.5 s to 0.6 s for `declared` over three days in each run, the same on the old binary (1.2 s, 1.1 s, 0.5 s).
- **No engine reading beyond that.** One run against one run: the probe groups scatter 0.81x to 1.22x new over old with no pattern by arm or day, and the `pk` control's own `on over off` runs 0.86x to 1.19x.
- **`on over off` above 1 on `board` is not the controller.** It appears on both binaries, with no lookup recorded on either, and the `07f4552a` binary's own run A on the `v3.5.0` run's host read 0.99x, 1.00x and 1.00x (section 6's last columns): the ratio follows the host. What on this host produces it is unverified; the check is the per-block latency of each arm by its position in the driver's interleave.

## 9. Noise floor, contamination and host

- **Run A (`s4`) overlapped another session's full build**: 43 of its 123 monitor samples carry foreign processes at 50 % or more of one CPU, on average 8.3 CPUs' worth (`cc1plus`, `as`, `ld`, `cmake`, then `kds_tests`). The driver is paced, so the run completed with no slot overrun, and its numbers are kept and disclosed; the run was made again as `r2-s4` (22:07:46 to 22:12:20 UTC), which carries no such sample and reads the same way (`on over off` on `board` 1.21x, 1.08x, 1.25x against 1.12x, 1.17x, 1.16x; the controller's final state the same: five Cabins of one page, `extends` 0).
- Run B (`cd2-s4`) carries 19 of 122 samples of another session's `kds_tests` at about one CPU. The `07f4552a`-binary control carries none.
- **On this host `on over off` is not level on the boards on any binary** (1.08x to 1.25x in means over the four runs, the `07f4552a` control among them), while the two arms' p50 are within 5 % of each other in the measured binary's three runs (the control's day 1: 218.3 µs against 202.0 µs). The difference is in the `off` arm's tail; the controller's Cabins record no lookup in either binary (section 5), so the ratio is not a Cabin serving. It is not read as a finding. The `pk` control's own `on over off` runs 0.86x to 1.19x over the four runs, so the floor on this host is about 20 % either way; the board's ratio is inside or just above it, but above 1 on all twelve days.
- Gate waits: 0 s at every scenario 4 run.

## 10. Archive

`bench/v3.5.0/archive/scenario4-v3.4.17-159-g9f8f8b354/` (per-run JSON with every evidence snapshot, driver stdout, prechecks, gate waits, start and end times, the three servers' configs and logs per run) and `bench/v3.5.0/archive/run-v3.4.17-159-g9f8f8b354/`. No data file is kept.
