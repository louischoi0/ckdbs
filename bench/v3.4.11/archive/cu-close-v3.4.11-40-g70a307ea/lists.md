## Server CPU per list

| column | rows | n | LA walk (base) | L set | L2 set | set spread | D n statements | J n descents | LA / L | L / J | D / L | reading |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 1,000 rows | 1 value | 24.0 µs | 25.2 µs | 24.4 µs | 8 % | 23.9 µs | 30.0 µs | 0.97 times | 0.83 times | 0.96 times | one value is keyed on both |
| `id` | 1,000 rows | 4 values | 137 µs | 30.1 µs | 30.9 µs | 5 % | 93.7 µs | 32.5 µs | 4.49 times | 0.94 times | 3.07 times | set faster |
| `id` | 1,000 rows | 64 values | 880 µs | 126 µs | 129 µs | 9 % | 1,467 µs | 97.1 µs | 6.92 times | 1.31 times | 11.53 times | set faster |
| `id` | 1,000 rows | 1,024 values | 6,370 µs | 1,347 µs | 1,328 µs | 5 % | 24.2 ms | 913 µs | 4.76 times | 1.47 times | 18.09 times | set faster |
| `id` | 1,000 rows | 4,096 values | 27.0 ms | 6,395 µs | 6,356 µs | 10 % | 95.6 ms | 2,561 µs | 4.23 times | 2.49 times | 14.99 times | set faster |
| `id` | 100,000 rows | 1 value | 28.5 µs | 29.0 µs | 29.4 µs | 7 % | 28.9 µs | 37.2 µs | 0.97 times | 0.78 times | 0.99 times | one value is keyed on both |
| `id` | 100,000 rows | 4 values | 9,974 µs | 35.0 µs | 33.8 µs | 8 % | 98.2 µs | 35.4 µs | 290.17 times | 0.97 times | 2.86 times | set faster |
| `id` | 100,000 rows | 64 values | 78.1 ms | 147 µs | 150 µs | 4 % | 1,748 µs | 103 µs | 525.86 times | 1.44 times | 11.77 times | set faster |
| `id` | 100,000 rows | 1,024 values | 1.14 s | 1,679 µs | 1,656 µs | 6 % | 25.2 ms | 1,225 µs | 685.23 times | 1.36 times | 15.14 times | set faster |
| `id` | 100,000 rows | 4,096 values | 4.17 s | 8,391 µs | 8,179 µs | 6 % | 96.2 ms | 4,442 µs | 503.93 times | 1.87 times | 11.62 times | set faster |
| `k` | 1,000 rows | 1 value | 28.1 µs | 28.9 µs | 28.0 µs | 7 % | 28.0 µs | 36.7 µs | 0.99 times | 0.77 times | 0.99 times | one value is keyed on both |
| `k` | 1,000 rows | 4 values | 139 µs | 45.6 µs | 44.0 µs | 8 % | 114 µs | 44.7 µs | 3.11 times | 1.00 times | 2.54 times | set faster |
| `k` | 1,000 rows | 64 values | 848 µs | 260 µs | 258 µs | 2 % | 1,736 µs | 243 µs | 3.27 times | 1.07 times | 6.70 times | set faster |
| `k` | 1,000 rows | 1,024 values | 7,099 µs | 3,669 µs | 3,581 µs | 17 % | 30.7 ms | 3,248 µs | 1.96 times | 1.12 times | 8.47 times | set faster |
| `k` | 1,000 rows | 4,096 values | 32.3 ms | 20.7 ms | 20.5 ms | 6 % | 122.9 ms | 16.8 ms | 1.57 times | 1.23 times | 5.95 times | set faster |
| `k` | 100,000 rows | 1 value | 31.5 µs | 29.9 µs | 31.6 µs | 13 % | 31.8 µs | 37.4 µs | 1.03 times | 0.82 times | 1.03 times | one value is keyed on both |
| `k` | 100,000 rows | 4 values | 12.7 ms | 59.1 µs | 53.6 µs | 26 % | 127 µs | 52.1 µs | 226.20 times | 1.08 times | 2.25 times | set faster |
| `k` | 100,000 rows | 64 values | 91.6 ms | 336 µs | 350 µs | 7 % | 2,042 µs | 286 µs | 267.07 times | 1.20 times | 5.95 times | set faster |
| `k` | 100,000 rows | 1,024 values | 1.32 s | 4,287 µs | 4,255 µs | 5 % | 32.8 ms | 3,980 µs | 309.66 times | 1.07 times | 7.68 times | set faster |
| `k` | 100,000 rows | 4,096 values | 5.46 s | 19.8 ms | 20.2 ms | 5 % | 125.7 ms | 16.1 ms | 272.94 times | 1.24 times | 6.28 times | set faster |

