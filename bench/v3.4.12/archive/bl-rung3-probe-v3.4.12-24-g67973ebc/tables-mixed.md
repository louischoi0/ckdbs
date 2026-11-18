
#### reader schedule latency, per run
| arm | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 4006 | 224 µs | 299 µs | 317 µs | 394 µs | 1.3 ms | 4.4 ms | 5.6 ms |
| isolated | r2 | 4006 | 189 µs | 295 µs | 315 µs | 385 µs | 476 µs | 3.5 ms | 4.5 ms |
| isolated | r3 | 4006 | 197 µs | 296 µs | 316 µs | 381 µs | 494 µs | 3.6 ms | 5.0 ms |
| isolated | r4 | 4006 | 207 µs | 316 µs | 337 µs | 414 µs | 963 µs | 4.1 ms | 5.5 ms |
| isolated | r5 | 4006 | 234 µs | 314 µs | 336 µs | 402 µs | 649 µs | 3.2 ms | 4.2 ms |
| nwonly | r1 | 4006 | 218 µs | 281 µs | 306 µs | 394 µs | 1.1 ms | 4.1 ms | 5.5 ms |
| nwonly | r2 | 4006 | 195 µs | 268 µs | 296 µs | 373 µs | 444 µs | 3.4 ms | 5.2 ms |
| nwonly | r3 | 4006 | 214 µs | 305 µs | 328 µs | 408 µs | 591 µs | 3.0 ms | 5.3 ms |
| nwonly | r4 | 4006 | 218 µs | 300 µs | 327 µs | 412 µs | 1.4 ms | 4.3 ms | 5.2 ms |
| nwonly | r5 | 4006 | 200 µs | 306 µs | 332 µs | 422 µs | 1.4 ms | 4.4 ms | 5.3 ms |
| levels | r1 | 4006 | 206 µs | 295 µs | 319 µs | 416 µs | 3.7 ms | 255.3 ms | 314.7 ms |
| levels | r2 | 4006 | 199 µs | 292 µs | 316 µs | 424 µs | 15.0 ms | 282.8 ms | 312.6 ms |
| levels | r3 | 4006 | 163 µs | 303 µs | 328 µs | 436 µs | 12.2 ms | 104.1 ms | 118.8 ms |
| levels | r4 | 4006 | 208 µs | 311 µs | 336 µs | 464 µs | 72.2 ms | 288.5 ms | 318.1 ms |
| levels | r5 | 4006 | 228 µs | 323 µs | 349 µs | 465 µs | 3.5 ms | 92.8 ms | 151.6 ms |
| probe | r1 | 4006 | 175 µs | 281 µs | 308 µs | 412 µs | 609 µs | 3.8 ms | 5.4 ms |
| probe | r2 | 4006 | 194 µs | 268 µs | 295 µs | 393 µs | 1.3 ms | 4.9 ms | 7.0 ms |
| probe | r3 | 4006 | 198 µs | 292 µs | 320 µs | 425 µs | 1.0 ms | 4.2 ms | 8.4 ms |
| probe | r4 | 4006 | 200 µs | 292 µs | 326 µs | 489 µs | 2.6 ms | 7.6 ms | 15.0 ms |
| probe | r5 | 4006 | 184 µs | 281 µs | 312 µs | 420 µs | 1.4 ms | 27.0 ms | 44.2 ms |

#### reader schedule latency, median of runs
| arm | runs | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| isolated | 5 | 207 µs | 299 µs | 317 µs | 394 µs | 649 µs | 3.6 ms | 5.0 ms |
| nwonly | 5 | 214 µs | 300 µs | 327 µs | 408 µs | 1.1 ms | 4.1 ms | 5.3 ms |
| levels | 5 | 206 µs | 303 µs | 328 µs | 436 µs | 12.2 ms | 255.3 ms | 312.6 ms |
| probe | 5 | 194 µs | 281 µs | 312 µs | 420 µs | 1.3 ms | 4.9 ms | 8.4 ms |

