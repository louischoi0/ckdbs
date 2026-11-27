
## relaxed

### relaxed: runs

| run | rolls in window | window s | interval between rolls p50 (ms) | log MiB/s | rows/s | wal_first_segment | segments removed | sync_segs max | reader p99 from-send (us) | reader p99 from-schedule (us) | slow(>1ms) samples |
|---|---|---|---|---|---|---|---|---|---|---|---|
| relaxed-mixed-r1 | 30 (counter 30.0) | 30.7 | 882 | 61.3 | 140437 | 0.0 | 0.0 | 36 | 634 | 1892781 | 2491 |
| relaxed-mixed-r2 | 30 (counter 30.0) | 30.6 | 883 | 62.7 | 155733 | 0.0 | 0.0 | 33 | 638 | 1477422 | 1814 |

### relaxed: roll hold (ms), pooled over 2 run(s), 60 rolls

| series | p0 | p25 | p50 | p75 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|
| hold | 0.87 | 3.94 | 574.42 | 915.87 | 1868.45 | 3001.34 | 3001.34 |
| hold, relaxed-mixed-r1 (n=30) | 1.20 | 3.75 | 574.42 | 1328.79 | 1889.42 | 2305.89 | 2305.89 |
| hold, relaxed-mixed-r2 (n=30) | 0.87 | 144.48 | 573.04 | 802.66 | 1529.39 | 3001.34 | 3001.34 |

### relaxed: the hold per step (ms; share = sum of step / sum of hold)