## Latency per list, client side

| column | rows | n | arm | groups | lists/s | p0 | p25 | p50 | p75 | p99 | max | rows/list |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 1,000 rows | 1 value | LA | 57,022 groups | 15,362.4 lists/s | 48.7 µs | 52.5 µs | 62.8 µs | 72.3 µs | 132 µs | 4,314 µs | 1 rows |
| `id` | 1,000 rows | 1 value | L | 53,120 groups | 14,292.8 lists/s | 49.1 µs | 53.5 µs | 67.2 µs | 74.0 µs | 162 µs | 4,109 µs | 1 rows |
| `id` | 1,000 rows | 1 value | L2 | 55,457 groups | 14,937.0 lists/s | 49.1 µs | 53.2 µs | 64.6 µs | 72.5 µs | 140 µs | 4,679 µs | 1 rows |
| `id` | 1,000 rows | 1 value | D | 56,699 groups | 15,297.4 lists/s | 45.9 µs | 52.7 µs | 63.8 µs | 70.9 µs | 136 µs | 1,904 µs | 1 rows |
| `id` | 1,000 rows | 1 value | J | 45,630 groups | 13,594.2 lists/s | 52.8 µs | 57.1 µs | 66.3 µs | 84.2 µs | 150 µs | 3,906 µs | 1 rows |
| `id` | 1,000 rows | 4 values | LA | 18,994 groups | 4,944.6 lists/s | 146 µs | 165 µs | 197 µs | 220 µs | 356 µs | 4,277 µs | 4 rows |
| `id` | 1,000 rows | 4 values | L | 45,793 groups | 12,427.6 lists/s | 58.1 µs | 62.1 µs | 73.9 µs | 91.1 µs | 160 µs | 4,150 µs | 4 rows |
| `id` | 1,000 rows | 4 values | L2 | 46,373 groups | 12,559.5 lists/s | 58.2 µs | 62.1 µs | 70.9 µs | 84.7 µs | 164 µs | 4,636 µs | 4 rows |
| `id` | 1,000 rows | 4 values | D | 14,960 groups | 3,865.1 lists/s | 195 µs | 209 µs | 258 µs | 283 µs | 474 µs | 3,407 µs | 4 rows |
| `id` | 1,000 rows | 4 values | J | 41,549 groups | 12,410.0 lists/s | 58.7 µs | 62.8 µs | 72.0 µs | 88.0 µs | 167 µs | 3,420 µs | 4 rows |
| `id` | 1,000 rows | 64 values | LA | 3,607 groups | 935.3 lists/s | 844 µs | 953 µs | 1,016 µs | 1,116 µs | 1,724 µs | 4,170 µs | 64 rows |
| `id` | 1,000 rows | 64 values | L | 13,553 groups | 3,772.7 lists/s | 184 µs | 203 µs | 260 µs | 299 µs | 460 µs | 1,978 µs | 64 rows |
| `id` | 1,000 rows | 64 values | L2 | 13,308 groups | 3,698.5 lists/s | 184 µs | 210 µs | 273 µs | 306 µs | 462 µs | 2,613 µs | 64 rows |
| `id` | 1,000 rows | 64 values | D | 982 groups | 247.7 lists/s | 3,191 µs | 3,513 µs | 3,989 µs | 4,495 µs | 5,558 µs | 8,133 µs | 64 rows |
| `id` | 1,000 rows | 64 values | J | 15,560 groups | 4,519.4 lists/s | 146 µs | 167 µs | 225 µs | 248 µs | 400 µs | 4,246 µs | 64 rows |
| `id` | 1,000 rows | 1,024 values | LA | 512 groups | 133.2 lists/s | 6,861 µs | 7,076 µs | 7,215 µs | 7,575 µs | 11.4 ms | 13.7 ms | 1,000 rows |
| `id` | 1,000 rows | 1,024 values | L | 1,411 groups | 398.4 lists/s | 2,178 µs | 2,310 µs | 2,382 µs | 2,574 µs | 3,648 µs | 4,302 µs | 1,000 rows |
| `id` | 1,000 rows | 1,024 values | L2 | 1,409 groups | 397.4 lists/s | 2,196 µs | 2,299 µs | 2,362 µs | 2,701 µs | 3,660 µs | 6,593 µs | 1,000 rows |
| `id` | 1,000 rows | 1,024 values | D | 62 groups | 15.3 lists/s | 54.3 ms | 62.6 ms | 66.1 ms | 69.7 ms | 76.7 ms | 76.8 ms | 1,000 rows |
| `id` | 1,000 rows | 1,024 values | J | 1,808 groups | 530.5 lists/s | 1,689 µs | 1,779 µs | 1,808 µs | 1,884 µs | 2,752 µs | 5,354 µs | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | LA | 136 groups | 35.0 lists/s | 25.5 ms | 26.8 ms | 27.4 ms | 28.7 ms | 46.8 ms | 47.7 ms | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | L | 441 groups | 125.9 lists/s | 7,220 µs | 7,511 µs | 7,711 µs | 8,101 µs | 10.4 ms | 11.4 ms | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | L2 | 435 groups | 124.8 lists/s | 7,132 µs | 7,378 µs | 7,612 µs | 8,364 µs | 11.0 ms | 13.1 ms | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | D | 17 groups | 4.0 lists/s | 214.8 ms | 231.0 ms | 240.1 ms | 254.5 ms | 299.5 ms | 329.2 ms | 1,000 rows |
| `id` | 1,000 rows | 4,096 values | J | 773 groups | 263.4 lists/s | 2,967 µs | 3,140 µs | 3,654 µs | 4,370 µs | 6,421 µs | 9,370 µs | 1,000 rows |
| `id` | 100,000 rows | 1 value | LA | 48,497 groups | 13,025.2 lists/s | 50.5 µs | 65.4 µs | 72.9 µs | 78.3 µs | 170 µs | 2,358 µs | 1 rows |
| `id` | 100,000 rows | 1 value | L | 47,280 groups | 12,703.5 lists/s | 50.2 µs | 66.3 µs | 73.7 µs | 79.7 µs | 179 µs | 6,012 µs | 1 rows |
| `id` | 100,000 rows | 1 value | L2 | 46,335 groups | 12,441.0 lists/s | 50.9 µs | 66.5 µs | 74.0 µs | 80.7 µs | 193 µs | 4,402 µs | 1 rows |
| `id` | 100,000 rows | 1 value | D | 47,838 groups | 12,859.1 lists/s | 49.4 µs | 64.8 µs | 71.8 µs | 77.9 µs | 191 µs | 4,115 µs | 1 rows |
| `id` | 100,000 rows | 1 value | J | 36,804 groups | 10,750.7 lists/s | 53.9 µs | 68.4 µs | 82.2 µs | 104 µs | 247 µs | 6,096 µs | 1 rows |
| `id` | 100,000 rows | 4 values | LA | 392 groups | 98.1 lists/s | 9,410 µs | 9,655 µs | 9,940 µs | 10.3 ms | 15.6 ms | 17.3 ms | 4 rows |
| `id` | 100,000 rows | 4 values | L | 42,039 groups | 11,333.5 lists/s | 59.4 µs | 64.8 µs | 77.0 µs | 102 µs | 189 µs | 4,818 µs | 4 rows |
| `id` | 100,000 rows | 4 values | L2 | 43,569 groups | 11,773.3 lists/s | 59.5 µs | 64.1 µs | 75.9 µs | 98.8 µs | 165 µs | 1,391 µs | 4 rows |
| `id` | 100,000 rows | 4 values | D | 14,669 groups | 3,784.4 lists/s | 197 µs | 213 µs | 262 µs | 289 µs | 497 µs | 5,096 µs | 4 rows |
| `id` | 100,000 rows | 4 values | J | 39,607 groups | 11,751.6 lists/s | 58.9 µs | 63.8 µs | 76.9 µs | 100 µs | 164 µs | 5,890 µs | 4 rows |
| `id` | 100,000 rows | 64 values | LA | 52 groups | 12.6 lists/s | 73.5 ms | 76.0 ms | 77.0 ms | 79.0 ms | 104.0 ms | 112.5 ms | 64 rows |
| `id` | 100,000 rows | 64 values | L | 12,756 groups | 3,518.1 lists/s | 196 µs | 219 µs | 285 µs | 319 µs | 488 µs | 5,424 µs | 64 rows |
| `id` | 100,000 rows | 64 values | L2 | 12,573 groups | 3,464.7 lists/s | 199 µs | 225 µs | 286 µs | 321 µs | 484 µs | 6,550 µs | 64 rows |
| `id` | 100,000 rows | 64 values | D | 867 groups | 218.2 lists/s | 3,216 µs | 3,967 µs | 4,554 µs | 5,220 µs | 7,618 µs | 18.3 ms | 64 rows |
| `id` | 100,000 rows | 64 values | J | 15,248 groups | 4,420.1 lists/s | 151 µs | 168 µs | 231 µs | 255 µs | 425 µs | 2,288 µs | 64 rows |
| `id` | 100,000 rows | 1,024 values | LA | 4 groups | 0.9 lists/s | 1.11 s | 1.13 s | 1.13 s | 1.17 s | 1.17 s | 1.18 s | 1,024 rows |
| `id` | 100,000 rows | 1,024 values | L | 1,193 groups | 335.1 lists/s | 2,504 µs | 2,709 µs | 2,850 µs | 3,178 µs | 4,049 µs | 7,654 µs | 1,024 rows |
| `id` | 100,000 rows | 1,024 values | L2 | 1,221 groups | 342.2 lists/s | 2,499 µs | 2,688 µs | 2,817 µs | 3,122 µs | 4,002 µs | 9,427 µs | 1,024 rows |
| `id` | 100,000 rows | 1,024 values | D | 63 groups | 15.3 lists/s | 54.1 ms | 59.4 ms | 65.0 ms | 69.5 ms | 77.1 ms | 77.6 ms | 1,024 rows |
| `id` | 100,000 rows | 1,024 values | J | 1,535 groups | 441.4 lists/s | 1,973 µs | 2,109 µs | 2,160 µs | 2,308 µs | 3,323 µs | 6,778 µs | 1,024 rows |
| `id` | 100,000 rows | 4,096 values | LA | 2 groups | 0.2 lists/s | 4.20 s | 4.21 s | 4.21 s | 4.21 s | 4.21 s | 4.22 s | 4,096 rows |
| `id` | 100,000 rows | 4,096 values | L | 269 groups | 74.2 lists/s | 11.6 ms | 12.1 ms | 13.5 ms | 14.0 ms | 18.9 ms | 20.5 ms | 4,096 rows |
| `id` | 100,000 rows | 4,096 values | L2 | 269 groups | 74.8 lists/s | 11.5 ms | 12.1 ms | 12.8 ms | 14.7 ms | 17.2 ms | 18.6 ms | 4,096 rows |
| `id` | 100,000 rows | 4,096 values | D | 16 groups | 3.8 lists/s | 235.1 ms | 250.3 ms | 261.8 ms | 273.8 ms | 281.2 ms | 283.9 ms | 4,096 rows |
| `id` | 100,000 rows | 4,096 values | J | 403 groups | 116.3 lists/s | 7,675 µs | 7,959 µs | 8,158 µs | 8,653 µs | 11.8 ms | 15.9 ms | 4,096 rows |
| `k` | 1,000 rows | 1 value | LA | 50,826 groups | 13,653.4 lists/s | 51.6 µs | 57.3 µs | 69.9 µs | 78.9 µs | 161 µs | 1,233 µs | 1 rows |
| `k` | 1,000 rows | 1 value | L | 50,807 groups | 13,646.4 lists/s | 51.7 µs | 60.2 µs | 67.9 µs | 79.4 µs | 158 µs | 4,998 µs | 1 rows |
| `k` | 1,000 rows | 1 value | L2 | 51,838 groups | 13,932.9 lists/s | 51.6 µs | 57.2 µs | 67.7 µs | 78.4 µs | 154 µs | 1,306 µs | 1 rows |
| `k` | 1,000 rows | 1 value | D | 53,147 groups | 14,283.3 lists/s | 50.7 µs | 55.9 µs | 68.2 µs | 76.5 µs | 145 µs | 1,184 µs | 1 rows |
| `k` | 1,000 rows | 1 value | J | 42,004 groups | 12,341.0 lists/s | 56.5 µs | 60.8 µs | 70.7 µs | 98.0 µs | 160 µs | 3,939 µs | 1 rows |
| `k` | 1,000 rows | 4 values | LA | 18,816 groups | 4,896.2 lists/s | 149 µs | 165 µs | 204 µs | 224 µs | 347 µs | 4,550 µs | 4 rows |
| `k` | 1,000 rows | 4 values | L | 36,803 groups | 9,868.9 lists/s | 64.6 µs | 72.5 µs | 88.2 µs | 132 µs | 178 µs | 2,477 µs | 4 rows |
| `k` | 1,000 rows | 4 values | L2 | 36,938 groups | 9,927.5 lists/s | 65.3 µs | 74.8 µs | 90.3 µs | 124 µs | 185 µs | 3,322 µs | 4 rows |
| `k` | 1,000 rows | 4 values | D | 13,456 groups | 3,470.8 lists/s | 207 µs | 226 µs | 286 µs | 310 µs | 566 µs | 7,152 µs | 4 rows |
| `k` | 1,000 rows | 4 values | J | 34,232 groups | 9,995.2 lists/s | 64.9 µs | 71.5 µs | 88.5 µs | 127 µs | 190 µs | 4,013 µs | 4 rows |
| `k` | 1,000 rows | 64 values | LA | 3,703 groups | 960.4 lists/s | 853 µs | 948 µs | 999 µs | 1,077 µs | 1,634 µs | 5,927 µs | 64 rows |
| `k` | 1,000 rows | 64 values | L | 8,561 groups | 2,315.4 lists/s | 299 µs | 377 µs | 412 µs | 466 µs | 701 µs | 5,609 µs | 64 rows |
| `k` | 1,000 rows | 64 values | L2 | 8,609 groups | 2,326.9 lists/s | 302 µs | 382 µs | 411 µs | 459 µs | 671 µs | 1,209 µs | 64 rows |
| `k` | 1,000 rows | 64 values | D | 893 groups | 225.2 lists/s | 3,378 µs | 3,808 µs | 4,556 µs | 4,852 µs | 6,368 µs | 11.9 ms | 64 rows |
| `k` | 1,000 rows | 64 values | J | 8,936 groups | 2,477.1 lists/s | 270 µs | 351 µs | 382 µs | 434 µs | 630 µs | 4,358 µs | 64 rows |
| `k` | 1,000 rows | 1,024 values | LA | 457 groups | 118.5 lists/s | 6,948 µs | 7,318 µs | 7,798 µs | 8,790 µs | 13.8 ms | 14.7 ms | 1,000 rows |
| `k` | 1,000 rows | 1,024 values | L | 729 groups | 195.5 lists/s | 4,041 µs | 4,412 µs | 4,947 µs | 5,691 µs | 7,122 µs | 7,423 µs | 1,000 rows |
| `k` | 1,000 rows | 1,024 values | L2 | 749 groups | 201.0 lists/s | 4,020 µs | 4,369 µs | 4,781 µs | 5,521 µs | 7,197 µs | 11.2 ms | 1,000 rows |
| `k` | 1,000 rows | 1,024 values | D | 53 groups | 13.0 lists/s | 60.1 ms | 72.0 ms | 76.4 ms | 79.7 ms | 98.5 ms | 114.3 ms | 1,000 rows |
| `k` | 1,000 rows | 1,024 values | J | 829 groups | 224.5 lists/s | 3,576 µs | 3,852 µs | 4,224 µs | 4,819 µs | 6,603 µs | 10.5 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | LA | 112 groups | 28.9 lists/s | 27.2 ms | 28.9 ms | 32.2 ms | 39.9 ms | 51.6 ms | 53.0 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | L | 167 groups | 43.8 lists/s | 18.3 ms | 19.5 ms | 21.4 ms | 25.8 ms | 32.5 ms | 33.2 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | L2 | 168 groups | 44.3 lists/s | 18.0 ms | 19.4 ms | 20.9 ms | 24.8 ms | 32.1 ms | 32.5 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | D | 14 groups | 3.3 lists/s | 279.4 ms | 282.2 ms | 299.0 ms | 309.2 ms | 323.3 ms | 327.2 ms | 1,000 rows |
| `k` | 1,000 rows | 4,096 values | J | 206 groups | 55.1 lists/s | 14.0 ms | 15.3 ms | 16.9 ms | 20.8 ms | 25.5 ms | 27.4 ms | 1,000 rows |
| `k` | 100,000 rows | 1 value | LA | 47,660 groups | 12,771.6 lists/s | 52.3 µs | 63.6 µs | 73.4 µs | 82.2 µs | 172 µs | 5,753 µs | 1 rows |
| `k` | 100,000 rows | 1 value | L | 48,555 groups | 13,028.0 lists/s | 52.7 µs | 61.5 µs | 73.4 µs | 82.0 µs | 165 µs | 4,610 µs | 1 rows |
| `k` | 100,000 rows | 1 value | L2 | 46,939 groups | 12,571.5 lists/s | 52.7 µs | 63.3 µs | 74.5 µs | 83.0 µs | 182 µs | 4,388 µs | 1 rows |
| `k` | 100,000 rows | 1 value | D | 47,357 groups | 12,706.7 lists/s | 51.4 µs | 64.0 µs | 76.6 µs | 82.2 µs | 177 µs | 1,324 µs | 1 rows |
| `k` | 100,000 rows | 1 value | J | 37,851 groups | 11,092.4 lists/s | 55.7 µs | 66.3 µs | 79.2 µs | 102 µs | 218 µs | 5,378 µs | 1 rows |
| `k` | 100,000 rows | 4 values | LA | 308 groups | 77.0 lists/s | 9,672 µs | 10.7 ms | 12.3 ms | 15.3 ms | 17.8 ms | 21.1 ms | 4 rows |
| `k` | 100,000 rows | 4 values | L | 28,281 groups | 7,548.2 lists/s | 66.6 µs | 94.6 µs | 119 µs | 154 µs | 402 µs | 5,105 µs | 4 rows |
| `k` | 100,000 rows | 4 values | L2 | 31,910 groups | 8,530.3 lists/s | 67.4 µs | 89.7 µs | 108 µs | 138 µs | 233 µs | 2,822 µs | 4 rows |
| `k` | 100,000 rows | 4 values | D | 12,214 groups | 3,147.4 lists/s | 208 µs | 271 µs | 301 µs | 330 µs | 647 µs | 8,909 µs | 4 rows |
| `k` | 100,000 rows | 4 values | J | 31,096 groups | 8,997.8 lists/s | 66.6 µs | 85.5 µs | 96.9 µs | 132 µs | 220 µs | 5,768 µs | 4 rows |
| `k` | 100,000 rows | 64 values | LA | 44 groups | 10.8 lists/s | 74.3 ms | 84.2 ms | 89.2 ms | 99.4 ms | 122.3 ms | 130.9 ms | 64 rows |
| `k` | 100,000 rows | 64 values | L | 7,171 groups | 1,918.2 lists/s | 324 µs | 436 µs | 484 µs | 604 µs | 791 µs | 7,383 µs | 64 rows |
| `k` | 100,000 rows | 64 values | L2 | 7,005 groups | 1,869.8 lists/s | 325 µs | 442 µs | 512 µs | 617 µs | 793 µs | 5,420 µs | 64 rows |
| `k` | 100,000 rows | 64 values | D | 808 groups | 203.7 lists/s | 3,476 µs | 4,497 µs | 4,914 µs | 5,263 µs | 6,640 µs | 7,829 µs | 64 rows |
| `k` | 100,000 rows | 64 values | J | 8,068 groups | 2,218.0 lists/s | 274 µs | 367 µs | 414 µs | 535 µs | 719 µs | 2,773 µs | 64 rows |
| `k` | 100,000 rows | 1,024 values | LA | 4 groups | 0.8 lists/s | 1.29 s | 1.29 s | 1.29 s | 1.38 s | 1.38 s | 1.43 s | 1,024 rows |
| `k` | 100,000 rows | 1,024 values | L | 658 groups | 175.7 lists/s | 4,436 µs | 4,859 µs | 5,408 µs | 6,424 µs | 7,851 µs | 10.2 ms | 1,024 rows |
| `k` | 100,000 rows | 1,024 values | L2 | 663 groups | 177.2 lists/s | 4,396 µs | 4,753 µs | 5,244 µs | 6,595 µs | 7,873 µs | 10.4 ms | 1,024 rows |
| `k` | 100,000 rows | 1,024 values | D | 50 groups | 12.4 lists/s | 70.6 ms | 77.1 ms | 80.3 ms | 82.7 ms | 96.8 ms | 102.1 ms | 1,024 rows |
| `k` | 100,000 rows | 1,024 values | J | 711 groups | 191.3 lists/s | 3,980 µs | 4,298 µs | 4,871 µs | 6,100 µs | 8,020 µs | 10.4 ms | 1,024 rows |
| `k` | 100,000 rows | 4,096 values | LA | 2 groups | 0.2 lists/s | 5.49 s | 5.53 s | 5.53 s | 5.53 s | 5.53 s | 5.56 s | 4,096 rows |
| `k` | 100,000 rows | 4,096 values | L | 147 groups | 39.0 lists/s | 19.5 ms | 22.9 ms | 25.2 ms | 28.5 ms | 34.0 ms | 34.7 ms | 4,096 rows |
| `k` | 100,000 rows | 4,096 values | L2 | 143 groups | 37.8 lists/s | 19.9 ms | 23.0 ms | 26.4 ms | 29.6 ms | 35.4 ms | 37.5 ms | 4,096 rows |
| `k` | 100,000 rows | 4,096 values | D | 14 groups | 3.2 lists/s | 277.2 ms | 297.3 ms | 304.6 ms | 315.5 ms | 333.0 ms | 340.6 ms | 4,096 rows |
| `k` | 100,000 rows | 4,096 values | J | 175 groups | 47.0 lists/s | 15.5 ms | 18.3 ms | 20.9 ms | 24.0 ms | 32.7 ms | 36.4 ms | 4,096 rows |