#### reader service latency, per run
| arm | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 4006 | 154 µs | 211 µs | 223 µs | 284 µs | 425 µs | 3.2 ms | 5.3 ms |
| isolated | r2 | 4006 | 118 µs | 206 µs | 222 µs | 283 µs | 324 µs | 912 µs | 3.8 ms |
| isolated | r3 | 4006 | 132 µs | 207 µs | 222 µs | 278 µs | 320 µs | 2.3 ms | 4.6 ms |
| isolated | r4 | 4006 | 128 µs | 222 µs | 238 µs | 305 µs | 364 µs | 1.5 ms | 4.0 ms |
| isolated | r5 | 4006 | 161 µs | 223 µs | 236 µs | 295 µs | 326 µs | 814 µs | 2.5 ms |
| nwonly | r1 | 4006 | 142 µs | 191 µs | 212 µs | 277 µs | 366 µs | 2.1 ms | 5.5 ms |
| nwonly | r2 | 4006 | 124 µs | 180 µs | 205 µs | 271 µs | 311 µs | 2.3 ms | 4.6 ms |
| nwonly | r3 | 4006 | 134 µs | 213 µs | 230 µs | 296 µs | 353 µs | 864 µs | 2.9 ms |
| nwonly | r4 | 4006 | 140 µs | 212 µs | 233 µs | 301 µs | 693 µs | 3.6 ms | 5.1 ms |
| nwonly | r5 | 4006 | 135 µs | 216 µs | 236 µs | 309 µs | 604 µs | 3.8 ms | 5.1 ms |
| levels | r1 | 4006 | 63 µs | 207 µs | 225 µs | 294 µs | 605 µs | 4.0 ms | 314.6 ms |
| levels | r2 | 4006 | 58 µs | 206 µs | 223 µs | 296 µs | 611 µs | 5.9 ms | 312.5 ms |
| levels | r3 | 4006 | 72 µs | 214 µs | 230 µs | 303 µs | 945 µs | 88.5 ms | 118.7 ms |
| levels | r4 | 4006 | 60 µs | 218 µs | 235 µs | 317 µs | 532 µs | 82.1 ms | 318.0 ms |
| levels | r5 | 4006 | 60 µs | 228 µs | 246 µs | 328 µs | 524 µs | 79.5 ms | 151.5 ms |
| probe | r1 | 4006 | 107 µs | 196 µs | 218 µs | 295 µs | 424 µs | 3.3 ms | 3.9 ms |
| probe | r2 | 4006 | 125 µs | 183 µs | 204 µs | 274 µs | 516 µs | 3.4 ms | 5.0 ms |
| probe | r3 | 4006 | 114 µs | 202 µs | 226 µs | 306 µs | 601 µs | 2.4 ms | 4.4 ms |
| probe | r4 | 4006 | 123 µs | 202 µs | 229 µs | 334 µs | 1.7 ms | 7.5 ms | 14.9 ms |
| probe | r5 | 4006 | 112 µs | 192 µs | 216 µs | 288 µs | 563 µs | 5.3 ms | 44.2 ms |

#### reader service latency, median of runs
| arm | runs | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|
| isolated | 5 | 132 µs | 211 µs | 223 µs | 284 µs | 326 µs | 1.5 ms | 4.0 ms |
| nwonly | 5 | 135 µs | 212 µs | 230 µs | 296 µs | 366 µs | 2.3 ms | 5.1 ms |
| levels | 5 | 60 µs | 214 µs | 230 µs | 303 µs | 605 µs | 79.5 ms | 312.5 ms |
| probe | 5 | 114 µs | 196 µs | 218 µs | 295 µs | 563 µs | 3.4 ms | 5.0 ms |

