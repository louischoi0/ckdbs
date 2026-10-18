GATE
| Cell | Rows | `planned` (P:A) | written A | written B | B saves vs planned, per pair | min over 3 pairs | Control W2 vs W:A | Gate (>= 15%) |
|---|---|---|---|---|---|---|---|---|
| c1-compare-all-r200 | 200 | 152.2 us | 148.2 us | 119.3 us | 21.7%, 21.6%, 21.7% | 21.6% | +0.1% | PASS |
| c1-compare-all-r1k | 1,000 | 640.0 us | 626.3 us | 418.3 us | 35.7%, 34.5%, 34.7% | 34.5% | +2.5% | PASS |
| c1-compare-all-r2872 | 2,872 (driver default) | 1,756.7 us | 1,740.9 us | 1,084.0 us | 38.1%, 38.3%, 38.2% | 38.1% | +0.7% | PASS |
| c1-compare-all-r10k | 10,000 | 5,757.8 us | 5,767.9 us | 3,612.6 us | 37.3%, 37.3%, 38.6% | 37.3% | +0.7% | PASS |
| c2-customer-statement-n200 | 180 freights / 200 cargos | 136.1 us | 131.2 us | 74.0 us | 46.1%, 45.5%, 41.7% | 41.7% | +0.8% | PASS |
| c2-customer-statement-n1k | 879 / 1,000 | 490.3 us | 489.6 us | 182.2 us | 62.8%, 62.8%, 60.7% | 60.7% | -0.1% | PASS |
| c2-customer-statement-ca10k | 8,234 / 10,000 (CA size) | 4,307.1 us | 4,322.6 us | 2,325.1 us | 46.2%, 32.2%, 46.2% | 32.2% | +0.1% | PASS |
| c2-customer-statement-df200k | 10,795 / 200,000 (defaults) | 7,262.4 us | 7,297.9 us | 7,094.6 us | 5.3%, 0.7%, 2.3% | 0.7% | -0.2% | FAIL |

