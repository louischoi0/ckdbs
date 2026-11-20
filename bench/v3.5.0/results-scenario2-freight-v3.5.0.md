# Scenario 2 (freight) at `v3.5.0`

**One line.** At the tag `v3.5.0` (`07f4552a`) the booking path is where `v3.4.14` left it on the same host: the six matrix cells read -2.0 % to +2.7 % against `v3.4.14`, every one inside its spread (1,173 tps at one core and 2,613 tps at eight, `group`, eight bookers), the A/B against the `v3.4.14` binary reads 0.99x and 1.02x, and the booker sweep is level up to 32 bookers (3,736 tps `group`, 3,571 tps `strict`, single runs). DJ's slot wait and connection accounting have no price this workload can read. The invariant failures under more than one booker are the driver's client-side lost update, as before: 0 of 400 with one booker and 0 of 400 under repeatable-read.

Driver: `tools/scenario2_freight.py` of the tag, unmodified, `--organizations 300 --ships 30 --operations 300 --cargos 4000 --bookers 8 --bookings 3000 --verify 100 --seed 1 --sync`, schema, load and run as three invocations. Comparator: `results-scenario2-freight-v3.4.14.md` (this host).

## 1. Stamp

| Field | Value |
|---|---|
| Date and time | 2026-11-20; main cells 18:09:53 to 18:15:44 UTC; A/B 18:28 to 18:36 UTC; extras (repeatable-read, 16 bookers) to 18:38:27 UTC |
| Version, commit, binary, host, device, budget | stamped once in `summary-scenarios-v3.5.0.md`, section Stamp |
| Server config | the common config; `cores` and `durability` per cell; ports 15604 to 15607 |
| Driver | exit 0 in every cell |

## 2. What was run

The `v3.4.14` run's cells, unchanged: four matrix cells (`cores` 1 or 8, `group` or `strict`) and two single-booker cells, three times each; the booker sweep (1, 2, 4 and 16 bookers at eight cores, once each); the 32-booker saturation cells (once each, with a 1 s host CPU sampler); repeatable-read at one and eight cores and 16 bookers again (extras, once each); and the A/B (8 bookers `c1-g` and 1 booker `c1-s`, three interleaved pairs). **Cut**: the extreme cells of `v3.4.14` (20,000 bookings, 32 bookers, one and two client processes) and PostgreSQL (no server on this host).

## 3. Throughput

| cell | cores | durability | run 1 | run 2 | run 3 | median | spread | v3.4.14 median | spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | group | 1,172.7 tps | 1,182.7 tps | 1,131.8 tps | **1,172.7 tps** | 4.3 % | 1,183.3 tps | 1.0 % | -0.9 % | inside the spread |
| `s2-c8-g` | 8 | group | 2,612.6 tps | 2,545.0 tps | 2,669.7 tps | **2,612.6 tps** | 4.8 % | 2,548.8 tps | 1.4 % | +2.5 % | inside the spread |
| `s2-c1-s` | 1 | strict | 1,108.6 tps | 1,089.3 tps | 1,130.1 tps | **1,108.6 tps** | 3.7 % | 1,123.5 tps | 5.1 % | -1.3 % | inside the spread |
| `s2-c8-s` | 8 | strict | 2,299.9 tps | 2,384.5 tps | 2,421.2 tps | **2,384.5 tps** | 5.1 % | 2,321.0 tps | 2.7 % | +2.7 % | inside the spread |
| `b1-s2-c1-g` | 1 | group | 636.9 tps | 615.9 tps | 613.6 tps | **615.9 tps** | 3.8 % | 628.7 tps | 3.7 % | -2.0 % | inside the spread |
| `b1-s2-c1-s` | 1 | strict | 631.3 tps | 637.4 tps | 624.0 tps | **631.3 tps** | 2.1 % | 625.8 tps | 6.7 % | +0.9 % | inside the spread |

## 4. The booker sweep and saturation