## Per value

| column | rows | n | L set CPU per value | J CPU per value | D CPU per value | LA CPU per row walked |
|---|---|---|---|---|---|---|
| `id` | 1,000 rows | 1 value | 24.80 µs | 30.05 µs | 23.85 µs | no walk (one value is keyed) |
| `id` | 1,000 rows | 4 values | 7.62 µs | 8.12 µs | 23.41 µs | 137 ns |
| `id` | 1,000 rows | 64 values | 1.99 µs | 1.52 µs | 22.92 µs | 880 ns |
| `id` | 1,000 rows | 1,024 values | 1.31 µs | 0.89 µs | 23.63 µs | 6,370 ns |
| `id` | 1,000 rows | 4,096 values | 1.56 µs | 0.63 µs | 23.33 µs | 26,985 ns |
| `id` | 100,000 rows | 1 value | 29.20 µs | 37.25 µs | 28.90 µs | no walk (one value is keyed) |
| `id` | 100,000 rows | 4 values | 8.59 µs | 8.84 µs | 24.55 µs | 100 ns |
| `id` | 100,000 rows | 64 values | 2.32 µs | 1.61 µs | 27.31 µs | 781 ns |
| `id` | 100,000 rows | 1,024 values | 1.63 µs | 1.20 µs | 24.66 µs | 11,425 ns |
| `id` | 100,000 rows | 4,096 values | 2.02 µs | 1.08 µs | 23.50 µs | 41,750 ns |
| `k` | 1,000 rows | 1 value | 28.43 µs | 36.70 µs | 28.00 µs | no walk (one value is keyed) |
| `k` | 1,000 rows | 4 values | 11.20 µs | 11.18 µs | 28.43 µs | 139 ns |
| `k` | 1,000 rows | 64 values | 4.05 µs | 3.79 µs | 27.12 µs | 848 ns |
| `k` | 1,000 rows | 1,024 values | 3.54 µs | 3.17 µs | 29.97 µs | 7,099 ns |
| `k` | 1,000 rows | 4,096 values | 5.04 µs | 4.10 µs | 29.99 µs | 32,335 ns |
| `k` | 100,000 rows | 1 value | 30.73 µs | 37.40 µs | 31.80 µs | no walk (one value is keyed) |
| `k` | 100,000 rows | 4 values | 14.09 µs | 13.02 µs | 31.72 µs | 127 ns |
| `k` | 100,000 rows | 64 values | 5.36 µs | 4.47 µs | 31.91 µs | 916 ns |
| `k` | 100,000 rows | 1,024 values | 4.17 µs | 3.89 µs | 32.03 µs | 13,225 ns |
| `k` | 100,000 rows | 4,096 values | 4.88 µs | 3.93 µs | 30.69 µs | 54,600 ns |