MATRIX
| Cell | Rows | Arm | Throughput (median of 3) | Server CPU per statement (median) | Delta CPU vs written A | Round spread of CPU | Ops per round |
|---|---|---|---|---|---|---|---|
| c1-compare-all-r200 | 200 | P:A | 3,021 stmt/s | 152.2 us | +2.7% | 1.4% | 3,000 |
| c1-compare-all-r200 | 200 | P:B | 3,011 stmt/s | 152.3 us | +2.8% | 1.7% | 3,000 |
| c1-compare-all-r200 | 200 | W:A | 3,020 stmt/s | 148.2 us | - | 2.6% | 3,000 |
| c1-compare-all-r200 | 200 | W:B | 3,331 stmt/s | 119.3 us | -19.5% | 1.5% | 3,000 |
| c1-compare-all-r200 | 200 | W2:A | 3,041 stmt/s | 148.3 us | +0.1% | 1.3% | 3,000 |
| c1-compare-all-r1k | 1,000 | P:A | 682 stmt/s | 640.0 us | +2.2% | 2.2% | 810 |
| c1-compare-all-r1k | 1,000 | P:B | 678 stmt/s | 641.9 us | +2.5% | 4.9% | 810 |
| c1-compare-all-r1k | 1,000 | W:A | 689 stmt/s | 626.3 us | - | 3.6% | 810 |
| c1-compare-all-r1k | 1,000 | W:B | 809 stmt/s | 418.3 us | -33.2% | 3.4% | 810 |
| c1-compare-all-r1k | 1,000 | W2:A | 674 stmt/s | 642.0 us | +2.5% | 3.8% | 810 |
| c1-compare-all-r2872 | 2,872 (driver default) | P:A | 243 stmt/s | 1,756.7 us | +0.9% | 1.8% | 299 |
| c1-compare-all-r2872 | 2,872 (driver default) | P:B | 243 stmt/s | 1,747.7 us | +0.4% | 2.5% | 299 |
| c1-compare-all-r2872 | 2,872 (driver default) | W:A | 245 stmt/s | 1,740.9 us | - | 2.2% | 299 |
| c1-compare-all-r2872 | 2,872 (driver default) | W:B | 289 stmt/s | 1,084.0 us | -37.7% | 1.5% | 299 |
| c1-compare-all-r2872 | 2,872 (driver default) | W2:A | 242 stmt/s | 1,753.8 us | +0.7% | 1.7% | 299 |
| c1-compare-all-r10k | 10,000 | P:A | 71 stmt/s | 5,757.8 us | -0.2% | 4.1% | 89 |
| c1-compare-all-r10k | 10,000 | P:B | 71 stmt/s | 5,793.1 us | +0.4% | 4.5% | 89 |
| c1-compare-all-r10k | 10,000 | W:A | 71 stmt/s | 5,767.9 us | - | 2.3% | 89 |
| c1-compare-all-r10k | 10,000 | W:B | 83 stmt/s | 3,612.6 us | -37.4% | 1.9% | 89 |
| c1-compare-all-r10k | 10,000 | W2:A | 71 stmt/s | 5,806.1 us | +0.7% | 2.5% | 89 |
| c2-customer-statement-n200 | 180 freights / 200 cargos | P:A | 6,336 stmt/s | 136.1 us | +3.7% | 11.1% | 3,000 |
| c2-customer-statement-n200 | 180 freights / 200 cargos | P:B | 6,232 stmt/s | 137.6 us | +4.8% | 11.2% | 3,000 |
| c2-customer-statement-n200 | 180 freights / 200 cargos | W:A | 6,541 stmt/s | 131.2 us | - | 1.4% | 3,000 |
| c2-customer-statement-n200 | 180 freights / 200 cargos | W:B | 10,520 stmt/s | 74.0 us | -43.6% | 19.6% | 3,000 |
| c2-customer-statement-n200 | 180 freights / 200 cargos | W2:A | 6,445 stmt/s | 132.3 us | +0.8% | 10.8% | 3,000 |
| c2-customer-statement-n1k | 879 / 1,000 | P:A | 1,936 stmt/s | 490.3 us | +0.1% | 1.3% | 2,224 |
| c2-customer-statement-n1k | 879 / 1,000 | P:B | 1,932 stmt/s | 490.2 us | +0.1% | 0.8% | 2,224 |
| c2-customer-statement-n1k | 879 / 1,000 | W:A | 1,934 stmt/s | 489.6 us | - | 2.0% | 2,224 |
| c2-customer-statement-n1k | 879 / 1,000 | W:B | 4,881 stmt/s | 182.2 us | -62.8% | 7.1% | 2,224 |
| c2-customer-statement-n1k | 879 / 1,000 | W2:A | 1,937 stmt/s | 489.1 us | -0.1% | 2.6% | 2,224 |
| c2-customer-statement-ca10k | 8,234 / 10,000 (CA size) | P:A | 228 stmt/s | 4,307.1 us | -0.4% | 2.7% | 280 |
| c2-customer-statement-ca10k | 8,234 / 10,000 (CA size) | P:B | 230 stmt/s | 4,263.7 us | -1.4% | 2.2% | 280 |
| c2-customer-statement-ca10k | 8,234 / 10,000 (CA size) | W:A | 227 stmt/s | 4,322.6 us | - | 1.9% | 280 |
| c2-customer-statement-ca10k | 8,234 / 10,000 (CA size) | W:B | 422 stmt/s | 2,325.1 us | -46.2% | 28.2% | 280 |
| c2-customer-statement-ca10k | 8,234 / 10,000 (CA size) | W2:A | 227 stmt/s | 4,328.1 us | +0.1% | 1.7% | 280 |
| c2-customer-statement-df200k | 10,795 / 200,000 (defaults) | P:A | 136 stmt/s | 7,262.4 us | -0.5% | 2.4% | 154 |
| c2-customer-statement-df200k | 10,795 / 200,000 (defaults) | P:B | 135 stmt/s | 7,295.6 us | -0.0% | 1.3% | 154 |
| c2-customer-statement-df200k | 10,795 / 200,000 (defaults) | W:A | 135 stmt/s | 7,297.9 us | - | 1.6% | 154 |
| c2-customer-statement-df200k | 10,795 / 200,000 (defaults) | W:B | 139 stmt/s | 7,094.6 us | -2.8% | 2.4% | 154 |
| c2-customer-statement-df200k | 10,795 / 200,000 (defaults) | W2:A | 136 stmt/s | 7,281.4 us | -0.2% | 3.3% | 154 |
| c3-cross-join-x200 | 200 stats rows | P:A | 8,524 stmt/s | 86.9 us | +32.0% | 2.7% | 3,000 |
| c3-cross-join-x200 | 200 stats rows | W:A | 10,384 stmt/s | 65.8 us | - | 1.3% | 3,000 |
| c3-cross-join-x200 | 200 stats rows | W2:A | 10,500 stmt/s | 65.1 us | -1.1% | 24.5% | 3,000 |
| c3-cross-join-x1k | 1,000 | P:A | 7,110 stmt/s | 109.6 us | +24.6% | 0.9% | 3,000 |
| c3-cross-join-x1k | 1,000 | W:A | 8,458 stmt/s | 88.0 us | - | 0.7% | 3,000 |
| c3-cross-join-x1k | 1,000 | W2:A | 8,517 stmt/s | 86.6 us | -1.5% | 0.1% | 3,000 |
| c3-cross-join-x10k | 10,000 | P:A | 2,597 stmt/s | 349.5 us | +5.3% | 2.0% | 3,000 |
| c3-cross-join-x10k | 10,000 | W:A | 2,709 stmt/s | 332.0 us | - | 4.1% | 3,000 |
| c3-cross-join-x10k | 10,000 | W2:A | 2,707 stmt/s | 332.3 us | +0.1% | 2.7% | 3,000 |
| c3-cross-join-x60k | 60,480 (driver default 30 y) | P:A | 589 stmt/s | 1,634.6 us | +1.7% | 1.7% | 725 |
| c3-cross-join-x60k | 60,480 (driver default 30 y) | W:A | 598 stmt/s | 1,607.4 us | - | 0.5% | 725 |
| c3-cross-join-x60k | 60,480 (driver default 30 y) | W2:A | 595 stmt/s | 1,614.4 us | +0.4% | 1.6% | 725 |

