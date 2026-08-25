
### A-creating-t2 (3 runs)

| run | config | started UTC | load at start | wall s | stmt/s | ddl/writer connects | retries | errors | verify |
|---|---|---|---:|---:|---:|---|---|---:|---|
| A-creating-t2-r1 | single-core | 2026-08-25 12:22:05 | 0.39 | 11.40 | 1,228 | None/None | none | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| A-creating-t2-r1 | multi-core | 2026-08-25 12:22:23 | 0.49 | 13.16 | 1,064 | None/None | none | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| A-creating-t2-r2 | single-core | 2026-08-25 12:25:30 | 0.46 | 11.65 | 1,202 | None/None | none | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| A-creating-t2-r2 | multi-core | 2026-08-25 12:25:52 | 0.49 | 11.85 | 1,182 | None/None | none | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| A-creating-t2-r3 | single-core | 2026-08-25 12:28:29 | 0.49 | 11.26 | 1,243 | None/None | none | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| A-creating-t2-r3 | multi-core | 2026-08-25 12:28:51 | 0.48 | 11.45 | 1,222 | None/None | none | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |

ratios: 0.866, 0.983, 0.983 mean 0.944; single mean 1,225 (1,202-1,243); multi mean 1,156 (1,064-1,222)

**insert** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 12,003 | 880 | 2,273.5 | 968 | 2,029 | 2,100 | 2,208 | 2,514 | 3,098 | 5,357 | 59,035 |
| multi-core | 12,003 | 815 | 2,461.4 | 992 | 2,053 | 2,128 | 2,248 | 2,664 | 3,490 | 6,666 | 485,842 |

**point-select** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 12,000 | 62,665 | 30.3 | 23 | 26 | 26 | 27 | 30 | 38 | 59 | 3,168 |
| multi-core | 12,000 | 48,038 | 47.8 | 19 | 26 | 26 | 27 | 35 | 44 | 1,040 | 77,459 |

**update** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 12,000 | 879 | 2,274.8 | 996 | 2,032 | 2,097 | 2,192 | 2,449 | 3,176 | 5,928 | 39,808 |
| multi-core | 12,000 | 842 | 2,367.3 | 996 | 2,060 | 2,132 | 2,236 | 2,519 | 3,473 | 7,226 | 48,156 |

**delete** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 6,000 | 880 | 2,268.9 | 989 | 2,000 | 2,065 | 2,162 | 2,466 | 3,297 | 5,883 | 37,841 |
| multi-core | 6,000 | 840 | 2,363.5 | 991 | 2,045 | 2,124 | 2,236 | 2,599 | 3,753 | 7,283 | 29,379 |

**scan** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 6 | 2,075 | 673.9 | 252 | 261 | 280 | 1,264 | 1,355 | 1,355 | 1,355 | 1,355 |
| multi-core | 6 | 1,450 | 868.1 | 267 | 295 | 502 | 1,368 | 1,460 | 1,460 | 1,460 | 1,460 |

**per run p50 / p99**

| run | config | insert | point-select | update | delete |
|---|---|---:|---:|---:|---:|
| A-creating-t2-r1 | single-core | 2,082 / 4,874 | 26 / 60 | 2,110 / 5,967 | 2,074 / 6,904 |
| A-creating-t2-r1 | multi-core | 2,112 / 9,007 | 27 / 1,139 | 2,171 / 9,169 | 2,204 / 8,653 |
| A-creating-t2-r2 | single-core | 2,118 / 5,712 | 26 / 59 | 2,099 / 6,824 | 2,059 / 4,975 |
| A-creating-t2-r2 | multi-core | 2,147 / 5,843 | 26 / 51 | 2,105 / 7,177 | 2,081 / 6,960 |
| A-creating-t2-r3 | single-core | 2,100 / 5,486 | 26 / 57 | 2,083 / 4,937 | 2,062 / 5,430 |
| A-creating-t2-r3 | multi-core | 2,119 / 5,651 | 26 / 62 | 2,125 / 4,623 | 2,095 / 6,105 |

**first INSERT per relation (multi-core), µs; refills per run**

- A-creating-t2-r1: owners 0,0; first 1,067 · 1,158; second 1,116 · 1,069; max 485,733 · 485,842
  refills: none (no peer listener)
  single-core first 1,076 · 1,155