#### the `normal` walker against the readers (arms with one)
| arm | run | nwalker core (start->end) | readers sharing it at end | readers moved | slow samples (>=1 ms) on shared readers | on the others | slow samples per shared reader | per other reader | nwalker p50 / p99 | nwalker stmts |
|---|---|---|---|---|---|---|---|---|---|---|
| nwonly | r1 | 0->3 | 0 | 1 | 0 | 47 | 0.0 | 7.8 | 394 µs / 602 µs | 24,140 |
| nwonly | r2 | 0->3 | 0 | 1 | 0 | 22 | 0.0 | 3.7 | 392 µs / 543 µs | 24,439 |
| nwonly | r3 | 0->3 | 0 | 1 | 0 | 24 | 0.0 | 4.0 | 398 µs / 534 µs | 24,139 |
| nwonly | r4 | 2->2 | 0 | 1 | 0 | 60 | 0.0 | 10.0 | 400 µs / 658 µs | 23,757 |
| nwonly | r5 | 1->2 | 0 | 1 | 0 | 51 | 0.0 | 8.5 | 407 µs / 655 µs | 23,407 |
| probe | r1 | 1->2 | 0 | 5 | 0 | 24 | 0.0 | 4.0 | 690 µs / 888 µs | 15,027 |
| probe | r2 | 1->3 | 0 | 4 | 0 | 48 | 0.0 | 8.0 | 811 µs / 1.2 ms | 14,065 |
| probe | r3 | 2->1 | 0 | 3 | 0 | 41 | 0.0 | 6.8 | 699 µs / 1.1 ms | 14,567 |
| probe | r4 | 3->2 | 0 | 3 | 0 | 103 | 0.0 | 17.2 | 740 µs / 1.2 ms | 13,739 |
| probe | r5 | 3->1 | 0 | 3 | 0 | 47 | 0.0 | 7.8 | 816 µs / 1.1 ms | 13,895 |

