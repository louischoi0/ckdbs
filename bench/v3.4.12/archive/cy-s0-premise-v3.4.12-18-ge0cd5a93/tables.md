## Order, est and obs per cell (planner, blind vs informed)

| cell | blind order | blind line | informed order | informed line | obs (rows per step) |
|---|---|---|---|---|---|
| compare-all-200 | r-m | planned order=written gen=0 switch=1 est=0:100000,1:100000 | m-r | planned order=memo gen=2 switch=0.194685 est=1:8,0:181 | examined/matched: 8/8, 375/200 |
| compare-one-200 | m-r | planned order=planned gen=0 switch=0.8335 est=1:1,0:1000 | m-r | planned order=memo gen=4 switch=0.317282 est=1:1,0:23 | examined/matched: 1/1, 200/25 |
| customer-statement-200 | f-c | planned order=written gen=0 switch=1 est=0:100000,1:1000 | c-f | planned order=memo gen=6 switch=0.196429 est=1:11,0:8 | examined/matched: 200/10, 188/9 |
| cross-join-200 | t-b-s | planned order=written gen=0 switch=1 est=0:1000,1:1000,2:1000 | t-b-s | planned order=memo gen=7 switch=1 est=0:8,1:8,2:8 | examined/matched: 200/8, 8/8, 8/8 |
| join-no-literal-200 | u-l | planned order=written gen=0 switch=1 est=0:16,1:1 | u-l | planned order=memo gen=8 switch=1 est=0:16,1:91 | examined/matched: 80/16, 1075/80 |
| compare-all-1000 | r-m | planned order=written gen=0 switch=1 est=0:100000,1:100000 | m-r | planned order=memo gen=10 switch=0.19122 est=1:8,0:1024 | examined/matched: 8/8, 1875/1000 |
| compare-one-1000 | m-r | planned order=planned gen=0 switch=0.8335 est=1:1,0:1000 | m-r | planned order=memo gen=11 switch=0.856351 est=1:1,0:128 | examined/matched: 1/1, 1000/125 |
| customer-statement-1000 | f-c | planned order=written gen=0 switch=1 est=0:100000,1:1000 | c-f | planned order=memo gen=13 switch=0.192581 est=1:23,0:16 | examined/matched: 1000/20, 917/18 |
| cross-join-1000 | t-b-s | planned order=written gen=0 switch=1 est=0:1000,1:1000,2:1000 | t-b-s | planned order=memo gen=14 switch=1 est=0:8,1:8,2:8 | examined/matched: 1000/8, 8/8, 8/8 |
| join-no-literal-1000 | u-l | planned order=written gen=0 switch=1 est=0:16,1:1 | u-l | planned order=memo gen=15 switch=1 est=0:16,1:91 | examined/matched: 80/16, 5075/80 |
| compare-all-10000 | r-m | planned order=written gen=0 switch=1 est=0:100000,1:100000 | m-r | planned order=memo gen=17 switch=0.190542 est=1:8,0:11585 | examined/matched: 8/8, 18750/10000 |
| compare-one-10000 | m-r | planned order=planned gen=0 switch=0.8335 est=1:1,0:1000 | m-r | planned order=memo gen=18 switch=0.835368 est=1:1,0:1448 | examined/matched: 1/1, 10000/1250 |
| customer-statement-10000 | f-c | planned order=written gen=0 switch=1 est=0:100000,1:1000 | c-f | planned order=memo gen=20 switch=0.190848 est=1:32,0:32 | examined/matched: 10000/33, 9029/30 |
| cross-join-10000 | t-b-s | planned order=written gen=0 switch=1 est=0:1000,1:1000,2:1000 | t-b-s | planned order=memo gen=21 switch=1 est=0:8,1:8,2:8 | examined/matched: 10000/8, 8/8, 8/8 |
| join-no-literal-10000 | u-l | planned order=written gen=0 switch=1 est=0:16,1:1 | u-l | planned order=memo gen=22 switch=1 est=0:16,1:91 | examined/matched: 80/16, 50075/80 |

## Literal conjunct selectivity (COUNT with / without)