Eight bookers are the matrix cells (three runs); every other row is one run, so its delta carries no claim.

| bookers | durability | tps now | runs | spread | v3.4.14 | delta | conflicted (run 1) | verify failures / checks (run 1) |
|---|---|---|---|---|---|---|---|---|
| 1 | group | **568.2 tps** | 1 | n/a | 578.6 tps | -1.8 % | 0 | 0 / 400 |
| 1 | strict | **572.1 tps** | 1 | n/a | 575.7 tps | -0.6 % | 0 | 0 / 400 |
| 2 | group | **1,058.2 tps** | 1 | n/a | 1,044.5 tps | +1.3 % | 0 | 4 / 400 |
| 2 | strict | **914.5 tps** | 1 | n/a | 910.6 tps | +0.4 % | 1 | 6 / 400 |
| 4 | group | **1,718.1 tps** | 1 | n/a | 1,700.8 tps | +1.0 % | 0 | 4 / 400 |
| 4 | strict | **1,532.1 tps** | 1 | n/a | 1,532.4 tps | -0.0 % | 13 | 5 / 400 |
| 8 | group | **2,612.6 tps** | 3 | 4.8 % | 2,548.8 tps | +2.5 % | 3 | 27 / 400 |
| 8 | strict | **2,384.5 tps** | 3 | 5.1 % | 2,321.0 tps | +2.7 % | 47 | 11 / 400 |
| 16 | group | **3,522.9 tps** | 1 | n/a | 3,474.0 tps | +1.4 % | 2 | 51 / 400 |
| 16 | strict | **3,286.8 tps** | 1 | n/a | 3,165.1 tps | +3.8 % | 105 | 45 / 400 |
| 32 | group | **3,735.5 tps** | 1 | n/a | 3,841.6 tps | -2.8 % | 1 | 80 / 400 |
| 32 | strict | **3,570.9 tps** | 1 | n/a | 3,471.0 tps | +2.9 % | 189 | 75 / 400 |

The 16-booker extras repeat: `group` 3,503.8 tps (`v3.4.14` 3,427.8 tps), `strict` 3,236.5 tps (3,231.4 tps).

## 5. Where the time goes

Mean booking latency and the commit's share of it, run 1:

| cell | booking mean now | commit share now | booking mean v3.4.14 | commit share v3.4.14 |
|---|---|---|---|---|
| `s2-c1-g` | 6,038 µs | 13.6 % | 6,002 µs | 13.5 % |
| `s2-c8-g` | 2,662 µs | 28.0 % | 2,654 µs | 29.2 % |
| `s2-c1-s` | 6,447 µs | 11.3 % | 6,331 µs | 11.5 % |
| `s2-c8-s` | 3,028 µs | 26.3 % | 2,984 µs | 26.2 % |
| `b1-s2-c1-g` | 1,379 µs | 35.2 % | 1,386 µs | 35.6 % |
| `b1-s2-c1-s` | 1,391 µs | 34.3 % | 1,404 µs | 35.3 % |

Percentiles, run 1 of `s2-c1-g`:

| phase | ops | p0 | p25 | p50 | p95 | p99 | max | errors |
|---|---|---|---|---|---|---|---|---|
| ddl | 8 | 352.0 µs | 376.8 µs | 577.1 µs | 682.1 µs | 682.1 µs | 682.1 µs | 0 |
| load-organizations | 300 | 314.9 µs | 431.6 µs | 484.3 µs | 652.2 µs | 736.0 µs | 826.4 µs | 0 |
| load-ships | 30 | 408.5 µs | 456.9 µs | 530.5 µs | 711.3 µs | 912.9 µs | 912.9 µs | 0 |
| load-operations | 300 | 319.4 µs | 435.6 µs | 483.7 µs | 704.1 µs | 802.4 µs | 989.6 µs | 0 |
| load-fees | 12 | 353.6 µs | 419.3 µs | 432.9 µs | 518.7 µs | 518.7 µs | 518.7 µs | 0 |
| load-recipes | 93 | 307.3 µs | 405.4 µs | 456.9 µs | 659.8 µs | 757.9 µs | 757.9 µs | 0 |
| load-cargos | 4,000 | 289.8 µs | 420.4 µs | 475.1 µs | 656.0 µs | 772.1 µs | 3,313.1 µs | 0 |
| booking | 3,347 | 590.7 µs | 5,470.2 µs | 6,199.9 µs | 8,026.6 µs | 9,556.7 µs | 25,110.2 µs | 0 |
| cargo-lookup | 3,347 | 72.9 µs | 227.7 µs | 303.2 µs | 893.0 µs | 1,079.2 µs | 4,400.7 µs | 0 |
| credit-lookup | 3,347 | 60.6 µs | 196.3 µs | 272.5 µs | 897.0 µs | 1,089.8 µs | 3,032.5 µs | 0 |
| capacity-read | 3,347 | 59.3 µs | 189.0 µs | 258.0 µs | 890.2 µs | 1,083.5 µs | 4,240.2 µs | 0 |
| recipe-read | 3,347 | 132.8 µs | 299.5 µs | 383.0 µs | 1,016.5 µs | 1,202.5 µs | 3,732.7 µs | 0 |
| freight-insert | 3,000 | 57.8 µs | 202.9 µs | 478.4 µs | 893.4 µs | 1,199.1 µs | 4,169.9 µs | 0 |
| charge-insert | 16,918 | 39.4 µs | 147.7 µs | 206.0 µs | 847.9 µs | 1,022.1 µs | 5,552.9 µs | 0 |
| operation-update | 3,000 | 53.4 µs | 150.0 µs | 206.9 µs | 853.5 µs | 1,074.2 µs | 3,092.0 µs | 0 |
| org-update | 3,000 | 44.0 µs | 145.6 µs | 199.4 µs | 855.5 µs | 1,062.3 µs | 1,970.6 µs | 0 |
| commit | 3,000 | 427.1 µs | 693.2 µs | 821.4 µs | 1,421.9 µs | 1,685.7 µs | 3,548.9 µs | 0 |
| manifest-scan | 60 | 109.0 µs | 294.4 µs | 405.4 µs | 906.6 µs | 963.9 µs | 963.9 µs | 0 |
| voyage-rollup | 60 | 97.0 µs | 262.5 µs | 356.7 µs | 1,129.5 µs | 1,453.2 µs | 1,453.2 µs | 0 |
| customer-statement | 30 | 251.1 µs | 786.9 µs | 1,513.4 µs | 2,420.0 µs | 2,538.0 µs | 2,538.0 µs | 0 |

## 6. Correctness: the invariant failures

| cell | run | committed | rejected-capacity | rejected-credit | conflicted | abandoned | verify failures / checks |
|---|---|---|---|---|---|---|---|
| `s2-c1-g` | 1 | 3,000 | 292 | 55 | 0 | 0 | 24 / 400 |
| `r2-s2-c1-g` | 2 | 3,000 | 290 | 55 | 0 | 0 | 31 / 400 |
| `r3-s2-c1-g` | 3 | 3,000 | 286 | 51 | 0 | 0 | 24 / 400 |
| `s2-c8-g` | 1 | 3,000 | 283 | 55 | 3 | 0 | 27 / 400 |
| `r2-s2-c8-g` | 2 | 3,000 | 288 | 55 | 4 | 0 | 17 / 400 |
| `r3-s2-c8-g` | 3 | 3,000 | 296 | 55 | 2 | 0 | 19 / 400 |
| `s2-c1-s` | 1 | 3,000 | 281 | 46 | 0 | 0 | 28 / 400 |
| `r2-s2-c1-s` | 2 | 3,000 | 278 | 53 | 0 | 0 | 34 / 400 |
| `r3-s2-c1-s` | 3 | 3,000 | 283 | 56 | 0 | 0 | 25 / 400 |
| `s2-c8-s` | 1 | 3,000 | 253 | 54 | 47 | 0 | 11 / 400 |
| `r2-s2-c8-s` | 2 | 3,000 | 297 | 53 | 50 | 0 | 17 / 400 |
| `r3-s2-c8-s` | 3 | 3,000 | 280 | 52 | 49 | 0 | 17 / 400 |
| `b1-s2-c1-g` | 1 | 3,000 | 328 | 74 | 0 | 0 | 0 / 400 |
| `r2-b1-s2-c1-g` | 2 | 3,000 | 328 | 74 | 0 | 0 | 0 / 400 |
| `r3-b1-s2-c1-g` | 3 | 3,000 | 328 | 74 | 0 | 0 | 0 / 400 |
| `b1-s2-c1-s` | 1 | 3,000 | 328 | 74 | 0 | 0 | 0 / 400 |
| `r2-b1-s2-c1-s` | 2 | 3,000 | 328 | 74 | 0 | 0 | 0 / 400 |
| `r3-b1-s2-c1-s` | 3 | 3,000 | 328 | 74 | 0 | 0 | 0 / 400 |