#### each read core over the window
| arm | run | core | readers at end | nwalker | fg polls >10 ms | fg longest poll | sys polls >10 ms | sys longest poll | checkpoint runs | roll-blocked wait |
|---|---|---|---|---|---|---|---|---|---|---|
| isolated | r1 | 0 | 1 |  | 0 | 307 µs | 0 | 513 µs | 2 | 0 µs |
| isolated | r1 | 1 | 1 |  | 0 | 333 µs | 0 | 56 µs | 2 | 0 µs |
| isolated | r1 | 2 | 2 |  | 0 | 902 µs | 0 | 56 µs | 2 | 0 µs |
| isolated | r1 | 3 | 2 |  | 0 | 295 µs | 0 | 70 µs | 2 | 0 µs |
| isolated | r2 | 0 | 2 |  | 0 | 303 µs | 0 | 781 µs | 2 | 0 µs |
| isolated | r2 | 1 | 0 |  | 0 | 917 µs | 0 | 50 µs | 2 | 0 µs |
| isolated | r2 | 2 | 2 |  | 0 | 384 µs | 0 | 63 µs | 2 | 0 µs |
| isolated | r2 | 3 | 2 |  | 0 | 327 µs | 0 | 83 µs | 2 | 0 µs |
| isolated | r3 | 0 | 3 |  | 0 | 1.1 ms | 0 | 405 µs | 2 | 0 µs |
| isolated | r3 | 1 | 0 |  | 0 | 395 µs | 0 | 57 µs | 2 | 0 µs |
| isolated | r3 | 2 | 2 |  | 0 | 299 µs | 0 | 53 µs | 2 | 0 µs |
| isolated | r3 | 3 | 1 |  | 0 | 1.1 ms | 0 | 89 µs | 2 | 0 µs |
| isolated | r4 | 0 | 1 |  | 0 | 1.7 ms | 0 | 442 µs | 2 | 0 µs |
| isolated | r4 | 1 | 1 |  | 0 | 304 µs | 0 | 59 µs | 2 | 0 µs |
| isolated | r4 | 2 | 0 |  | 0 | 338 µs | 0 | 63 µs | 2 | 0 µs |
| isolated | r4 | 3 | 4 |  | 0 | 393 µs | 0 | 572 µs | 2 | 0 µs |
| isolated | r5 | 0 | 2 |  | 0 | 1.9 ms | 0 | 692 µs | 2 | 0 µs |
| isolated | r5 | 1 | 2 |  | 0 | 296 µs | 0 | 58 µs | 2 | 0 µs |
| isolated | r5 | 2 | 2 |  | 0 | 319 µs | 0 | 59 µs | 2 | 0 µs |
| isolated | r5 | 3 | 0 |  | 0 | 923 µs | 0 | 340 µs | 2 | 0 µs |
| nwonly | r1 | 0 | 3 |  | 0 | 1.6 ms | 0 | 540 µs | 2 | 0 µs |
| nwonly | r1 | 1 | 3 |  | 0 | 1.1 ms | 0 | 714 µs | 2 | 0 µs |
| nwonly | r1 | 2 | 0 |  | 0 | 313 µs | 0 | 50 µs | 2 | 0 µs |
| nwonly | r1 | 3 | 0 | yes | 0 | 366 µs | 0 | 598 µs | 2 | 0 µs |
| nwonly | r2 | 0 | 3 |  | 0 | 396 µs | 0 | 343 µs | 2 | 0 µs |
| nwonly | r2 | 1 | 3 |  | 0 | 909 µs | 0 | 57 µs | 2 | 0 µs |
| nwonly | r2 | 2 | 0 |  | 0 | 353 µs | 0 | 49 µs | 2 | 0 µs |
| nwonly | r2 | 3 | 0 | yes | 0 | 400 µs | 0 | 80 µs | 2 | 0 µs |
| nwonly | r3 | 0 | 3 |  | 0 | 367 µs | 0 | 425 µs | 2 | 0 µs |
| nwonly | r3 | 1 | 1 |  | 0 | 940 µs | 0 | 67 µs | 2 | 0 µs |
| nwonly | r3 | 2 | 2 |  | 0 | 300 µs | 0 | 55 µs | 2 | 0 µs |
| nwonly | r3 | 3 | 0 | yes | 0 | 320 µs | 0 | 82 µs | 2 | 0 µs |
| nwonly | r4 | 0 | 0 |  | 0 | 1.6 ms | 0 | 490 µs | 2 | 0 µs |
| nwonly | r4 | 1 | 3 |  | 0 | 317 µs | 0 | 63 µs | 2 | 0 µs |
| nwonly | r4 | 2 | 0 | yes | 0 | 301 µs | 0 | 49 µs | 2 | 0 µs |
| nwonly | r4 | 3 | 3 |  | 0 | 916 µs | 0 | 537 µs | 2 | 0 µs |
| nwonly | r5 | 0 | 1 |  | 0 | 1.8 ms | 0 | 651 µs | 2 | 0 µs |
| nwonly | r5 | 1 | 3 |  | 0 | 1.3 ms | 0 | 60 µs | 2 | 0 µs |
| nwonly | r5 | 2 | 0 | yes | 0 | 406 µs | 0 | 278 µs | 2 | 0 µs |
| nwonly | r5 | 3 | 2 |  | 0 | 305 µs | 0 | 70 µs | 2 | 0 µs |
| levels | r1 | 1 | 3 |  | 0 | 4.1 ms | 0 | 67 µs | 0 | 0 µs |
| levels | r1 | 2 | 3 |  | 0 | 372 µs | 1 | 322.1 ms | 1 | 321.8 ms |
| levels | r1 | 3 | 0 |  | 0 | 1.0 ms | 3 | 125.2 ms | 1 | 198.7 ms |
| levels | r2 | 1 | 2 |  | 0 | 3.3 ms | 0 | 2.7 ms | 1 | 2.1 ms |
| levels | r2 | 2 | 3 |  | 0 | 5.8 ms | 2 | 18.3 ms | 2 | 17.6 ms |
| levels | r2 | 3 | 1 |  | 0 | 3.9 ms | 1 | 315.6 ms | 2 | 319.4 ms |
| levels | r3 | 1 | 2 |  | 0 | 3.8 ms | 0 | 3.3 ms | 2 | 1.8 ms |
| levels | r3 | 2 | 1 |  | 0 | 3.8 ms | 0 | 5.4 ms | 1 | 3.7 ms |
| levels | r3 | 3 | 3 |  | 0 | 3.7 ms | 3 | 120.0 ms | 2 | 141.8 ms |
| levels | r4 | 1 | 3 |  | 0 | 3.8 ms | 0 | 58 µs | 0 | 0 µs |
| levels | r4 | 2 | 3 |  | 0 | 3.3 ms | 1 | 43.3 ms | 1 | 1.1 ms |
| levels | r4 | 3 | 0 |  | 0 | 314 µs | 2 | 322.0 ms | 1 | 409.2 ms |
| levels | r5 | 1 | 3 |  | 0 | 3.2 ms | 0 | 87 µs | 0 | 0 µs |
| levels | r5 | 2 | 2 |  | 0 | 1.2 ms | 0 | 16.6 ms | 1 | 2.2 ms |
| levels | r5 | 3 | 1 |  | 0 | 3.7 ms | 2 | 163.4 ms | 1 | 255.9 ms |
| probe | r1 | 1 | 5 |  | 0 | 3.7 ms | 0 | 3.7 ms | 2 | 0 µs |
| probe | r1 | 2 | 0 | yes | 0 | 6.2 ms | 1 | 66.6 ms | 2 | 68.5 ms |
| probe | r1 | 3 | 1 |  | 0 | 920 µs | 0 | 3.4 ms | 2 | 2.0 ms |
| probe | r2 | 1 | 2 |  | 0 | 5.2 ms | 0 | 5.1 ms | 2 | 2.5 ms |
| probe | r2 | 2 | 4 |  | 0 | 395 µs | 0 | 5.5 ms | 2 | 0 µs |
| probe | r2 | 3 | 0 | yes | 0 | 3.6 ms | 0 | 4.2 ms | 3 | 2.1 ms |
| probe | r3 | 1 | 0 | yes | 0 | 2.2 ms | 0 | 3.7 ms | 2 | 0 µs |
| probe | r3 | 2 | 1 |  | 0 | 5.8 ms | 0 | 2.9 ms | 2 | 2.8 ms |
| probe | r3 | 3 | 5 |  | 0 | 411 µs | 0 | 3.1 ms | 2 | 2.0 ms |
| probe | r4 | 1 | 2 |  | 0 | 1.9 ms | 1 | 27.5 ms | 2 | 0 µs |
| probe | r4 | 2 | 0 | yes | 0 | 8.4 ms | 1 | 25.8 ms | 2 | 14.4 ms |
| probe | r4 | 3 | 4 |  | 0 | 3.2 ms | 1 | 15.1 ms | 2 | 0 µs |
| probe | r5 | 1 | 0 | yes | 0 | 1.1 ms | 0 | 3.8 ms | 2 | 0 µs |
| probe | r5 | 2 | 4 |  | 0 | 454 µs | 0 | 20.8 ms | 2 | 0 µs |
| probe | r5 | 3 | 2 |  | 0 | 939 µs | 1 | 47.9 ms | 3 | 47.8 ms |