- A-creating-t2-r2: owners 0,0; first 1,149 · 1,139; second 1,119 · 1,083; max 46,827 · 47,097
  refills: none (no peer listener)
  single-core first 1,111 · 1,229
- A-creating-t2-r3: owners 0,0; first 1,167 · 1,034; second 2,108 · 2,075; max 20,400 · 20,330
  refills: none (no peer listener)
  single-core first 1,244 · 1,213

### B-rotate-t2 (3 runs)

| run | config | started UTC | load at start | wall s | stmt/s | ddl/writer connects | retries | errors | verify |
|---|---|---|---:|---:|---:|---|---|---:|---|
| B-rotate-t2-r1 | single-core | 2026-08-25 12:22:54 | 0.47 | 12.03 | 1,164 | None/None | none | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| B-rotate-t2-r1 | multi-core | 2026-08-25 12:23:52 | 0.48 | 11.78 | 1,189 | 1/3 | insert=7 | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| B-rotate-t2-r2 | single-core | 2026-08-25 12:26:17 | 0.48 | 11.79 | 1,188 | None/None | none | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| B-rotate-t2-r2 | multi-core | 2026-08-25 12:26:35 | 0.49 | 11.93 | 1,174 | 1/3 | insert=7 | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| B-rotate-t2-r3 | single-core | 2026-08-25 12:29:25 | 0.48 | 11.54 | 1,214 | None/None | none | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| B-rotate-t2-r3 | multi-core | 2026-08-25 12:29:53 | 0.46 | 11.99 | 1,168 | 1/4 | insert=7 | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |

ratios: 1.021, 0.988, 0.962 mean 0.990; single mean 1,189 (1,164-1,214); multi mean 1,177 (1,168-1,189)

**insert** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 12,003 | 848 | 2,358.9 | 1,057 | 2,079 | 2,153 | 2,266 | 2,648 | 3,533 | 6,260 | 38,483 |
| multi-core | 12,003 | 852 | 2,347.3 | 981 | 2,044 | 2,121 | 2,247 | 2,671 | 3,458 | 6,125 | 117,204 |

**point-select** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 12,000 | 46,101 | 39.0 | 22 | 26 | 26 | 27 | 43 | 56 | 89 | 3,151 |
| multi-core | 12,000 | 55,462 | 32.6 | 21 | 24 | 25 | 26 | 35 | 41 | 64 | 3,347 |

**update** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 12,000 | 861 | 2,319.3 | 987 | 2,068 | 2,146 | 2,258 | 2,597 | 3,312 | 5,836 | 24,928 |
| multi-core | 12,000 | 831 | 2,401.8 | 983 | 2,060 | 2,139 | 2,279 | 2,802 | 3,673 | 6,604 | 99,969 |

**delete** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 6,000 | 856 | 2,326.1 | 995 | 2,041 | 2,115 | 2,241 | 2,666 | 3,416 | 6,044 | 33,898 |
| multi-core | 6,000 | 860 | 2,320.5 | 1,008 | 2,029 | 2,117 | 2,268 | 2,770 | 3,513 | 6,057 | 24,651 |

**scan** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 6 | 1,514 | 800.1 | 265 | 280 | 288 | 1,287 | 1,395 | 1,395 | 1,395 | 1,395 |
| multi-core | 6 | 1,190 | 1,107.5 | 262 | 271 | 277 | 1,376 | 3,099 | 3,099 | 3,099 | 3,099 |

**per run p50 / p99**

| run | config | insert | point-select | update | delete |
|---|---|---:|---:|---:|---:|
| B-rotate-t2-r1 | single-core | 2,177 / 6,644 | 27 / 108 | 2,205 / 6,497 | 2,162 / 6,000 |
| B-rotate-t2-r1 | multi-core | 2,090 / 7,038 | 24 / 46 | 2,097 / 7,821 | 2,052 / 6,057 |
| B-rotate-t2-r2 | single-core | 2,133 / 6,570 | 26 / 1,034 | 2,118 / 5,947 | 2,098 / 7,920 |
| B-rotate-t2-r2 | multi-core | 2,155 / 5,935 | 25 / 71 | 2,157 / 5,062 | 2,098 / 4,739 |
| B-rotate-t2-r3 | single-core | 2,144 / 5,451 | 26 / 48 | 2,115 / 4,946 | 2,087 / 5,875 |
| B-rotate-t2-r3 | multi-core | 2,114 / 5,029 | 25 / 70 | 2,170 / 7,059 | 2,217 / 6,373 |