With one booker every run passes 400 of 400; under `--isolation repeatable-read` both cells pass 400 of 400 (`rr-s2-c1-g` 1,110.2 tps, 108 conflicted; `rr-s2-c8-g` 2,634.0 tps, 95 conflicted; `v3.4.14` 1,149.8 and 2,569.2 tps). Under the default isolation with more bookers the `--verify 100` pass fails as at `v3.4.14` (`I1 operation <n>: booked_cbm=..., SUM(freights.cbm)=...`): the driver writes `SET booked_cbm = <literal>` from a value it read earlier in the transaction, the client-side lost update its source names. It is not engine corruption.

## 7. The engine, isolated: `v3.4.14` against `v3.5.0`, interleaved on this host

| measure | v3.4.14 engine | spread | v3.5.0 engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s2 8 bookers, cores 1, group | 1,187.4 tps | 1.8 % | 1,170.5 tps | 0.7 % | 0.99x | inside the spread |
| s2 1 booker, cores 1, strict | 605.8 tps | 3.9 % | 619.7 tps | 3.7 % | 1.02x | inside the spread |
| s2 1 booker strict, commit (1e6 / mean) | 1,800.2 qps | 4.2 % | 1,829.2 qps | 1.4 % | 1.02x | inside the spread |
| s2 1 booker strict, recipe-read (1e6 / mean) | 6,353.2 qps | 9.7 % | 6,570.3 qps | 6.6 % | 1.03x | inside the spread |
| s2 1 booker strict, charge-insert (1e6 / mean) | 20,661.2 qps | 6.7 % | 21,186.4 qps | 3.3 % | 1.03x | inside the spread |

## 8. What the run says about the engine

- **No change between `v3.4.14` and `v3.5.0` on the booking path**, at one or eight cores, `group` or `strict`, one to 32 bookers: every matrix delta is inside its spread and the A/B is 0.99x to 1.03x.
- DJ's wait in place of a slot refusal does not fire here (no cell reaches 1,024 in-flight slots a core), and nothing reads its cost when idle.

## 9. Noise floor, contamination and host

Every quiet-gate wait was 0 s. One A/B cell carries a foreign sample above 50 %: `ab-new3-b1-s2-c1-s` (another session's `claude` process at 74.3 % of one CPU for one sample at 18:35:43 UTC); its tps (619.7 tps median of the new arm) is not an outlier of its group, and it is kept and disclosed.

## 10. Archive

`bench/v3.5.0/archive/scenario2-v3.5.0/` (per-cell JSON, driver stdout, prechecks, gate waits, server configs and logs) and `bench/v3.5.0/archive/run-v3.5.0/` (the run scripts, watchdogs, monitor log, timeline, binary hashes, the table generator `gen.py` and its output under `tables/`). No data file is kept.