#### batch sessions' work and moves
| arm | run | cores at end (roles in order) | moves (since start) | moves in window | inserter rows/s | walker stmts | errors |
|---|---|---|---|---|---|---|---|
| isolated | r1 | 0, 1, 3, 2, 3, 2 | 0 | 0 | 0 | 0 | 0 |
| isolated | r2 | 2, 2, 3, 3, 0, 0 | 0 | 0 | 0 | 0 | 0 |
| isolated | r3 | 0, 3, 0, 2, 2, 0 | 1 | 0 | 0 | 0 | 0 |
| isolated | r4 | 3, 3, 1, 3, 3, 0 | 0 | 0 | 0 | 0 | 0 |
| isolated | r5 | 1, 0, 2, 0, 1, 2 | 0 | 0 | 0 | 0 | 0 |
| nwonly | r1 | 1, 1, 0, 0, 1, 0, 3 | 3 | 2 | 0 | 0 | 0 |
| nwonly | r2 | 1, 1, 1, 0, 0, 0, 3 | 3 | 2 | 0 | 0 | 0 |
| nwonly | r3 | 2, 0, 0, 2, 1, 0, 3 | 6 | 5 | 0 | 0 | 0 |
| nwonly | r4 | 3, 1, 3, 1, 1, 3, 2 | 4 | 4 | 0 | 0 | 0 |
| nwonly | r5 | 1, 0, 3, 3, 1, 1, 2 | 3 | 2 | 0 | 0 | 0 |
| levels | r1 | 1, 1, 2, 2, 1, 2, 0, 0 | 5 | 2 | 159,555 | 1,036 | 0 |
| levels | r2 | 1, 3, 2, 1, 2, 2, 0, 0 | 9 | 4 | 167,762 | 1,083 | 0 |
| levels | r3 | 3, 1, 1, 3, 2, 3, 0, 0 | 10 | 5 | 158,332 | 977 | 0 |
| levels | r4 | 2, 2, 2, 1, 1, 1, 0, 0 | 7 | 5 | 165,207 | 1,079 | 0 |
| levels | r5 | 1, 2, 1, 1, 3, 2, 0, 0 | 7 | 3 | 161,106 | 1,103 | 0 |
| probe | r1 | 3, 1, 1, 1, 1, 1, 0, 0, 2 | 10 | 4 | 139,599 | 662 | 0 |
| probe | r2 | 2, 2, 1, 1, 2, 2, 0, 0, 3 | 14 | 8 | 126,674 | 716 | 0 |
| probe | r3 | 3, 3, 3, 3, 2, 3, 0, 0, 1 | 16 | 10 | 134,725 | 692 | 0 |
| probe | r4 | 1, 3, 3, 3, 3, 1, 0, 0, 2 | 19 | 10 | 136,805 | 734 | 0 |
| probe | r5 | 2, 3, 2, 2, 2, 3, 0, 0, 1 | 17 | 10 | 128,878 | 729 | 0 |