**first INSERT per relation (multi-core), µs; refills per run**

- B-rotate-t2-r1: owners 1,1; first 3,667 · 4,093; second 1,060 · 3,073; max 22,206 · 20,508
  refills: core 1: rowid 2/2 wait_max=1.0ms (submit 0.0ms/0it, to-grant 1.0ms/2177it, resume 0.0ms/1it), trxid 3/3 wait_max=30.3ms (submit 0.0ms/0it, to-grant 30.3ms/83064it, resume 2.0ms/1it), extent 1/1 wait_max=5.6ms (submit 0.0ms/0it, to-grant 4.6ms/3it, resume 1.0ms/1it)
  single-core first 1,181 · 1,197
- B-rotate-t2-r2: owners 1,1; first 3,680 · 7,575; second 3,285 · 2,346; max 117,204 · 117,176
  refills: core 1: rowid 2/2 wait_max=1.2ms (submit 0.0ms/0it, to-grant 1.2ms/2107it, resume 0.0ms/1it), trxid 3/3 wait_max=33.0ms (submit 0.0ms/0it, to-grant 33.0ms/91547it, resume 3.3ms/1it), extent 1/1 wait_max=4.4ms (submit 0.0ms/0it, to-grant 3.4ms/2it, resume 1.1ms/1it)
  single-core first 1,287 · 1,436
- B-rotate-t2-r3: owners 1,1; first 3,791 · 5,409; second 1,208 · 2,578; max 16,354 · 14,341
  refills: core 1: rowid 2/2 wait_max=1.1ms (submit 0.0ms/0it, to-grant 1.1ms/2047it, resume 0.0ms/1it), trxid 3/3 wait_max=22.9ms (submit 0.0ms/0it, to-grant 22.8ms/61850it, resume 2.0ms/1it), extent 1/1 wait_max=4.4ms (submit 0.0ms/0it, to-grant 3.3ms/2it, resume 1.1ms/1it)
  single-core first 1,149 · 1,104

### C-rotate-t4 (3 runs)

| run | config | started UTC | load at start | wall s | stmt/s | ddl/writer connects | retries | errors | verify |
|---|---|---|---:|---:|---:|---|---|---:|---|
| C-rotate-t4-r1 | single-core | 2026-08-25 12:24:27 | 0.46 | 12.14 | 2,307 | None/None | none | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| C-rotate-t4-r1 | multi-core | 2026-08-25 12:24:50 | 0.49 | 11.83 | 2,367 | 1/7 | insert=18 | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| C-rotate-t4-r2 | single-core | 2026-08-25 12:27:35 | 0.48 | 12.25 | 2,286 | None/None | none | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| C-rotate-t4-r2 | multi-core | 2026-08-25 12:28:04 | 0.46 | 12.16 | 2,303 | 1/4 | insert=18 | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| C-rotate-t4-r3 | single-core | 2026-08-25 12:30:08 | 0.50 | 12.65 | 2,214 | None/None | none | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |
| C-rotate-t4-r3 | multi-core | 2026-08-25 12:30:31 | 0.49 | 11.97 | 2,340 | 1/4 | insert=20 | 0 | survivors as expected (1000, and one more in bench0 for the probe row) |

ratios: 1.026, 1.007, 1.057 mean 1.030; single mean 2,269 (2,214-2,307); multi mean 2,337 (2,303-2,367)

**insert** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 24,003 | 1,646 | 2,433.0 | 1,090 | 2,134 | 2,220 | 2,388 | 2,858 | 3,563 | 6,025 | 31,345 |
| multi-core | 24,003 | 1,697 | 2,355.6 | 1,005 | 2,100 | 2,177 | 2,307 | 2,698 | 3,378 | 5,168 | 25,758 |

**point-select** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 24,000 | 39,853 | 84.9 | 23 | 47 | 51 | 59 | 100 | 120 | 1,170 | 6,885 |
| multi-core | 24,000 | 41,562 | 75.8 | 18 | 47 | 48 | 51 | 70 | 91 | 1,122 | 10,263 |

**update** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 24,000 | 1,630 | 2,452.1 | 1,064 | 2,138 | 2,228 | 2,377 | 2,917 | 3,763 | 6,584 | 16,173 |
| multi-core | 24,000 | 1,685 | 2,367.2 | 1,038 | 2,087 | 2,160 | 2,268 | 2,711 | 3,562 | 6,298 | 24,171 |