| step | share of hold | p0 | p25 | p50 | p75 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| seal | 0.0 % | 0.00 | 0.01 | 0.02 | 0.04 | 0.06 | 0.07 | 0.07 |
| flush | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| sync_gate (wait on writer-thread sync) | 95.2 % | 0.00 | 2.21 | 569.93 | 913.81 | 1852.85 | 2740.01 | 2740.01 |
| sync (roll's own, total) | 2.8 % | 0.54 | 0.96 | 1.48 | 2.26 | 15.21 | 646.40 | 646.40 |
|   sync tail | 0.1 % | 0.05 | 0.05 | 0.05 | 0.24 | 1.02 | 13.21 | 13.21 |
|   sync rest (non-tail segs) | 2.7 % | 0.29 | 0.88 | 1.25 | 1.80 | 14.97 | 646.34 | 646.34 |
| create (total) | 2.0 % | 0.30 | 0.38 | 0.45 | 0.51 | 10.19 | 378.30 | 378.30 |
|   body_wait | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
|   body inline | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
|   header write+sync | 1.2 % | 0.11 | 0.12 | 0.13 | 0.16 | 0.21 | 377.79 | 377.79 |
|   rename | 0.0 % | 0.01 | 0.02 | 0.02 | 0.02 | 0.04 | 0.12 | 0.12 |
|   dirsync | 0.8 % | 0.15 | 0.23 | 0.28 | 0.33 | 0.44 | 305.87 | 305.87 |
| unaccounted (hold - listed) | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.01 | 0.01 | 0.01 |

Body outcome: hit 60, waited 0, inline 0 (of 60).
Segments covered by the roll's own sync (sync_segs): p0/p50/p99/max = 4/20/36/36; waited_syncs>0 in 46 of 60 rolls.

### relaxed: the residual under the latch if sync, header, rename, dirsync leave it (us; bar 10,000 us)

| residual | n | p50 | p90 | p99 | max | rolls > 10 ms |
|---|---|---|---|---|---|---|
| R1 = seal + flush + body_wait + body | 60 | 19 | 61 | 74 | 74 | 0 |
| R2 = seal + flush | 60 | 19 | 61 | 74 | 74 | 0 |

### relaxed: could one thread keep up with the syncs if they left the latch

| series | p50 | p90 | p99 | max |
|---|---|---|---|---|
| gate + own sync (us) | 571036 | 1867982 | 2739008 | 2739008 |
| waited writer-thread sync, tail+rest (us) | 689040 | 1949244 | 2426632 | 2426632 |
| interval to next roll (us) | 882470.012 | 2180411.845 | 3052939.06 | 3052939.06 |

Nominal interval (one 64 MiB segment at the run's mean log byte rate) = 1032 ms; this is the stalled-free-rate-free reading, because the measured intervals above are stretched by the holds themselves. (gate + own sync) exceeds it in **14** of 58 rolls; the waited writer-thread sync exceeds it in **14**.

Consecutive roll pairs: 58. (gate + own sync) of roll k exceeds the interval to roll k+1 in **0** pairs; the waited writer-thread sync of roll k exceeds that interval in **0**.

### relaxed: the point readers (us)

| arm | metric | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| with bulk writer, relaxed-mixed-r1 | read_service | 11999 | 28 | 168 | 256 | 442 | 634 | 1522544 | 2298025 |
| with bulk writer, relaxed-mixed-r1 | read_from_schedule | 11999 | 63 | 366 | 456 | 1281390 | 1892781 | 2150126 | 2298172 |
| with bulk writer, relaxed-mixed-r2 | read_service | 11538 | 29 | 195 | 264 | 453 | 638 | 1100418 | 2989946 |
| with bulk writer, relaxed-mixed-r2 | read_from_schedule | 11538 | 78 | 369 | 452 | 702080 | 1477422 | 2613827 | 2990067 |
| alone (isolated), relaxed-iso-r1 | read_service | 12006 | 89 | 580 | 680 | 824 | 873 | 1486 | 3350 |
| alone (isolated), relaxed-iso-r1 | read_from_schedule | 12006 | 151 | 790 | 898 | 1064 | 1118 | 1742 | 3447 |
| alone (isolated), relaxed-iso-r2 | read_service | 12006 | 91 | 595 | 683 | 825 | 882 | 1157 | 2413 |
| alone (isolated), relaxed-iso-r2 | read_from_schedule | 12006 | 150 | 806 | 897 | 1069 | 1129 | 1746 | 4309 |

p99 ratio read_service, relaxed-mixed-r1 / relaxed-iso-r1 = 0.7x (634 / 873 us).

p99 ratio read_service, relaxed-mixed-r1 / relaxed-iso-r2 = 0.7x (634 / 882 us).

p99 ratio read_from_schedule, relaxed-mixed-r1 / relaxed-iso-r1 = 1693.8x (1892781 / 1118 us).

p99 ratio read_from_schedule, relaxed-mixed-r1 / relaxed-iso-r2 = 1676.2x (1892781 / 1129 us).

p99 ratio read_service, relaxed-mixed-r2 / relaxed-iso-r1 = 0.7x (638 / 873 us).

p99 ratio read_service, relaxed-mixed-r2 / relaxed-iso-r2 = 0.7x (638 / 882 us).

p99 ratio read_from_schedule, relaxed-mixed-r2 / relaxed-iso-r1 = 1322.1x (1477422 / 1118 us).

p99 ratio read_from_schedule, relaxed-mixed-r2 / relaxed-iso-r2 = 1308.4x (1477422 / 1129 us).

### relaxed: slow reader samples (> 1000 us from schedule) against roll windows

| run | slow samples | overlapping a roll [start,end] | overlapping a roll + 50 ms | > 10 ms and overlapping | > 10 ms total | slow outside any roll window (us, max) |
|---|---|---|---|---|---|---|
| relaxed-mixed-r1 | 2491 | 2464 | 2476 | 2438 | 2438 | 7250 |
| relaxed-mixed-r2 | 1814 | 1791 | 1805 | 1777 | 1780 | 14659 |

### relaxed: SHOW META wal_roll_* deltas (m1 - m0)

| run | wal_rolls | hold_us | gate_us | sync_us | create_us | body hit/waited/inline | c0 stream-latch wait (us) | c0 roll_blocked_wait_us | wal_syncs |
|---|---|---|---|---|---|---|---|---|---|
| relaxed-mixed-r1 | 30.0 | 21849727.0 | 20764962.0 | 242710.0 | 841073.0 | 30.0/0.0/0.0 | 21733038.0 | 21729746.0 | 481.0 |
| relaxed-mixed-r2 | 30.0 | 21297495.0 | 20289710.0 | 982443.0 | 24553.0 | 30.0/0.0/0.0 | 21168168.0 | 21165438.0 | 471.0 |

## group

### group: runs

| run | rolls in window | window s | interval between rolls p50 (ms) | log MiB/s | rows/s | wal_first_segment | segments removed | sync_segs max | reader p99 from-send (us) | reader p99 from-schedule (us) | slow(>1ms) samples |
|---|---|---|---|---|---|---|---|---|---|---|---|
| group-mixed-r1 | 20 (counter 20.0) | 30.1 | 1261 | 42.8 | 96324 | 10.0 | 10.0 | 18 | 678 | 893 | 53 |
| group-mixed-r2 | 23 (counter 23.0) | 30.7 | 1184 | 48.0 | 114203 | 10.0 | 10.0 | 25 | 688 | 909 | 46 |

### group: roll hold (ms), pooled over 2 run(s), 43 rolls

| series | p0 | p25 | p50 | p75 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|
| hold | 0.75 | 1.35 | 1.65 | 2.38 | 4.20 | 15.34 | 15.34 |
| hold, group-mixed-r1 (n=20) | 0.75 | 1.01 | 1.55 | 1.76 | 3.10 | 12.74 | 12.74 |
| hold, group-mixed-r2 (n=23) | 1.08 | 1.45 | 1.70 | 2.59 | 4.20 | 15.34 | 15.34 |

### group: the hold per step (ms; share = sum of step / sum of hold)

| step | share of hold | p0 | p25 | p50 | p75 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| seal | 0.5 % | 0.00 | 0.01 | 0.01 | 0.02 | 0.03 | 0.03 | 0.03 |
| flush | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| sync_gate (wait on writer-thread sync) | 1.7 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 1.00 | 1.00 |
| sync (roll's own, total) | 69.0 % | 0.40 | 0.90 | 1.11 | 1.42 | 2.17 | 14.13 | 14.13 |
|   sync tail | 34.3 % | 0.05 | 0.15 | 0.19 | 0.20 | 0.30 | 12.90 | 12.90 |
|   sync rest (non-tail segs) | 34.6 % | 0.15 | 0.66 | 0.95 | 1.23 | 1.64 | 1.94 | 1.94 |
| create (total) | 28.8 % | 0.29 | 0.39 | 0.44 | 0.49 | 0.55 | 13.10 | 13.10 |
|   body_wait | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
|   body inline | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
|   header write+sync | 17.0 % | 0.11 | 0.12 | 0.12 | 0.14 | 0.16 | 12.54 | 12.54 |
|   rename | 0.7 % | 0.01 | 0.02 | 0.02 | 0.02 | 0.02 | 0.05 | 0.05 |
|   dirsync | 10.9 % | 0.16 | 0.24 | 0.28 | 0.32 | 0.36 | 0.90 | 0.90 |
| unaccounted (hold - listed) | 0.1 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |

Body outcome: hit 43, waited 0, inline 0 (of 43).
Segments covered by the roll's own sync (sync_segs): p0/p50/p99/max = 3/14/25/25; waited_syncs>0 in 2 of 43 rolls.

### group: the residual under the latch if sync, header, rename, dirsync leave it (us; bar 10,000 us)

| residual | n | p50 | p90 | p99 | max | rolls > 10 ms |
|---|---|---|---|---|---|---|
| R1 = seal + flush + body_wait + body | 43 | 15 | 25 | 27 | 27 | 0 |
| R2 = seal + flush | 43 | 15 | 25 | 27 | 27 | 0 |

### group: could one thread keep up with the syncs if they left the latch

| series | p50 | p90 | p99 | max |
|---|---|---|---|---|
| gate + own sync (us) | 1110 | 2423 | 14132 | 14132 |
| waited writer-thread sync, tail+rest (us) | 0 | 0 | 1274 | 1274 |
| interval to next roll (us) | 1240966.547 | 1994747.524 | 3690532.997 | 3690532.997 |

Nominal interval (one 64 MiB segment at the run's mean log byte rate) = 1414 ms; this is the stalled-free-rate-free reading, because the measured intervals above are stretched by the holds themselves. (gate + own sync) exceeds it in **0** of 41 rolls; the waited writer-thread sync exceeds it in **0**.

Consecutive roll pairs: 41. (gate + own sync) of roll k exceeds the interval to roll k+1 in **0** pairs; the waited writer-thread sync of roll k exceeds that interval in **0**.

### group: the point readers (us)

| arm | metric | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| with bulk writer, group-mixed-r1 | read_service | 12006 | 37 | 222 | 282 | 490 | 678 | 1168 | 2902 |
| with bulk writer, group-mixed-r1 | read_from_schedule | 12006 | 69 | 353 | 429 | 680 | 893 | 1826 | 4055 |
| with bulk writer, group-mixed-r2 | read_service | 12006 | 44 | 215 | 278 | 491 | 688 | 920 | 7787 |
| with bulk writer, group-mixed-r2 | read_from_schedule | 12006 | 97 | 344 | 429 | 683 | 909 | 1603 | 7917 |
| alone (isolated), group-iso-r1 | read_service | 12006 | 85 | 559 | 681 | 820 | 865 | 1461 | 2736 |
| alone (isolated), group-iso-r1 | read_from_schedule | 12006 | 146 | 767 | 901 | 1064 | 1123 | 2132 | 3026 |

p99 ratio read_service, group-mixed-r1 / group-iso-r1 = 0.8x (678 / 865 us).

p99 ratio read_from_schedule, group-mixed-r1 / group-iso-r1 = 0.8x (893 / 1123 us).

p99 ratio read_service, group-mixed-r2 / group-iso-r1 = 0.8x (688 / 865 us).

p99 ratio read_from_schedule, group-mixed-r2 / group-iso-r1 = 0.8x (909 / 1123 us).

### group: slow reader samples (> 1000 us from schedule) against roll windows

| run | slow samples | overlapping a roll [start,end] | overlapping a roll + 50 ms | > 10 ms and overlapping | > 10 ms total | slow outside any roll window (us, max) |
|---|---|---|---|---|---|---|
| group-mixed-r1 | 53 | 0 | 2 | 0 | 0 | 4055 |
| group-mixed-r2 | 46 | 3 | 3 | 0 | 0 | 2903 |

### group: SHOW META wal_roll_* deltas (m1 - m0)

| run | wal_rolls | hold_us | gate_us | sync_us | create_us | body hit/waited/inline | c0 stream-latch wait (us) | c0 roll_blocked_wait_us | wal_syncs |
|---|---|---|---|---|---|---|---|---|---|
| group-mixed-r1 | 20.0 | 49317.0 | 991.0 | 36663.0 | 11372.0 | 20.0/0.0/0.0 | 660.0 | 630.0 | 3080.0 |
| group-mixed-r2 | 23.0 | 69659.0 | 1003.0 | 45396.0 | 22853.0 | 23.0/0.0/0.0 | 10907.0 | 10405.0 | 3903.0 |

## strict

### strict: runs

| run | rolls in window | window s | interval between rolls p50 (ms) | log MiB/s | rows/s | wal_first_segment | segments removed | sync_segs max | reader p99 from-send (us) | reader p99 from-schedule (us) | slow(>1ms) samples |
|---|---|---|---|---|---|---|---|---|---|---|---|
| strict-mixed-r1 | 19 (counter 19.0) | 30.5 | 1588 | 39.9 | 91287 | 10.0 | 10.0 | 19 | 632 | 876 | 38 |

### strict: roll hold (ms), pooled over 1 run(s), 19 rolls

| series | p0 | p25 | p50 | p75 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|
| hold | 0.91 | 1.22 | 1.39 | 1.47 | 2.40 | 15.79 | 15.79 |
| hold, strict-mixed-r1 (n=19) | 0.91 | 1.22 | 1.39 | 1.47 | 2.40 | 15.79 | 15.79 |

### strict: the hold per step (ms; share = sum of step / sum of hold)

| step | share of hold | p0 | p25 | p50 | p75 | p90 | p99 | max |
|---|---|---|---|---|---|---|---|---|
| seal | 0.7 % | 0.00 | 0.01 | 0.01 | 0.02 | 0.02 | 0.03 | 0.03 |
| flush | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| sync_gate (wait on writer-thread sync) | 3.7 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.55 | 0.94 | 0.94 |
| sync (roll's own, total) | 74.5 % | 0.44 | 0.77 | 0.90 | 1.08 | 1.11 | 14.49 | 14.49 |
|   sync tail | 41.1 % | 0.05 | 0.14 | 0.19 | 0.22 | 0.24 | 13.64 | 13.64 |
|   sync rest (non-tail segs) | 33.4 % | 0.32 | 0.56 | 0.75 | 0.88 | 0.93 | 0.98 | 0.98 |
| create (total) | 21.1 % | 0.30 | 0.38 | 0.41 | 0.46 | 0.48 | 1.27 | 1.27 |
|   body_wait | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
|   body inline | 0.0 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
|   header write+sync | 5.9 % | 0.11 | 0.11 | 0.12 | 0.12 | 0.16 | 0.26 | 0.26 |
|   rename | 0.9 % | 0.01 | 0.01 | 0.02 | 0.02 | 0.02 | 0.04 | 0.04 |
|   dirsync | 14.0 % | 0.18 | 0.24 | 0.27 | 0.30 | 0.34 | 0.99 | 0.99 |
| unaccounted (hold - listed) | 0.1 % | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |

Body outcome: hit 19, waited 0, inline 0 (of 19).
Segments covered by the roll's own sync (sync_segs): p0/p50/p99/max = 6/14/19/19; waited_syncs>0 in 2 of 19 rolls.

### strict: the residual under the latch if sync, header, rename, dirsync leave it (us; bar 10,000 us)

| residual | n | p50 | p90 | p99 | max | rolls > 10 ms |
|---|---|---|---|---|---|---|
| R1 = seal + flush + body_wait + body | 19 | 14 | 24 | 26 | 26 | 0 |
| R2 = seal + flush | 19 | 14 | 24 | 26 | 26 | 0 |

### strict: could one thread keep up with the syncs if they left the latch

| series | p50 | p90 | p99 | max |
|---|---|---|---|---|
| gate + own sync (us) | 903 | 1970 | 14488 | 14488 |
| waited writer-thread sync, tail+rest (us) | 0 | 551 | 1169 | 1169 |
| interval to next roll (us) | 1588151.167 | 3020938.499 | 3366278.183 | 3366278.183 |

Nominal interval (one 64 MiB segment at the run's mean log byte rate) = 1604 ms; this is the stalled-free-rate-free reading, because the measured intervals above are stretched by the holds themselves. (gate + own sync) exceeds it in **0** of 18 rolls; the waited writer-thread sync exceeds it in **0**.

Consecutive roll pairs: 18. (gate + own sync) of roll k exceeds the interval to roll k+1 in **0** pairs; the waited writer-thread sync of roll k exceeds that interval in **0**.

### strict: the point readers (us)

| arm | metric | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| with bulk writer, strict-mixed-r1 | read_service | 12006 | 34 | 219 | 281 | 457 | 632 | 893 | 4753 |
| with bulk writer, strict-mixed-r1 | read_from_schedule | 12006 | 44 | 349 | 431 | 648 | 876 | 1484 | 4941 |
| alone (isolated), strict-iso-r1 | read_service | 12006 | 72 | 588 | 697 | 822 | 863 | 1017 | 2809 |
| alone (isolated), strict-iso-r1 | read_from_schedule | 12006 | 129 | 805 | 916 | 1069 | 1120 | 1752 | 3006 |

p99 ratio read_service, strict-mixed-r1 / strict-iso-r1 = 0.7x (632 / 863 us).

p99 ratio read_from_schedule, strict-mixed-r1 / strict-iso-r1 = 0.8x (876 / 1120 us).

### strict: slow reader samples (> 1000 us from schedule) against roll windows

| run | slow samples | overlapping a roll [start,end] | overlapping a roll + 50 ms | > 10 ms and overlapping | > 10 ms total | slow outside any roll window (us, max) |
|---|---|---|---|---|---|---|
| strict-mixed-r1 | 38 | 0 | 1 | 0 | 0 | 4941 |

### strict: SHOW META wal_roll_* deltas (m1 - m0)

| run | wal_rolls | hold_us | gate_us | sync_us | create_us | body hit/waited/inline | c0 stream-latch wait (us) | c0 roll_blocked_wait_us | wal_syncs |
|---|---|---|---|---|---|---|---|---|---|
| strict-mixed-r1 | 19.0 | 40611.0 | 1493.0 | 30250.0 | 8575.0 | 19.0/0.0/0.0 | 12088.0 | 11597.0 | 3263.0 |