| cell | conjunct | with | without | true sel | planner constant | error factor (const / true) |
|---|---|---|---|---|---|---|
| compare-one-200 | r.model_id = 3 | 25 | 200 | 0.1250 | 0.01 | 0.08 |
| customer-statement-200 | c.org_id = 3 | 10 | 200 | 0.0500 | 0.01 | 0.2 |
| cross-join-200 | t.session_no = 3 | 8 | 200 | 0.0400 | 0.01 | 0.25 |
| join-no-literal-200 | u.id >= 1 | 200 | 200 | 1.0000 | 0.3 | 0.3 |
| join-no-literal-200 | u.id <= 16 | 16 | 200 | 0.0800 | 0.3 | 3.75 |
| join-no-literal-200 | u.id BETWEEN 1 AND 16 (joint) | 16 | 200 | 0.0800 | 0.3 | 3.75 |
| compare-one-1000 | r.model_id = 3 | 125 | 1000 | 0.1250 | 0.01 | 0.08 |
| customer-statement-1000 | c.org_id = 3 | 20 | 1000 | 0.0200 | 0.01 | 0.5 |
| cross-join-1000 | t.session_no = 3 | 8 | 1000 | 0.0080 | 0.01 | 1.25 |
| join-no-literal-1000 | u.id >= 1 | 1000 | 1000 | 1.0000 | 0.3 | 0.3 |
| join-no-literal-1000 | u.id <= 16 | 16 | 1000 | 0.0160 | 0.3 | 18.8 |
| join-no-literal-1000 | u.id BETWEEN 1 AND 16 (joint) | 16 | 1000 | 0.0160 | 0.3 | 18.8 |
| compare-one-10000 | r.model_id = 3 | 1250 | 10000 | 0.1250 | 0.01 | 0.08 |
| customer-statement-10000 | c.org_id = 3 | 34 | 10000 | 0.0034 | 0.01 | 2.94 |
| cross-join-10000 | t.session_no = 3 | 8 | 10000 | 0.0008 | 0.01 | 12.5 |
| join-no-literal-10000 | u.id >= 1 | 10000 | 10000 | 1.0000 | 0.3 | 0.3 |
| join-no-literal-10000 | u.id <= 16 | 16 | 10000 | 0.0016 | 0.3 | 187 |
| join-no-literal-10000 | u.id BETWEEN 1 AND 16 (joint) | 16 | 10000 | 0.0016 | 0.3 | 187 |

## Latency percentiles and server CPU per arm (all rounds pooled; us)