## Host, per arm run

load average (1 min) min 3.19 max 5.18; cc1plus processes min 0 max 0; 200 arm runs; checks all same_rows: True (20 cells)

## Plans

- `id`, 100,000 rows, n = 4, arm L: `SELECT * FROM cu_t WHERE id IN (9228, 89109, 49376, 11113)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=4 pages=4 opens=1 pattern_id=0x37dac98f166831de fetch_id=0x69d4d7cd6d180d15`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 Lookup cu_t key_set=4`
    - `  filter bool (0:0.0 = 9228 OR 0:0.0 = 89109 OR 0:0.0 = 49376 OR 0:0.0 = 11113)`
    - `step 0 Lookup cu_t opens=1 examined=4 matched=4 sel=100% pages=4`
    - `DB time 0.13 ms = CPU 0.13`
- `id`, 100,000 rows, n = 4, arm LA: `SELECT * FROM cu_t WHERE id IN (9228, 89109, 49376, 11113)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=100000 pages=705 opens=1 pattern_id=0x37dac98f166831de fetch_id=0x69d4d7cd6d180d15`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 Scan cu_t`
    - `  filter bool (0:0.0 = 9228 OR 0:0.0 = 89109 OR 0:0.0 = 49376 OR 0:0.0 = 11113)`
    - `step 0 Scan cu_t opens=1 examined=100000 matched=4 sel=0% pages=705`
    - `DB time 16.09 ms = CPU 16.09`