LATENCY
| Cell | Arm | Statements | p0 | p25 | p50 | p95 | p99 | Server CPU / p50 |
|---|---|---|---|---|---|---|---|---|
| c1-compare-all-r200 | P:A | 9,000 | 311 us | 322 us | 328 us | 352 us | 400 us | 46% |
| c1-compare-all-r200 | W:A | 9,000 | 308 us | 320 us | 327 us | 363 us | 416 us | 45% |
| c1-compare-all-r200 | W:B | 9,000 | 279 us | 292 us | 297 us | 322 us | 396 us | 40% |
| c1-compare-all-r200 | W2:A | 9,000 | 308 us | 320 us | 325 us | 354 us | 421 us | 46% |
| c1-compare-all-r1k | P:A | 2,430 | 1,407 us | 1,451 us | 1,473 us | 1,578 us | 1,646 us | 43% |
| c1-compare-all-r1k | W:A | 2,430 | 1,406 us | 1,436 us | 1,453 us | 1,573 us | 1,619 us | 43% |
| c1-compare-all-r1k | W:B | 2,430 | 1,194 us | 1,222 us | 1,241 us | 1,346 us | 1,407 us | 34% |
| c1-compare-all-r1k | W2:A | 2,430 | 1,415 us | 1,463 us | 1,485 us | 1,544 us | 1,683 us | 43% |
| c1-compare-all-r2872 | P:A | 897 | 3,957 us | 4,049 us | 4,103 us | 4,336 us | 4,736 us | 43% |
| c1-compare-all-r2872 | W:A | 897 | 3,966 us | 4,034 us | 4,083 us | 4,295 us | 4,678 us | 43% |
| c1-compare-all-r2872 | W:B | 897 | 3,336 us | 3,422 us | 3,473 us | 3,620 us | 3,685 us | 31% |
| c1-compare-all-r2872 | W2:A | 897 | 3,975 us | 4,066 us | 4,117 us | 4,284 us | 4,501 us | 43% |
| c1-compare-all-r10k | P:A | 267 | 13,422 us | 13,711 us | 14,048 us | 14,760 us | 14,945 us | 41% |
| c1-compare-all-r10k | W:A | 267 | 13,476 us | 13,833 us | 14,111 us | 14,607 us | 15,200 us | 41% |
| c1-compare-all-r10k | W:B | 267 | 11,364 us | 11,758 us | 11,978 us | 12,518 us | 13,465 us | 30% |
| c1-compare-all-r10k | W2:A | 267 | 13,589 us | 13,794 us | 14,104 us | 14,499 us | 14,635 us | 41% |
| c2-customer-statement-n200 | P:A | 9,000 | 131 us | 151 us | 165 us | 197 us | 211 us | 82% |
| c2-customer-statement-n200 | W:A | 9,000 | 132 us | 144 us | 147 us | 175 us | 195 us | 89% |
| c2-customer-statement-n200 | W:B | 9,000 | 77 us | 90 us | 97 us | 132 us | 141 us | 77% |
| c2-customer-statement-n200 | W2:A | 9,000 | 131 us | 145 us | 160 us | 193 us | 212 us | 83% |
| c2-customer-statement-n1k | P:A | 6,672 | 476 us | 507 us | 516 us | 557 us | 631 us | 95% |
| c2-customer-statement-n1k | W:A | 6,672 | 476 us | 502 us | 511 us | 552 us | 694 us | 96% |
| c2-customer-statement-n1k | W:B | 6,672 | 179 us | 195 us | 208 us | 247 us | 275 us | 88% |
| c2-customer-statement-n1k | W2:A | 6,672 | 472 us | 507 us | 518 us | 556 us | 595 us | 94% |
| c2-customer-statement-ca10k | P:A | 840 | 4,166 us | 4,227 us | 4,317 us | 4,577 us | 4,991 us | 100% |
| c2-customer-statement-ca10k | W:A | 840 | 4,172 us | 4,237 us | 4,353 us | 4,580 us | 5,773 us | 99% |
| c2-customer-statement-ca10k | W:B | 840 | 2,272 us | 2,311 us | 2,364 us | 2,979 us | 3,085 us | 98% |
| c2-customer-statement-ca10k | W2:A | 840 | 4,190 us | 4,259 us | 4,394 us | 4,578 us | 4,955 us | 98% |
| c2-customer-statement-df200k | P:A | 462 | 7,162 us | 7,259 us | 7,338 us | 7,794 us | 8,353 us | 99% |
| c2-customer-statement-df200k | W:A | 462 | 7,228 us | 7,318 us | 7,364 us | 7,724 us | 9,239 us | 99% |
| c2-customer-statement-df200k | W:B | 462 | 6,970 us | 7,091 us | 7,148 us | 7,439 us | 8,102 us | 99% |
| c2-customer-statement-df200k | W2:A | 462 | 7,195 us | 7,295 us | 7,370 us | 7,806 us | 8,459 us | 99% |
| c3-cross-join-x200 | P:A | 9,000 | 107 us | 111 us | 113 us | 140 us | 158 us | 77% |
| c3-cross-join-x200 | W:A | 9,000 | 89 us | 91 us | 92 us | 114 us | 128 us | 71% |
| c3-cross-join-x200 | W2:A | 9,000 | 84 us | 90 us | 97 us | 135 us | 145 us | 67% |
| c3-cross-join-x1k | P:A | 9,000 | 129 us | 133 us | 136 us | 160 us | 173 us | 80% |
| c3-cross-join-x1k | W:A | 9,000 | 109 us | 112 us | 115 us | 136 us | 148 us | 77% |
| c3-cross-join-x1k | W2:A | 9,000 | 106 us | 112 us | 114 us | 132 us | 143 us | 76% |
| c3-cross-join-x10k | P:A | 9,000 | 357 us | 376 us | 385 us | 425 us | 463 us | 91% |
| c3-cross-join-x10k | W:A | 9,000 | 336 us | 356 us | 371 us | 411 us | 486 us | 90% |
| c3-cross-join-x10k | W2:A | 9,000 | 335 us | 350 us | 362 us | 402 us | 481 us | 92% |
| c3-cross-join-x60k | P:A | 2,175 | 1,618 us | 1,656 us | 1,688 us | 1,733 us | 1,845 us | 97% |
| c3-cross-join-x60k | W:A | 2,175 | 1,592 us | 1,641 us | 1,663 us | 1,707 us | 1,859 us | 97% |
| c3-cross-join-x60k | W2:A | 2,175 | 1,600 us | 1,642 us | 1,666 us | 1,710 us | 1,809 us | 97% |