| cell | arm | ops/block | n | p0 | p25 | p50 | p95 | p99 | QPS (derived 1e6/mean) | server CPU us/op (mean of rounds) |
|---|---|---|---|---|---|---|---|---|---|---|
| compare-all-200 | P:r-m | 777 | 3108 | 291 | 309 | 339 | 483 | 552 | 2825 | 141 |
| compare-all-200 | W2:r-m | 777 | 3108 | 336 | 366 | 404 | 533 | 591 | 2413 | 188 |
| compare-all-200 | W:m-r | 777 | 3108 | 287 | 301 | 323 | 464 | 532 | 2893 | 131 |
| compare-all-200 | W:r-m | 777 | 3108 | 333 | 356 | 394 | 534 | 593 | 2461 | 185 |
| compare-one-200 | W:m-r | 2000 | 8000 | 70 | 72 | 78 | 142 | 199 | 9987 | 50 |
| compare-one-200 | W2:r-m | 2000 | 8000 | 76 | 79 | 92 | 157 | 223 | 8895 | 58 |
| compare-one-200 | P:r-m | 2000 | 8000 | 72 | 76 | 78 | 142 | 178 | 11314 | 49 |
| compare-one-200 | W:r-m | 2000 | 8000 | 76 | 80 | 102 | 153 | 219 | 8875 | 59 |
| customer-statement-200 | W:c-f | 2000 | 8000 | 67 | 72 | 75 | 127 | 157 | 11151 | 64 |
| customer-statement-200 | W:f-c | 2000 | 8000 | 124 | 127 | 131 | 186 | 247 | 7047 | 117 |
| customer-statement-200 | P:f-c | 2000 | 8000 | 73 | 76 | 82 | 134 | 157 | 10172 | 71 |
| customer-statement-200 | W2:f-c | 2000 | 8000 | 126 | 129 | 136 | 188 | 233 | 6736 | 122 |
| cross-join-200 | W:s-t-b | 2000 | 8000 | 106 | 109 | 113 | 171 | 190 | 7955 | 89 |
| cross-join-200 | W2:t-b-s | 2000 | 8000 | 77 | 82 | 126 | 142 | 157 | 9046 | 66 |
| cross-join-200 | P:t-b-s | 2000 | 8000 | 78 | 81 | 114 | 147 | 160 | 9094 | 67 |
| cross-join-200 | W:t-b-s | 2000 | 8000 | 77 | 80 | 125 | 141 | 152 | 9292 | 65 |
| cross-join-200 | W:t-s-b | 2000 | 8000 | 77 | 80 | 91 | 138 | 154 | 9523 | 63 |
| cross-join-200 | W:b-t-s | 2000 | 8000 | 940 | 994 | 1015 | 1111 | 1536 | 967 | 947 |
| join-no-literal-200 | P:u-l | 2000 | 8000 | 123 | 127 | 131 | 207 | 252 | 6759 | 101 |
| join-no-literal-200 | W2:u-l | 2000 | 8000 | 122 | 126 | 131 | 200 | 227 | 6685 | 99 |
| join-no-literal-200 | W:l-u | 2000 | 8000 | 139 | 144 | 149 | 214 | 250 | 6099 | 117 |
| join-no-literal-200 | W:u-l | 2000 | 8000 | 122 | 126 | 130 | 199 | 224 | 6848 | 98 |
| compare-all-1000 | W:m-r | 175 | 700 | 1359 | 1447 | 1502 | 1676 | 1903 | 658 | 515 |
| compare-all-1000 | W2:r-m | 175 | 700 | 1598 | 1702 | 1760 | 1906 | 2081 | 565 | 747 |
| compare-all-1000 | P:r-m | 175 | 700 | 1364 | 1465 | 1520 | 1713 | 2045 | 651 | 521 |
| compare-all-1000 | W:r-m | 175 | 700 | 1610 | 1709 | 1761 | 1931 | 2082 | 563 | 760 |
| compare-one-1000 | W:m-r | 1356 | 5424 | 169 | 174 | 177 | 213 | 282 | 5463 | 84 |
| compare-one-1000 | W:r-m | 1356 | 5424 | 202 | 208 | 212 | 261 | 379 | 4533 | 120 |
| compare-one-1000 | P:r-m | 1356 | 5424 | 170 | 178 | 182 | 215 | 295 | 5328 | 89 |
| compare-one-1000 | W2:r-m | 1356 | 5424 | 197 | 208 | 212 | 313 | 435 | 4416 | 124 |
| customer-statement-1000 | P:f-c | 587 | 2348 | 161 | 165 | 170 | 206 | 284 | 5676 | 153 |
| customer-statement-1000 | W:f-c | 587 | 2348 | 451 | 481 | 500 | 564 | 685 | 1965 | 449 |
| customer-statement-1000 | W:c-f | 587 | 2348 | 158 | 163 | 171 | 212 | 317 | 5619 | 155 |
| customer-statement-1000 | W2:f-c | 587 | 2348 | 454 | 481 | 500 | 567 | 693 | 1966 | 448 |
| cross-join-1000 | W:t-s-b | 2000 | 8000 | 92 | 95 | 98 | 136 | 172 | 9620 | 72 |
| cross-join-1000 | W:t-b-s | 2000 | 8000 | 93 | 95 | 97 | 120 | 151 | 9905 | 70 |
| cross-join-1000 | P:t-b-s | 2000 | 8000 | 94 | 97 | 99 | 148 | 178 | 9376 | 73 |
| cross-join-1000 | W2:t-b-s | 2000 | 8000 | 92 | 96 | 99 | 153 | 173 | 9125 | 74 |
| cross-join-1000 | W:s-t-b | 2000 | 8000 | 233 | 252 | 268 | 334 | 377 | 3620 | 213 |
| cross-join-1000 | W:b-t-s | 201 | 800 | 19561 | 19770 | 19910 | 20865 | 22050 | 50 | 1978 |
| join-no-literal-1000 | W:l-u | 1164 | 4656 | 252 | 268 | 283 | 361 | 507 | 3371 | 224 |
| join-no-literal-1000 | W2:u-l | 1164 | 4656 | 224 | 239 | 252 | 326 | 450 | 3791 | 201 |
| join-no-literal-1000 | P:u-l | 1164 | 4656 | 226 | 241 | 255 | 331 | 402 | 3753 | 202 |
| join-no-literal-1000 | W:u-l | 1164 | 4656 | 224 | 241 | 256 | 333 | 423 | 3731 | 201 |
| compare-all-10000 | W:m-r | 30 | 120 | 12929 | 13100 | 13216 | 14207 | 15503 | 75 | 4151 |
| compare-all-10000 | W:r-m | 30 | 120 | 13513 | 13790 | 13902 | 14426 | 15036 | 72 | 6675 |
| compare-all-10000 | P:r-m | 30 | 120 | 13007 | 13189 | 13292 | 13768 | 14292 | 75 | 4179 |
| compare-all-10000 | W2:r-m | 30 | 120 | 13786 | 14050 | 14173 | 14595 | 15138 | 70 | 6973 |
| compare-one-10000 | P:r-m | 157 | 628 | 1383 | 1459 | 1489 | 1643 | 1683 | 663 | 646 |
| compare-one-10000 | W:r-m | 157 | 628 | 1732 | 1817 | 1880 | 2028 | 2588 | 528 | 1012 |
| compare-one-10000 | W:m-r | 157 | 628 | 1372 | 1476 | 1520 | 1651 | 1903 | 654 | 659 |
| compare-one-10000 | W2:r-m | 157 | 628 | 1704 | 1805 | 1850 | 1980 | 2182 | 534 | 989 |
| customer-statement-10000 | P:f-c | 70 | 280 | 1827 | 1898 | 1931 | 2144 | 2594 | 507 | 1869 |
| customer-statement-10000 | W2:f-c | 70 | 280 | 4060 | 4164 | 4205 | 4443 | 4582 | 236 | 4108 |
| customer-statement-10000 | W:c-f | 70 | 280 | 1812 | 1886 | 1919 | 2052 | 2253 | 517 | 1832 |
| customer-statement-10000 | W:f-c | 70 | 280 | 4040 | 4126 | 4186 | 4461 | 4958 | 236 | 4112 |
| cross-join-10000 | W:t-s-b | 853 | 3412 | 277 | 306 | 331 | 407 | 471 | 2957 | 268 |
| cross-join-10000 | W2:t-b-s | 853 | 3412 | 278 | 302 | 324 | 394 | 463 | 3011 | 264 |
| cross-join-10000 | P:t-b-s | 853 | 3412 | 280 | 320 | 341 | 420 | 468 | 2865 | 281 |
| cross-join-10000 | W:t-b-s | 853 | 3412 | 277 | 299 | 318 | 401 | 510 | 3035 | 263 |
| join-no-literal-10000 | W:l-u | 181 | 724 | 1483 | 1554 | 1577 | 1687 | 2101 | 627 | 1477 |
| join-no-literal-10000 | W:u-l | 181 | 724 | 1440 | 1504 | 1541 | 1659 | 1916 | 644 | 1446 |
| join-no-literal-10000 | P:u-l | 181 | 724 | 1446 | 1524 | 1554 | 1691 | 2041 | 633 | 1466 |
| join-no-literal-10000 | W2:u-l | 181 | 724 | 1449 | 1538 | 1564 | 1660 | 1803 | 636 | 1453 |