- `id`, 100,000 rows, n = 4, arm J: `SELECT t.id, t.k, t.a, t.b FROM cu_v_4 AS v JOIN cu_t AS t ON t.id = v.rid`
    - `analyze rows=4 class=JoinSelect steps=2 examined=8 pages=5 opens=5 pattern_id=0xc054b33f51ca54bc fetch_id=0x133730dc035e9b4`
    - `class=JoinSelect steps=2 hoisted=0`
    - `step 0 Scan cu_v_4 AS v`
    - `step 1 Probe cu_t AS t key=0:0.1`
    - `  filter 0:1.0 = 0:0.1`
    - `step 0 Scan cu_v_4 AS v opens=1 examined=4 matched=4 sel=100% pages=1`
    - `step 1 Probe cu_t AS t opens=4 examined=4 matched=4 sel=100% pages=4`
    - `DB time 0.14 ms = CPU 0.14`
- `id`, 100,000 rows, n = 4, arm D: `SELECT * FROM cu_t WHERE id = 77`
    - `analyze rows=1 class=PointSelect steps=1 examined=1 pages=1 opens=1 pattern_id=0x207dc3e32ec5053b fetch_id=0xd8b45cb18d55b324`
    - `class=PointSelect steps=1 hoisted=0`
    - `step 0 Lookup cu_t key=77`
    - `  filter 0:0.0 = 77`
    - `step 0 Lookup cu_t opens=1 examined=1 matched=1 sel=100% pages=1`
    - `DB time 0.04 ms = CPU 0.04`
