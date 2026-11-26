# Scenario 2 (freight) at `v3.4.17-159-g9f8f8b354`

**One line.** On the booking path the engine at `9f8f8b354` is the engine of `07f4552a` (`v3.5.0`): in the pinned same-host A/B (five interleaved pairs) one booker at one core, `strict`, reads 1,749.7 tps against 1,749.5 tps (1.00x, spreads 1.1 % and 0.9 %) with every statement of the transaction at 1.00x to 1.02x. Eight bookers read 1.01x at one core and 1.03x at eight, neither a claim, because each has one stalled run in its ten (526.1 tps in the old arm at one core, 4,149.1 tps in the new arm at eight; no foreign CPU in the monitor). At eight cores the three write statements read 1.05x each, `org-update` marked by the spread rule (+4.6 % against 4.2 %). The matrix reads 2.1x to 3.2x the `v3.5.0` run's, which is the host. The invariant failures under more than one booker are the driver's client-side lost update, as before: 0 of 400 with one booker and 0 of 400 under repeatable-read. One booking in this run's 80 scenario 2 cells was abandoned at `--max-retries 5` (1 in all).

Driver: `tools/scenario2_freight.py` of the measured commit, unmodified (byte-identical to `07f4552a`'s), `--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync`, schema, load and run as three invocations on one server, as the `v3.5.0` run. Comparator run: `results-scenario2-freight-v3.5.0.md` (2026-11-20, `07f4552a`, **another host**), so a ratio against it is host plus engine. The engine reading is the interleaved A/B on this host (sections 7 and 8).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-26; main cells 21:09:39 to 21:11:56 UTC; unpinned A/B cells inside 21:19:44 to 21:41:59 UTC; extras (repeatable-read, 16 bookers) 21:46:35 to 21:46:54 UTC; pinned A/B cells inside 21:52:11 to 22:07:44 UTC |
| Version, commit, binary, host, device, budget | stamped once in `summary-scenarios-v3.4.17-159-g9f8f8b354.md`, section Stamp |
| Server config | the common config; `cores` and `durability` per cell; ports 15704 to 15707 |
| Driver | exit 0 in every cell, and in every `--schema-only` and `--load-only` invocation |

## 2. What was run

The `v3.5.0` run's cells, unchanged in arguments, suffixes and order: four matrix cells (`cores` 1 or 8, `group` or `strict`) and two single-booker cells, three times each; the booker sweep (1, 2, 4 and 16 bookers at eight cores, once each); the 32-booker saturation cells (once each, with a 1 s host CPU sampler); repeatable-read at one and eight cores and 16 bookers again (extras, once each); and the A/B (8 bookers `c1-g` and 1 booker `c1-s`, three interleaved pairs). **Added**: an eight-core A/B cell (8 bookers `c8-g`), and the whole A/B again with the server pinned to the P-cores (five pairs). **Cut**, as at `v3.5.0`: the extreme cells (20,000 bookings, 32 bookers, one and two client processes) and PostgreSQL (no server on this host).

**A cell here is short.** 3,000 bookings take 0.4 s at eight cores and 0.9 s at one on this host (they took 1.1 s and 2.6 s on the `v3.5.0` run's), so one cell is a sample of under a second and its run-to-run spread is wide.

## 3. Throughput

**The last column is this host over another host**: no cell of it is a finding about the engine.

| cell | cores | durability | run 1 | run 2 | run 3 | median | spread | v3.5.0 run median (other host) | spread | now over v3.5.0 run (host + engine) |
|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | group | 3,103.5 tps | 3,174.5 tps | 3,205.9 tps | **3,174.5 tps** | 3.2 % | 1,172.7 tps | 4.3 % | 2.71x |
| `s2-c8-g` | 8 | group | 7,354.7 tps | 8,111.5 tps | 8,193.6 tps | **8,111.5 tps** | 10.3 % | 2,612.6 tps | 4.8 % | 3.10x |
| `s2-c1-s` | 1 | strict | 3,089.2 tps | 3,177.1 tps | 1,922.1 tps | **3,089.2 tps** | 40.6 % | 1,108.6 tps | 3.7 % | 2.79x |
| `s2-c8-s` | 8 | strict | 8,175.0 tps | 4,031.4 tps | 7,521.4 tps | **7,521.4 tps** | 55.1 % | 2,384.5 tps | 5.1 % | 3.15x |
| `b1-s2-c1-g` | 1 | group | 1,820.8 tps | 1,097.6 tps | 1,270.7 tps | **1,270.7 tps** | 56.9 % | 615.9 tps | 3.8 % | 2.06x |
| `b1-s2-c1-s` | 1 | strict | 1,258.4 tps | 1,631.2 tps | 1,333.0 tps | **1,333.0 tps** | 28.0 % | 631.3 tps | 2.1 % | 2.11x |

## 4. The booker sweep and saturation

Eight bookers are the matrix cells (three runs); every other row is one run.

| bookers | durability | tps now | runs | spread | v3.5.0 run (other host) | now over v3.5.0 run (host + engine) | conflicted (run 1) | verify failures / checks (run 1) |
|---|---|---|---|---|---|---|---|---|
| 1 | group | **1,351.5 tps** | 1 | n/a | 568.2 tps | 2.38x | 0 | 0 / 400 |
| 1 | strict | **1,615.2 tps** | 1 | n/a | 572.1 tps | 2.82x | 0 | 0 / 400 |
| 2 | group | **2,589.0 tps** | 1 | n/a | 1,058.2 tps | 2.45x | 0 | 1 / 400 |
| 2 | strict | **2,992.5 tps** | 1 | n/a | 914.5 tps | 3.27x | 1 | 9 / 400 |
| 4 | group | **4,719.5 tps** | 1 | n/a | 1,718.1 tps | 2.75x | 0 | 13 / 400 |
| 4 | strict | **5,395.0 tps** | 1 | n/a | 1,532.1 tps | 3.52x | 22 | 11 / 400 |
| 8 | group | **8,111.5 tps** | 3 | 10.3 % | 2,612.6 tps | 3.10x | 0 | 23 / 400 |
| 8 | strict | **7,521.4 tps** | 3 | 55.1 % | 2,384.5 tps | 3.15x | 43 | 15 / 400 |
| 16 | group | **8,402.1 tps** | 1 | n/a | 3,522.9 tps | 2.39x | 0 | 57 / 400 |
| 16 | strict | **7,331.6 tps** | 1 | n/a | 3,286.8 tps | 2.23x | 60 | 40 / 400 |
| 32 | group | **10,475.1 tps** | 1 | n/a | 3,735.5 tps | 2.80x | 1 | 75 / 400 |
| 32 | strict | **10,468.4 tps** | 1 | n/a | 3,570.9 tps | 2.93x | 115 | 66 / 400 |

The extras:

| cell | tps now | conflicted | retries | verify failures / checks | v3.5.0 run tps (other host) | its conflicted | its verify failures / checks |
|---|---|---|---|---|---|---|---|
| `rr-s2-c1-g` | 3,296.8 tps | 106 | 106 | 0 / 400 | 1,110.2 tps | 108 | 0 / 400 |
| `rr-s2-c8-g` | 8,223.0 tps | 103 | 103 | 0 / 400 | 2,634.0 tps | 95 | 0 / 400 |
| `sat-b16-s2-c8-g` | 5,282.5 tps | 1 | 1 | 43 / 400 | 3,503.8 tps | 1 | 61 / 400 |
| `sat-b16-s2-c8-s` | 11,158.8 tps | 100 | 100 | 30 / 400 | 3,236.5 tps | 103 | 29 / 400 |

## 5. Where the time goes

Mean booking latency and the commit's share of it, run 1:

| cell | booking mean now | commit share now | booking mean v3.5.0 run (other host) | commit share v3.5.0 run |
|---|---|---|---|---|
| `s2-c1-g` | 1,891 µs | 12.6 % | 6,038 µs | 13.6 % |
| `s2-c8-g` | 674 µs | 28.7 % | 2,662 µs | 28.0 % |
| `s2-c1-s` | 2,019 µs | 10.6 % | 6,447 µs | 11.3 % |
| `s2-c8-s` | 712 µs | 27.2 % | 3,028 µs | 26.3 % |
| `b1-s2-c1-g` | 467 µs | 28.3 % | 1,379 µs | 35.2 % |
| `b1-s2-c1-s` | 686 µs | 24.0 % | 1,391 µs | 34.3 % |

Every statement of the booking transaction, run 1 of `s2-c8-g` (the share is of the summed statement time, the reporter's three reads included):

| phase | ops | mean | p50 | p99 | share of the summed statement time |
|---|---|---|---|---|---|
| booking | 3,355 | 673.6 µs | 594.5 µs | 1,795.3 µs | - |
| cargo-lookup | 3,355 | 41.9 µs | 37.1 µs | 100.5 µs | 6.7 % |
| credit-lookup | 3,355 | 38.8 µs | 30.5 µs | 137.2 µs | 6.2 % |
| capacity-read | 3,355 | 39.5 µs | 29.5 µs | 143.7 µs | 6.3 % |
| recipe-read | 3,355 | 80.5 µs | 67.0 µs | 268.5 µs | 12.9 % |
| freight-insert | 3,000 | 48.2 µs | 26.9 µs | 354.8 µs | 6.9 % |
| charge-insert | 16,916 | 26.4 µs | 20.6 µs | 105.0 µs | 21.3 % |
| operation-update | 3,000 | 31.2 µs | 24.1 µs | 119.5 µs | 4.5 % |
| org-update | 3,000 | 29.3 µs | 22.0 µs | 105.0 µs | 4.2 % |
| commit | 3,000 | 216.4 µs | 196.4 µs | 497.2 µs | 30.9 % |
| manifest-scan | 20 | 72.9 µs | 42.2 µs | 614.9 µs | 0.1 % |
| voyage-rollup | 20 | 55.5 µs | 38.3 µs | 356.1 µs | 0.1 % |
| customer-statement | 10 | 52.5 µs | 43.8 µs | 117.4 µs | 0.0 % |

Run 1 of `s2-c1-g`:

| phase | ops | mean | p50 | p99 | share of the summed statement time |
|---|---|---|---|---|---|
| booking | 3,326 | 1,891.0 µs | 1,572.7 µs | 5,029.7 µs | - |
| cargo-lookup | 3,326 | 139.1 µs | 90.1 µs | 470.4 µs | 8.1 % |
| credit-lookup | 3,326 | 131.2 µs | 78.9 µs | 452.0 µs | 7.6 % |
| capacity-read | 3,326 | 132.1 µs | 79.4 µs | 470.3 µs | 7.7 % |
| recipe-read | 3,326 | 169.7 µs | 116.1 µs | 503.0 µs | 9.9 % |
| freight-insert | 3,000 | 140.7 µs | 122.8 µs | 455.2 µs | 7.4 % |
| charge-insert | 16,912 | 114.3 µs | 60.9 µs | 454.3 µs | 33.8 % |
| operation-update | 3,000 | 113.2 µs | 58.1 µs | 452.4 µs | 5.9 % |
| org-update | 3,000 | 108.2 µs | 56.1 µs | 444.4 µs | 5.7 % |
| commit | 3,000 | 263.6 µs | 214.0 µs | 750.9 µs | 13.8 % |
| manifest-scan | 20 | 69.5 µs | 40.9 µs | 263.0 µs | 0.0 % |
| voyage-rollup | 20 | 59.9 µs | 40.6 µs | 254.9 µs | 0.0 % |
| customer-statement | 10 | 43.0 µs | 39.9 µs | 61.1 µs | 0.0 % |

Percentiles, run 1 of `s2-c8-g`:

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 8 | 224.8 µs | 320.7 µs | 710.2 µs | 884.9 µs | 884.9 µs | 884.9 µs | 0 |
| load-organizations | 300 | 143.7 µs | 146.9 µs | 148.9 µs | 202.8 µs | 377.1 µs | 1,378.4 µs | 0 |
| load-ships | 30 | 143.2 µs | 145.3 µs | 146.6 µs | 219.1 µs | 1,554.4 µs | 1,554.4 µs | 0 |
| load-operations | 300 | 140.7 µs | 142.8 µs | 144.2 µs | 170.2 µs | 188.6 µs | 220.2 µs | 0 |
| load-fees | 12 | 143.9 µs | 144.3 µs | 145.3 µs | 167.2 µs | 167.2 µs | 167.2 µs | 0 |
| load-recipes | 93 | 140.1 µs | 142.1 µs | 143.4 µs | 157.2 µs | 282.7 µs | 282.7 µs | 0 |
| load-cargos | 4,000 | 132.0 µs | 141.7 µs | 144.0 µs | 177.9 µs | 371.9 µs | 1,915.8 µs | 0 |
| booking | 3,355 | 174.1 µs | 549.6 µs | 594.5 µs | 1,153.6 µs | 1,795.3 µs | 10,259.5 µs | 0 |
| cargo-lookup | 3,355 | 27.9 µs | 35.5 µs | 37.1 µs | 58.4 µs | 100.5 µs | 669.0 µs | 0 |
| credit-lookup | 3,355 | 23.3 µs | 29.2 µs | 30.5 µs | 73.4 µs | 137.2 µs | 1,209.6 µs | 0 |
| capacity-read | 3,355 | 21.9 µs | 27.8 µs | 29.5 µs | 76.8 µs | 143.7 µs | 1,350.0 µs | 0 |
| recipe-read | 3,355 | 54.3 µs | 62.7 µs | 67.0 µs | 132.5 µs | 268.5 µs | 1,697.4 µs | 0 |
| freight-insert | 3,000 | 17.2 µs | 24.9 µs | 26.9 µs | 159.6 µs | 354.8 µs | 1,482.4 µs | 0 |
| charge-insert | 16,916 | 14.3 µs | 17.1 µs | 20.6 µs | 48.5 µs | 105.0 µs | 2,687.4 µs | 0 |
| operation-update | 3,000 | 18.0 µs | 22.4 µs | 24.1 µs | 55.1 µs | 119.5 µs | 1,210.1 µs | 0 |
| org-update | 3,000 | 17.4 µs | 19.6 µs | 22.0 µs | 54.1 µs | 105.0 µs | 1,020.7 µs | 0 |
| commit | 3,000 | 126.0 µs | 168.7 µs | 196.4 µs | 288.9 µs | 497.2 µs | 6,308.5 µs | 0 |
| manifest-scan | 20 | 40.9 µs | 41.8 µs | 42.2 µs | 72.9 µs | 614.9 µs | 614.9 µs | 0 |
| voyage-rollup | 20 | 37.2 µs | 37.7 µs | 38.3 µs | 51.5 µs | 356.1 µs | 356.1 µs | 0 |
| customer-statement | 10 | 42.6 µs | 42.8 µs | 43.8 µs | 117.4 µs | 117.4 µs | 117.4 µs | 0 |

## 6. Correctness: the invariant failures

| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | verify failures / checks |
|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | 3,000 | 282 | 44 | 0 | 0 | 30 / 400 |
| `r2-s2-c1-g` | 2 | 3,000 | 298 | 51 | 0 | 0 | 28 / 400 |
| `r3-s2-c1-g` | 3 | 3,000 | 292 | 55 | 0 | 0 | 20 / 400 |
| `s2-c8-g` | 1 | 3,000 | 294 | 61 | 0 | 0 | 23 / 400 |
| `r2-s2-c8-g` | 2 | 3,000 | 287 | 57 | 0 | 0 | 23 / 400 |
| `r3-s2-c8-g` | 3 | 3,000 | 294 | 52 | 0 | 0 | 25 / 400 |
| `s2-c1-s` | 1 | 3,000 | 294 | 54 | 0 | 0 | 28 / 400 |
| `r2-s2-c1-s` | 2 | 3,000 | 305 | 52 | 0 | 0 | 29 / 400 |
| `r3-s2-c1-s` | 3 | 3,000 | 317 | 48 | 0 | 0 | 33 / 400 |
| `s2-c8-s` | 1 | 3,000 | 290 | 56 | 43 | 0 | 15 / 400 |
| `r2-s2-c8-s` | 2 | 3,000 | 256 | 53 | 55 | 1 | 15 / 400 |
| `r3-s2-c8-s` | 3 | 3,000 | 288 | 60 | 43 | 0 | 23 / 400 |
| `b1-s2-c1-g` | 1 | 3,000 | 328 | 74 | 0 | 0 | 0 / 400 |
| `r2-b1-s2-c1-g` | 2 | 3,000 | 328 | 74 | 0 | 0 | 0 / 400 |
| `r3-b1-s2-c1-g` | 3 | 3,000 | 328 | 74 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | 1 | 3,000 | 328 | 74 | 0 | 0 | 0 / 400 |
| `r2-b1-s2-c1-s` | 2 | 3,000 | 328 | 74 | 0 | 0 | 0 / 400 |
| `r3-b1-s2-c1-s` | 3 | 3,000 | 328 | 74 | 0 | 0 | 0 / 400 |

With one booker every run passes 400 of 400, and under `--isolation repeatable-read` both cells pass 400 of 400 (section 4's extras). Under the default isolation with more bookers the `--verify 100` pass fails as at `v3.5.0` (`I1 operation <n>: booked_cbm=..., SUM(freights.cbm)=...`): the driver writes `SET booked_cbm = <literal>` from a value it read earlier in the transaction, the client-side lost update its source names. 54 of this run's 80 scenario 2 cells report a failure: 53 with `I1` first and one (`x-b2-s2-c8-g`) with `I3 organization <n>: outstanding=..., recomputed from its freights and charges=...`, which is consistent with the same lost update on `org-update`, a statement that also writes a literal (`SET outstanding = <literal>`). Every single-booker cell and both repeatable-read cells report 0. It is not engine corruption. **One booking was abandoned**: `r2-s2-c8-s` (eight cores, `strict`, run 2) lost one booking to `--max-retries 5` after 55 conflicts; no `v3.5.0` cell abandoned one. One event in 28 main cells is not a rate, and across all 80 scenario 2 cells of this run (main, extras and both A/B series, old binary and new) it is the only one (1 in all).

## 7. The engine, isolated, unpinned: `07f4552a` against `9f8f8b354`, interleaved on this host

The `07f4552a` binary built on this host (sha256 `03c8c2e9...`) and the measured binary (sha256 `a333ee48...`), one driver, interleaved old, new, old, new, old, new, each on a fresh server and data file.

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s2 8 bookers, cores 1, group: tps | 3,305.2 tps | 7.2 % | 3,438.8 tps | 13.6 % | 1.04x | inside the spread |
| s2 1 booker, cores 1, strict: tps | 1,157.7 tps | 66.2 % | 1,368.1 tps | 8.9 % | 1.18x | spread > 20 %, no claim |
| s2 8 bookers, cores 8, group: tps | 8,099.4 tps | 51.3 % | 8,597.2 tps | 16.2 % | 1.06x | spread > 20 %, no claim |

Outcomes per run:

| cell | arm | run | tps | committed | conflicted | retries | verify failures / checks |
|---|---|---|---|---|---|---|---|
| `s2-c1-g` | old | 1 | 3,492.2 tps | 3,000 | 0 | 0 | 29 / 400 |
| `s2-c1-g` | old | 2 | 3,253.8 tps | 3,000 | 0 | 0 | 22 / 400 |
| `s2-c1-g` | old | 3 | 3,305.2 tps | 3,000 | 0 | 0 | 27 / 400 |
| `s2-c1-g` | new | 1 | 3,438.8 tps | 3,000 | 0 | 0 | 22 / 400 |
| `s2-c1-g` | new | 2 | 3,042.4 tps | 3,000 | 0 | 0 | 23 / 400 |
| `s2-c1-g` | new | 3 | 3,509.4 tps | 3,000 | 0 | 0 | 28 / 400 |
| `b1-s2-c1-s` | old | 1 | 1,041.9 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | old | 2 | 1,157.7 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | old | 3 | 1,808.6 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | new | 1 | 1,319.6 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | new | 2 | 1,441.1 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | new | 3 | 1,368.1 tps | 3,000 | 0 | 0 | 0 / 400 |
| `s2-c8-g` | old | 1 | 8,099.4 tps | 3,000 | 2 | 2 | 19 / 400 |
| `s2-c8-g` | old | 2 | 8,341.6 tps | 3,000 | 2 | 2 | 18 / 400 |
| `s2-c8-g` | old | 3 | 4,187.7 tps | 3,000 | 1 | 1 | 28 / 400 |
| `s2-c8-g` | new | 1 | 7,207.0 tps | 3,000 | 0 | 0 | 20 / 400 |
| `s2-c8-g` | new | 2 | 8,597.2 tps | 3,000 | 1 | 1 | 25 / 400 |
| `s2-c8-g` | new | 3 | 8,599.6 tps | 3,000 | 1 | 1 | 20 / 400 |

Statements, 8 bookers at one core, `group`:

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| load-organizations (1e6 / mean) | 6,002.4 qps | 2.4 % | 6,596.3 qps | 16.8 % | 1.10x | inside the spread |
| load-ships (1e6 / mean) | 6,180.5 qps | 29.3 % | 5,249.3 qps | 44.2 % | 0.85x | spread > 20 %, no claim |
| load-operations (1e6 / mean) | 6,321.1 qps | 15.2 % | 6,472.5 qps | 6.2 % | 1.02x | inside the spread |
| load-fees (1e6 / mean) | 6,640.1 qps | 17.2 % | 6,281.4 qps | 18.7 % | 0.95x | inside the spread |
| load-recipes (1e6 / mean) | 6,609.4 qps | 13.3 % | 6,261.7 qps | 13.8 % | 0.95x | inside the spread |
| load-cargos (1e6 / mean) | 6,273.5 qps | 1.3 % | 6,389.8 qps | 1.4 % | 1.02x | **finding** |
| booking (1e6 / mean) | 547.9 qps | 14.1 % | 575.1 qps | 13.2 % | 1.05x | inside the spread |
| cargo-lookup (1e6 / mean) | 7,092.2 qps | 17.0 % | 7,564.3 qps | 14.9 % | 1.07x | inside the spread |
| credit-lookup (1e6 / mean) | 7,598.8 qps | 18.5 % | 7,936.5 qps | 17.0 % | 1.04x | inside the spread |
| capacity-read (1e6 / mean) | 7,704.2 qps | 17.1 % | 8,110.3 qps | 16.3 % | 1.05x | inside the spread |
| recipe-read (1e6 / mean) | 5,872.0 qps | 13.9 % | 6,176.7 qps | 10.9 % | 1.05x | inside the spread |
| freight-insert (1e6 / mean) | 7,299.3 qps | 11.5 % | 7,843.1 qps | 13.7 % | 1.07x | inside the spread |
| charge-insert (1e6 / mean) | 9,082.7 qps | 14.0 % | 9,606.1 qps | 13.7 % | 1.06x | inside the spread |
| operation-update (1e6 / mean) | 9,718.2 qps | 8.9 % | 9,920.6 qps | 17.6 % | 1.02x | inside the spread |
| org-update (1e6 / mean) | 9,746.6 qps | 8.8 % | 10,416.7 qps | 15.7 % | 1.07x | inside the spread |
| commit (1e6 / mean) | 3,818.3 qps | 12.1 % | 3,984.1 qps | 14.9 % | 1.04x | inside the spread |
| manifest-scan (1e6 / mean) | 14,641.3 qps | 61.0 % | 14,534.9 qps | 86.0 % | 0.99x | spread > 20 %, no claim |
| voyage-rollup (1e6 / mean) | 13,587.0 qps | 97.5 % | 22,727.3 qps | 77.7 % | 1.67x | spread > 20 %, no claim |
| customer-statement (1e6 / mean) | 12,738.9 qps | 89.0 % | 16,750.4 qps | 66.6 % | 1.31x | spread > 20 %, no claim |

## 8. The engine, isolated, pinned: the same A/B with the server on the P-cores

Server under `taskset -c 0-7`, driver (and its booker processes) under `taskset -c 8-23`, five interleaved pairs, old first.

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s2 8 bookers, cores 1, group: tps | 4,192.8 tps | 88.3 % | 4,228.0 tps | 6.3 % | 1.01x | spread > 20 %, no claim |
| s2 1 booker, cores 1, strict: tps | 1,749.5 tps | 0.9 % | 1,749.7 tps | 1.1 % | 1.00x | inside the spread |
| s2 8 bookers, cores 8, group: tps | 9,543.2 tps | 3.9 % | 9,801.8 tps | 58.4 % | 1.03x | spread > 20 %, no claim |

Outcomes per run:

| cell | arm | run | tps | committed | conflicted | retries | verify failures / checks |
|---|---|---|---|---|---|---|---|
| `s2-c1-g` | old | 1 | 4,227.9 tps | 3,000 | 0 | 0 | 30 / 400 |
| `s2-c1-g` | old | 2 | 4,185.8 tps | 3,000 | 0 | 0 | 26 / 400 |
| `s2-c1-g` | old | 3 | 4,192.8 tps | 3,000 | 0 | 0 | 25 / 400 |
| `s2-c1-g` | old | 4 | 4,223.5 tps | 3,000 | 0 | 0 | 20 / 400 |
| `s2-c1-g` | old | 5 | 526.1 tps | 3,000 | 0 | 0 | 42 / 400 |
| `s2-c1-g` | new | 1 | 4,257.2 tps | 3,000 | 0 | 0 | 19 / 400 |
| `s2-c1-g` | new | 2 | 4,228.0 tps | 3,000 | 0 | 0 | 34 / 400 |
| `s2-c1-g` | new | 3 | 4,239.0 tps | 3,000 | 0 | 0 | 28 / 400 |
| `s2-c1-g` | new | 4 | 3,991.7 tps | 3,000 | 0 | 0 | 22 / 400 |
| `s2-c1-g` | new | 5 | 4,216.0 tps | 3,000 | 0 | 0 | 37 / 400 |
| `b1-s2-c1-s` | old | 1 | 1,742.3 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | old | 2 | 1,755.0 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | old | 3 | 1,757.3 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | old | 4 | 1,749.5 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | old | 5 | 1,746.6 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | new | 1 | 1,748.9 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | new | 2 | 1,760.8 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | new | 3 | 1,742.1 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | new | 4 | 1,752.0 tps | 3,000 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | new | 5 | 1,749.7 tps | 3,000 | 0 | 0 | 0 / 400 |
| `s2-c8-g` | old | 1 | 9,482.3 tps | 3,000 | 1 | 1 | 30 / 400 |
| `s2-c8-g` | old | 2 | 9,288.8 tps | 3,000 | 0 | 0 | 25 / 400 |
| `s2-c8-g` | old | 3 | 9,595.6 tps | 3,000 | 1 | 1 | 19 / 400 |
| `s2-c8-g` | old | 4 | 9,543.2 tps | 3,000 | 0 | 0 | 21 / 400 |
| `s2-c8-g` | old | 5 | 9,661.8 tps | 3,000 | 3 | 3 | 22 / 400 |
| `s2-c8-g` | new | 1 | 9,593.5 tps | 3,000 | 1 | 1 | 23 / 400 |
| `s2-c8-g` | new | 2 | 9,801.8 tps | 3,000 | 0 | 0 | 25 / 400 |
| `s2-c8-g` | new | 3 | 9,873.5 tps | 3,000 | 1 | 1 | 22 / 400 |
| `s2-c8-g` | new | 4 | 9,874.0 tps | 3,000 | 3 | 3 | 22 / 400 |
| `s2-c8-g` | new | 5 | 4,149.1 tps | 3,000 | 0 | 0 | 23 / 400 |

Statements, 8 bookers at one core, `group`:

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| load-organizations (1e6 / mean) | 6,518.9 qps | 57.8 % | 6,514.7 qps | 11.3 % | 1.00x | spread > 20 %, no claim |
| load-ships (1e6 / mean) | 6,812.0 qps | 26.0 % | 6,863.4 qps | 18.3 % | 1.01x | spread > 20 %, no claim |
| load-operations (1e6 / mean) | 7,163.3 qps | 10.7 % | 6,949.3 qps | 10.1 % | 0.97x | inside the spread |
| load-fees (1e6 / mean) | 7,102.3 qps | 30.3 % | 7,052.2 qps | 16.5 % | 0.99x | spread > 20 %, no claim |
| load-recipes (1e6 / mean) | 6,934.8 qps | 10.5 % | 7,027.4 qps | 11.4 % | 1.01x | inside the spread |
| load-cargos (1e6 / mean) | 6,949.3 qps | 15.3 % | 6,901.3 qps | 4.8 % | 0.99x | inside the spread |
| booking (1e6 / mean) | 629.7 qps | 89.3 % | 635.5 qps | 7.7 % | 1.01x | spread > 20 %, no claim |
| cargo-lookup (1e6 / mean) | 8,347.2 qps | 87.1 % | 8,460.2 qps | 8.0 % | 1.01x | spread > 20 %, no claim |
| credit-lookup (1e6 / mean) | 8,818.3 qps | 87.8 % | 8,896.8 qps | 6.4 % | 1.01x | spread > 20 %, no claim |
| capacity-read (1e6 / mean) | 9,058.0 qps | 92.9 % | 9,049.8 qps | 8.7 % | 1.00x | spread > 20 %, no claim |
| recipe-read (1e6 / mean) | 6,561.7 qps | 79.1 % | 6,510.4 qps | 6.8 % | 0.99x | spread > 20 %, no claim |
| freight-insert (1e6 / mean) | 8,143.3 qps | 92.0 % | 8,223.7 qps | 11.2 % | 1.01x | spread > 20 %, no claim |
| charge-insert (1e6 / mean) | 10,593.2 qps | 89.7 % | 10,649.6 qps | 7.8 % | 1.01x | spread > 20 %, no claim |
| operation-update (1e6 / mean) | 11,454.8 qps | 89.6 % | 11,627.9 qps | 4.4 % | 1.02x | spread > 20 %, no claim |
| org-update (1e6 / mean) | 11,709.6 qps | 85.0 % | 11,961.7 qps | 7.7 % | 1.02x | spread > 20 %, no claim |
| commit (1e6 / mean) | 4,330.9 qps | 92.6 % | 4,397.5 qps | 10.1 % | 1.02x | spread > 20 %, no claim |
| manifest-scan (1e6 / mean) | 4,315.9 qps | 397.6 % | 3,110.4 qps | 157.8 % | 0.72x | spread > 20 %, no claim |
| voyage-rollup (1e6 / mean) | 4,960.3 qps | 414.3 % | 4,029.0 qps | 226.1 % | 0.81x | spread > 20 %, no claim |
| customer-statement (1e6 / mean) | 2,971.8 qps | 474.4 % | 6,680.0 qps | 126.2 % | 2.25x | spread > 20 %, no claim |

Statements, 1 booker at one core, `strict`:

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| load-organizations (1e6 / mean) | 6,514.7 qps | 4.5 % | 6,561.7 qps | 9.3 % | 1.01x | inside the spread |
| load-ships (1e6 / mean) | 6,680.0 qps | 29.5 % | 7,052.2 qps | 4.3 % | 1.06x | spread > 20 %, no claim |
| load-operations (1e6 / mean) | 6,825.9 qps | 15.8 % | 7,225.4 qps | 4.7 % | 1.06x | inside the spread |
| load-fees (1e6 / mean) | 7,142.9 qps | 18.8 % | 7,352.9 qps | 6.9 % | 1.03x | inside the spread |
| load-recipes (1e6 / mean) | 6,734.0 qps | 13.7 % | 6,939.6 qps | 17.9 % | 1.03x | inside the spread |
| load-cargos (1e6 / mean) | 6,896.6 qps | 12.1 % | 6,973.5 qps | 13.1 % | 1.01x | inside the spread |
| booking (1e6 / mean) | 2,045.0 qps | 0.9 % | 2,045.8 qps | 1.1 % | 1.00x | inside the spread |
| cargo-lookup (1e6 / mean) | 28,169.0 qps | 1.4 % | 28,248.6 qps | 3.4 % | 1.00x | inside the spread |
| credit-lookup (1e6 / mean) | 33,003.3 qps | 2.7 % | 33,112.6 qps | 2.0 % | 1.00x | inside the spread |
| capacity-read (1e6 / mean) | 33,783.8 qps | 2.0 % | 33,898.3 qps | 1.7 % | 1.00x | inside the spread |
| recipe-read (1e6 / mean) | 14,814.8 qps | 1.2 % | 14,858.8 qps | 1.8 % | 1.00x | inside the spread |
| freight-insert (1e6 / mean) | 40,322.6 qps | 3.7 % | 40,983.6 qps | 1.6 % | 1.02x | inside the spread |
| charge-insert (1e6 / mean) | 47,393.4 qps | 1.4 % | 47,619.0 qps | 1.4 % | 1.00x | inside the spread |
| operation-update (1e6 / mean) | 42,553.2 qps | 3.5 % | 42,918.5 qps | 3.0 % | 1.01x | inside the spread |
| org-update (1e6 / mean) | 45,871.6 qps | 2.3 % | 45,871.6 qps | 1.4 % | 1.00x | inside the spread |
| commit (1e6 / mean) | 7,412.9 qps | 2.9 % | 7,380.1 qps | 1.0 % | 1.00x | inside the spread |
| manifest-scan (1e6 / mean) | 8,795.1 qps | 56.6 % | 7,745.9 qps | 25.8 % | 0.88x | spread > 20 %, no claim |
| voyage-rollup (1e6 / mean) | 10,235.4 qps | 64.2 % | 9,756.1 qps | 9.6 % | 0.95x | spread > 20 %, no claim |
| customer-statement (1e6 / mean) | 3,132.8 qps | 26.5 % | 3,112.4 qps | 16.6 % | 0.99x | spread > 20 %, no claim |

Statements, 8 bookers at eight cores, `group`:

| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| load-organizations (1e6 / mean) | 5,675.4 qps | 14.8 % | 5,717.6 qps | 61.6 % | 1.01x | spread > 20 %, no claim |
| load-ships (1e6 / mean) | 6,480.9 qps | 15.6 % | 6,613.8 qps | 9.9 % | 1.02x | inside the spread |
| load-operations (1e6 / mean) | 6,414.4 qps | 9.4 % | 6,497.7 qps | 66.6 % | 1.01x | spread > 20 %, no claim |
| load-fees (1e6 / mean) | 6,835.3 qps | 13.9 % | 6,337.1 qps | 37.9 % | 0.93x | spread > 20 %, no claim |
| load-recipes (1e6 / mean) | 6,257.8 qps | 9.4 % | 6,510.4 qps | 71.8 % | 1.04x | spread > 20 %, no claim |
| load-cargos (1e6 / mean) | 6,153.8 qps | 11.0 % | 6,476.7 qps | 65.0 % | 1.05x | spread > 20 %, no claim |
| booking (1e6 / mean) | 1,611.6 qps | 2.8 % | 1,662.2 qps | 62.3 % | 1.03x | spread > 20 %, no claim |
| cargo-lookup (1e6 / mean) | 23,310.0 qps | 5.2 % | 23,923.4 qps | 34.1 % | 1.03x | spread > 20 %, no claim |
| credit-lookup (1e6 / mean) | 29,069.8 qps | 3.7 % | 29,498.5 qps | 37.9 % | 1.01x | spread > 20 %, no claim |
| capacity-read (1e6 / mean) | 29,498.5 qps | 4.6 % | 30,211.5 qps | 37.4 % | 1.02x | spread > 20 %, no claim |
| recipe-read (1e6 / mean) | 13,568.5 qps | 3.3 % | 13,755.2 qps | 21.2 % | 1.01x | spread > 20 %, no claim |
| freight-insert (1e6 / mean) | 33,333.3 qps | 5.5 % | 33,444.8 qps | 34.1 % | 1.00x | spread > 20 %, no claim |
| charge-insert (1e6 / mean) | 38,759.7 qps | 2.7 % | 40,816.3 qps | 17.4 % | 1.05x | inside the spread |
| operation-update (1e6 / mean) | 35,211.3 qps | 2.5 % | 36,900.4 qps | 6.7 % | 1.05x | inside the spread |
| org-update (1e6 / mean) | 36,630.0 qps | 3.3 % | 38,314.2 qps | 4.2 % | 1.05x | **finding** |
| commit (1e6 / mean) | 4,906.8 qps | 9.6 % | 4,931.0 qps | 85.4 % | 1.00x | spread > 20 %, no claim |
| manifest-scan (1e6 / mean) | 5,777.0 qps | 73.1 % | 8,928.6 qps | 63.8 % | 1.55x | spread > 20 %, no claim |
| voyage-rollup (1e6 / mean) | 6,872.9 qps | 64.4 % | 11,695.9 qps | 57.8 % | 1.70x | spread > 20 %, no claim |
| customer-statement (1e6 / mean) | 4,672.9 qps | 64.2 % | 8,503.4 qps | 109.5 % | 1.82x | spread > 20 %, no claim |

## 9. What the run says about the engine

- **No change the pinned A/B can read at one core.** One booker, `strict`: 1.00x on tps, 1.00x to 1.02x on the booking and its nine statements, every one inside its spread (the three reporter reads, 10 to 30 statements a run, do not resolve). Eight bookers, `group`: 4,228.0 tps against 4,192.8 tps (1.01x); the old arm's fifth run stalled (526.1 tps), which puts every row of that cell over 20 %, and the other nine runs lie between 3,991.7 and 4,257.2 tps.
- **At eight cores the A/B leans 3 % to 5 % toward the new engine, without a claim.** 9,801.8 tps against 9,543.2 tps (1.03x); the new arm's fifth run stalled (4,149.1 tps), so the rule gives no claim; the new arm's other four runs are 9,593.5 to 9,874.0 tps and the old arm's five 9,288.8 to 9,661.8 tps. `charge-insert`, `operation-update` and `org-update` read 1.05x each, the last marked (+4.6 % against spreads of 4.2 % and 3.3 %). It is not attributed to a milestone: the unpinned series cannot see it (1.06x, spread 51.3 %), and at one core the same statements read 1.00x to 1.02x.
- **Two stalled runs, cause unverified.** `pin-ab-old5-s2-c1-g` (old engine): booking max 2,176.4 ms, commit max 805.9 ms, `charge-insert` max 523.1 ms. `pin-ab-new5-s2-c8-g` (new engine): commit p95 10.8 ms against a p50 of 0.2 ms. Neither cell has a foreign CPU sample or a gate wait. The monitor samples CPU only, so a device stall from another process is not excluded; the check is an I/O sampler (`iostat -x 1`) beside the cells. One stall is in each engine, so the pair says nothing about either.
- **An insert can be refused by a descent that gave up, on both engines.** At eight cores the server logs `btree descent for key <k> from page <p> gave up after 5 attempts: each leaf it reached had already given the key away` and answers the `INSERT` `TXN_CONFLICT retryable=1`, on `charges` (29 of the 37) and `freights` (8) at keys 107 to 138: 24 such refusals over the old arm's eight eight-core A/B cells and 13 over the new arm's eight. The driver retries them and they are inside `conflicted`. Counts this small carry no claim between the engines.
- The cross-host ratios (2.06x to 3.15x on the matrix, 2.23x to 3.52x on the booker sweep) are the host.
- The unpinned series marks one row, `load-cargos` in the eight-booker one-core cell (1.02x against spreads of 1.4 % and 1.3 %). Pinned, that row carries no claim (1.12x, the old arm's stalled run inside it) and reads 0.99x in the single-booker cell and 1.05x, without a claim, at eight cores. It is not read as an engine change.

## 10. Noise floor, contamination and host

- **The unpinned cells are bimodal on this host** (hybrid CPU, governor `powersave`; summary, Stamp, the Host row): `s2-c8-s` ran 8,175, 4,031 and 7,521 tps and `b1-s2-c1-g` 1,821, 1,098 and 1,271 tps, with no foreign sample of 50 % or more of one CPU inside those cells. Four of the six matrix cells spread over 20 % (the `v3.5.0` run's spread 2.1 % to 5.1 %).
- **No scenario 2 cell carries a foreign sample of 50 % or more of one CPU**: not the main cells, the extras or either A/B series. (One sample inside `s2-c1-g` run 1 shows six `python3 -c` processes at 10.9 % each; they are this driver's own booker processes, `multiprocessing` children.)
- Gate waits on scenario 2 cells: `ab-new1-s2-c1-g` 720 s (another session's build and test cycle); every other scenario 2 gate of the main run and the unpinned A/B 0 s; pinned A/B: `pin-ab-new1-b1-s2-c1-s` 46 s, `pin-ab-new1-s2-c8-g` 6 s, the rest 0 s.

## 11. Archive

`bench/v3.5.0/archive/scenario2-v3.4.17-159-g9f8f8b354/` (per-cell JSON, driver stdout, the schema and load invocations' output, prechecks, gate waits, start and end times, the saturation cells' CPU samples, server configs and logs) and `bench/v3.5.0/archive/run-v3.4.17-159-g9f8f8b354/`. No data file is kept.