**delete** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 12,000 | 1,697 | 2,353.7 | 1,020 | 2,065 | 2,138 | 2,248 | 2,640 | 3,510 | 6,606 | 21,363 |
| multi-core | 12,000 | 1,700 | 2,344.9 | 1,006 | 2,071 | 2,140 | 2,238 | 2,684 | 3,592 | 6,080 | 17,997 |

**scan** pooled, µs

| config | n | rate/s (derived, mean of runs) | mean | p0 | p25 | p50 | p75 | p90 | p95 | p99 | p100 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| single-core | 12 | 2,996 | 1,056.5 | 261 | 485 | 1,246 | 1,309 | 1,348 | 1,422 | 1,422 | 1,422 |
| multi-core | 12 | 3,034 | 1,009.1 | 259 | 274 | 1,251 | 1,272 | 1,292 | 1,379 | 1,379 | 1,379 |

**per run p50 / p99**

| run | config | insert | point-select | update | delete |
|---|---|---:|---:|---:|---:|
| C-rotate-t4-r1 | single-core | 2,192 / 5,257 | 51 / 1,192 | 2,163 / 6,702 | 2,110 / 7,110 |
| C-rotate-t4-r1 | multi-core | 2,143 / 4,518 | 49 / 1,094 | 2,120 / 6,906 | 2,115 / 5,974 |
| C-rotate-t4-r2 | single-core | 2,175 / 6,023 | 50 / 1,043 | 2,246 / 7,278 | 2,149 / 6,982 |
| C-rotate-t4-r2 | multi-core | 2,182 / 5,958 | 48 / 1,116 | 2,188 / 6,577 | 2,165 / 7,030 |
| C-rotate-t4-r3 | single-core | 2,321 / 6,411 | 50 / 1,210 | 2,278 / 5,442 | 2,149 / 5,773 |
| C-rotate-t4-r3 | multi-core | 2,202 / 5,812 | 48 / 1,170 | 2,165 / 5,113 | 2,138 / 5,744 |

**first INSERT per relation (multi-core), µs; refills per run**

- C-rotate-t4-r1: owners 1,1,1,1; first 2,804 · 4,486 · 7,243 · 10,528; second 1,220 · 2,787 · 2,143 · 2,049; max 25,750 · 25,693 · 25,739 · 25,758
  refills: core 1: rowid 4/4 wait_max=3.0ms (submit 0.0ms/0it, to-grant 1.2ms/802it, resume 1.8ms/1it), trxid 6/6 wait_max=8.4ms (submit 0.0ms/0it, to-grant 8.2ms/20499it, resume 1.9ms/1it), extent 2/2 wait_max=5.8ms (submit 0.0ms/0it, to-grant 4.0ms/2it, resume 2.6ms/1it)
  single-core first 1,250 · 1,267 · 1,233 · 1,238
- C-rotate-t4-r2: owners 1,1,1,1; first 3,040 · 5,643 · 7,726 · 11,071; second 1,337 · 2,112 · 2,257 · 2,031; max 17,107 · 17,079 · 17,228 · 17,120
  refills: core 1: rowid 4/4 wait_max=3.1ms (submit 0.0ms/0it, to-grant 2.1ms/889it, resume 1.1ms/1it), trxid 6/6 wait_max=7.4ms (submit 0.0ms/0it, to-grant 5.8ms/13531it, resume 2.6ms/1it), extent 2/2 wait_max=4.9ms (submit 0.0ms/0it, to-grant 3.7ms/2it, resume 1.7ms/1it)
  single-core first 1,217 · 2,236 · 2,260 · 1,162
- C-rotate-t4-r3: owners 1,1,1,1; first 2,792 · 4,318 · 7,650 · 10,663; second 1,188 · 2,255 · 2,024 · 1,994; max 10,672 · 11,155 · 10,670 · 10,673
  refills: core 1: rowid 4/4 wait_max=3.3ms (submit 0.0ms/0it, to-grant 2.3ms/792it, resume 1.0ms/1it), trxid 6/6 wait_max=26.5ms (submit 0.0ms/0it, to-grant 26.5ms/72660it, resume 1.4ms/1it), extent 2/2 wait_max=5.6ms (submit 0.0ms/0it, to-grant 4.1ms/3it, resume 2.0ms/1it)
  single-core first 1,226 · 2,348 · 2,309 · 1,167