- `k`, 100,000 rows, n = 4, arm L: `SELECT * FROM cu_t WHERE k IN (27685, 267328, 148129, 33340)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=4 pages=8 opens=1 pattern_id=0x1310e3352f39eb6b fetch_id=0x6b650e973563bea`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 IndexProbe cu_t key_set=4`
    - `  filter bool (0:0.1 = 27685 OR 0:0.1 = 267328 OR 0:0.1 = 148129 OR 0:0.1 = 33340)`
    - `step 0 IndexProbe cu_t opens=1 examined=4 matched=4 sel=100% pages=8 index_scanned=4 index_resolved=4`
    - `DB time 0.10 ms = CPU 0.10`
- `k`, 100,000 rows, n = 4, arm LA: `SELECT * FROM cu_t WHERE k IN (27685, 267328, 148129, 33340)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=100000 pages=705 opens=1 pattern_id=0x1310e3352f39eb6b fetch_id=0x6b650e973563bea`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 Scan cu_t`
    - `  filter bool (0:0.1 = 27685 OR 0:0.1 = 267328 OR 0:0.1 = 148129 OR 0:0.1 = 33340)`
    - `step 0 Scan cu_t opens=1 examined=100000 matched=4 sel=0% pages=705`
    - `DB time 11.26 ms = CPU 11.26`