## Gate per cell

(a) p50 >= 15 % faster than the planner's informed order (P arm) and outside the noise floor; (b) server CPU >= 15 % lower in every round (>= 3 rounds). Noise floor = |p50(W2) - p50(W:first)| / p50(W:first), the two being the same configuration and spelling.

| cell | planner informed order | best forced (p50) | best forced p50 us | P p50 us | forced-same-as-planner p50 us | delta p50 | noise floor | (a) | (b) rounds with CPU>=15% lower | (b) | blind order == informed order | blind-order forced p50 us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| compare-all-200 | m-r | m-r | 323 | 339 | 323 | 4.7% | 2.5% | no | 1/4 | no | NO | 394 |
| compare-one-200 | m-r | m-r | 78 | 78 | 78 | 0.0% | 9.7% | no | 0/4 | no | yes | 78 |
| customer-statement-200 | c-f | c-f | 75 | 82 | 75 | 8.3% | 3.8% | no | 1/4 | no | NO | 131 |
| cross-join-200 | t-b-s | t-s-b | 91 | 114 | 125 | 20.1% | 0.8% | yes | 0/4 | no | yes | 125 |
| join-no-literal-200 | u-l | u-l | 130 | 131 | 130 | 0.4% | 0.1% | no | 0/4 | no | yes | 130 |
| compare-all-1000 | m-r | m-r | 1502 | 1520 | 1502 | 1.2% | 0.1% | no | 0/4 | no | NO | 1761 |
| compare-one-1000 | m-r | m-r | 177 | 182 | 177 | 2.8% | 0.0% | no | 0/4 | no | yes | 177 |
| customer-statement-1000 | c-f | c-f | 171 | 170 | 171 | -0.6% | 0.1% | no | 0/4 | no | NO | 500 |
| cross-join-1000 | t-b-s | t-b-s | 97 | 99 | 97 | 2.1% | 2.1% | no | 0/4 | no | yes | 97 |
| join-no-literal-1000 | u-l | u-l | 256 | 255 | 256 | -0.6% | 1.7% | no | 0/4 | no | yes | 256 |
| compare-all-10000 | m-r | m-r | 13216 | 13292 | 13216 | 0.6% | 1.9% | no | 0/4 | no | NO | 13902 |
| compare-one-10000 | m-r | m-r | 1520 | 1489 | 1520 | -2.1% | 1.6% | no | 0/4 | no | yes | 1520 |
| customer-statement-10000 | c-f | c-f | 1919 | 1931 | 1919 | 0.6% | 0.4% | no | 0/4 | no | NO | 4186 |
| cross-join-10000 | t-b-s | t-b-s | 318 | 341 | 318 | 6.8% | 2.0% | no | 0/4 | no | yes | 318 |
| join-no-literal-10000 | u-l | u-l | 1541 | 1554 | 1541 | 0.9% | 1.5% | no | 0/4 | no | yes | 1541 |

## Host per block (max load1, foreign build processes seen)

1.19 0
rows-per-cell-round equal across arms: False