#### forced sharing: reader schedule latency, per run and median
| arm | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| same-iso | r1 | 4006 | 207 µs | 309 µs | 331 µs | 406 µs | 758 µs | 5.1 ms | 7.5 ms |
| same-iso | r2 | 4006 | 203 µs | 307 µs | 329 µs | 395 µs | 426 µs | 1.5 ms | 4.0 ms |
| same-iso | r3 | 4006 | 223 µs | 309 µs | 330 µs | 399 µs | 506 µs | 2.7 ms | 6.2 ms |
| same-iso | r4 | 4006 | 191 µs | 308 µs | 332 µs | 404 µs | 505 µs | 2.9 ms | 3.8 ms |
| same-iso | r5 | 4006 | 212 µs | 308 µs | 331 µs | 402 µs | 536 µs | 2.2 ms | 5.3 ms |
| same-iso | median of 5 | | 207 µs | 308 µs | 331 µs | 402 µs | 506 µs | 2.7 ms | 5.3 ms |
| same-nw | r1 | 4006 | 159 µs | 277 µs | 316 µs | 434 µs | 1.6 ms | 4.1 ms | 5.2 ms |
| same-nw | r2 | 4006 | 169 µs | 275 µs | 317 µs | 424 µs | 1.3 ms | 3.9 ms | 6.2 ms |
| same-nw | r3 | 4006 | 166 µs | 272 µs | 313 µs | 412 µs | 535 µs | 1.9 ms | 4.0 ms |
| same-nw | r4 | 4006 | 164 µs | 275 µs | 314 µs | 403 µs | 477 µs | 1.7 ms | 4.2 ms |
| same-nw | r5 | 4006 | 164 µs | 271 µs | 312 µs | 413 µs | 490 µs | 1.8 ms | 6.1 ms |
| same-nw | median of 5 | | 164 µs | 275 µs | 314 µs | 413 µs | 535 µs | 1.9 ms | 5.2 ms |