- `k`, 100,000 rows, n = 4, arm J: `SELECT t.id, t.k, t.a, t.b FROM cu_v_4 AS v JOIN cu_t AS t ON t.k = v.rk`
    - `analyze rows=4 class=JoinSelect steps=2 examined=8 pages=9 opens=5 pattern_id=0xb569831e9ad879b0 fetch_id=0x6890c43fabf52fc8`
    - `class=JoinSelect steps=2 hoisted=0`
    - `step 0 Scan cu_v_4 AS v`
    - `step 1 IndexProbe cu_t AS t key=0:0.2`
    - `  filter 0:1.1 = 0:0.2`
    - `step 0 Scan cu_v_4 AS v opens=1 examined=4 matched=4 sel=100% pages=1`
    - `step 1 IndexProbe cu_t AS t opens=4 examined=4 matched=4 sel=100% pages=8 index_scanned=4 index_resolved=4`
    - `DB time 0.10 ms = CPU 0.10`
- `k`, 100,000 rows, n = 4, arm D: `SELECT * FROM cu_t WHERE k = 232`
    - `analyze rows=1 class=JoinSelect steps=1 examined=1 pages=2 opens=1 pattern_id=0xe94e9ed3346cd4a2 fetch_id=0xa647081d39bbe5b7`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 IndexProbe cu_t`
    - `  filter 0:0.1 = 232`
    - `step 0 IndexProbe cu_t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1`
    - `DB time 0.06 ms = CPU 0.06`