#### forced sharing: reader service latency, per run and median
| arm | run | n | p0 | p25 | p50 | p95 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|---|---|---|
| same-iso | r1 | 4006 | 127 µs | 216 µs | 231 µs | 297 µs | 329 µs | 2.9 ms | 7.4 ms |
| same-iso | r2 | 4006 | 120 µs | 215 µs | 230 µs | 291 µs | 313 µs | 406 µs | 1.6 ms |
| same-iso | r3 | 4006 | 143 µs | 216 µs | 231 µs | 295 µs | 323 µs | 854 µs | 2.7 ms |
| same-iso | r4 | 4006 | 117 µs | 216 µs | 234 µs | 298 µs | 321 µs | 505 µs | 940 µs |
| same-iso | r5 | 4006 | 135 µs | 216 µs | 233 µs | 297 µs | 329 µs | 828 µs | 2.8 ms |
| same-iso | median of 5 | | 127 µs | 216 µs | 231 µs | 297 µs | 323 µs | 828 µs | 2.7 ms |
| same-nw | r1 | 4006 | 91 µs | 181 µs | 218 µs | 310 µs | 379 µs | 2.8 ms | 4.4 ms |
| same-nw | r2 | 4006 | 90 µs | 179 µs | 220 µs | 303 µs | 368 µs | 3.3 ms | 6.1 ms |
| same-nw | r3 | 4006 | 90 µs | 175 µs | 215 µs | 301 µs | 353 µs | 590 µs | 2.3 ms |
| same-nw | r4 | 4006 | 84 µs | 180 µs | 218 µs | 295 µs | 340 µs | 798 µs | 1.3 ms |
| same-nw | r5 | 4006 | 86 µs | 174 µs | 216 µs | 303 µs | 347 µs | 1.1 ms | 3.2 ms |
| same-nw | median of 5 | | 90 µs | 179 µs | 218 µs | 303 µs | 353 µs | 1.1 ms | 3.2 ms |

#### forced sharing: core 1 and the walker
| arm | run | cores at end | moves | core-1 fg longest poll | core-1 fg polls | nwalker p50 / p99 | nwalker stmts |
|---|---|---|---|---|---|---|---|
| same-iso | r1 | 1, 1, 1, 1, 1, 1 | 0 | 352 µs | 4,015 | - | 0 |
| same-iso | r2 | 1, 1, 1, 1, 1, 1 | 0 | 388 µs | 4,015 | - | 0 |
| same-iso | r3 | 1, 1, 1, 1, 1, 1 | 0 | 1.3 ms | 4,015 | - | 0 |
| same-iso | r4 | 1, 1, 1, 1, 1, 1 | 0 | 1.0 ms | 4,015 | - | 0 |
| same-iso | r5 | 1, 1, 1, 1, 1, 1 | 0 | 402 µs | 4,015 | - | 0 |
| same-nw | r1 | 1, 1, 1, 1, 1, 1, 1 | 0 | 1.4 ms | 122,481 | 399 µs / 646 µs | 23,695 |
| same-nw | r2 | 1, 1, 1, 1, 1, 1, 1 | 0 | 1.4 ms | 123,306 | 401 µs / 568 µs | 23,858 |
| same-nw | r3 | 1, 1, 1, 1, 1, 1, 1 | 0 | 909 µs | 123,468 | 401 µs / 596 µs | 23,892 |
| same-nw | r4 | 1, 1, 1, 1, 1, 1, 1 | 0 | 1.7 ms | 123,782 | 402 µs / 555 µs | 23,955 |
| same-nw | r5 | 1, 1, 1, 1, 1, 1, 1 | 0 | 338 µs | 123,696 | 402 µs / 548 µs | 23,938 |
