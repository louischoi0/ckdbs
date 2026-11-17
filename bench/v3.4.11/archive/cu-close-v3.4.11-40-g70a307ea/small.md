## Server CPU per list

| column | rows | n | LA walk (base) | L set | L2 set | set spread | D n statements | J n descents | LA / L | L / J | D / L | reading |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 1 rows | 1 value | 24.3 µs | 26.1 µs | 24.6 µs | 14 % | 24.8 µs | 32.8 µs | 0.96 times | 0.77 times | 0.97 times | one value is keyed on both |
| `id` | 1 rows | 4 values | 27.4 µs | 28.8 µs | 29.1 µs | 7 % | 94.1 µs | 32.2 µs | 0.95 times | 0.90 times | 3.25 times | inside noise |
| `id` | 1 rows | 64 values | 65.5 µs | 82.5 µs | 88.1 µs | 12 % | 1,457 µs | 62.0 µs | 0.77 times | 1.38 times | 17.08 times | set slower |
| `id` | 1 rows | 1,024 values | 686 µs | 968 µs | 941 µs | 13 % | 23.4 ms | 444 µs | 0.72 times | 2.15 times | 24.49 times | set slower |
| `id` | 1 rows | 4,096 values | 5,257 µs | 6,461 µs | 6,281 µs | 12 % | 98.8 ms | 1,921 µs | 0.83 times | 3.32 times | 15.50 times | set slower |
| `id` | 10 rows | 1 value | 20.2 µs | 20.6 µs | 21.7 µs | 12 % | 20.7 µs | 28.2 µs | 0.95 times | 0.75 times | 0.98 times | one value is keyed on both |
| `id` | 10 rows | 4 values | 29.9 µs | 26.0 µs | 25.5 µs | 14 % | 81.8 µs | 27.6 µs | 1.16 times | 0.93 times | 3.18 times | inside noise |
| `id` | 10 rows | 64 values | 67.6 µs | 80.8 µs | 87.2 µs | 25 % | 1,553 µs | 69.5 µs | 0.80 times | 1.21 times | 18.49 times | inside noise |
| `id` | 10 rows | 1,024 values | 670 µs | 891 µs | 904 µs | 3 % | 23.4 ms | 418 µs | 0.75 times | 2.15 times | 26.03 times | set slower |
| `id` | 10 rows | 4,096 values | 4,658 µs | 5,591 µs | 5,532 µs | 2 % | 95.8 ms | 1,534 µs | 0.84 times | 3.62 times | 17.22 times | set slower |
| `id` | 100 rows | 1 value | 20.2 µs | 21.4 µs | 20.7 µs | 5 % | 20.0 µs | 28.1 µs | 0.96 times | 0.75 times | 0.95 times | one value is keyed on both |
| `id` | 100 rows | 4 values | 35.1 µs | 25.3 µs | 25.4 µs | 4 % | 83.7 µs | 27.3 µs | 1.38 times | 0.93 times | 3.30 times | set faster |
| `id` | 100 rows | 64 values | 125 µs | 99.8 µs | 102 µs | 9 % | 1,292 µs | 77.8 µs | 1.24 times | 1.30 times | 12.78 times | set faster |
| `id` | 100 rows | 1,024 values | 1,191 µs | 977 µs | 977 µs | 3 % | 20.0 ms | 507 µs | 1.22 times | 1.93 times | 20.47 times | set faster |
| `id` | 100 rows | 4,096 values | 6,475 µs | 5,784 µs | 5,768 µs | 1 % | 79.2 ms | 1,755 µs | 1.12 times | 3.29 times | 13.71 times | set faster |
| `k` | 1 rows | 1 value | 29.0 µs | 29.8 µs | 29.1 µs | 4 % | 28.4 µs | 37.1 µs | 0.99 times | 0.79 times | 0.96 times | one value is keyed on both |
| `k` | 1 rows | 4 values | 26.7 µs | 34.4 µs | 35.3 µs | 13 % | 87.0 µs | 33.5 µs | 0.77 times | 1.04 times | 2.49 times | set slower |
| `k` | 1 rows | 64 values | 78.5 µs | 94.6 µs | 103 µs | 29 % | 1,306 µs | 69.0 µs | 0.79 times | 1.43 times | 13.21 times | inside noise |
| `k` | 1 rows | 1,024 values | 638 µs | 988 µs | 1,016 µs | 4 % | 25.1 ms | 473 µs | 0.64 times | 2.12 times | 25.00 times | set slower |
| `k` | 1 rows | 4,096 values | 4,523 µs | 5,977 µs | 5,977 µs | 1 % | 84.0 ms | 1,810 µs | 0.76 times | 3.30 times | 14.05 times | set slower |
| `k` | 10 rows | 1 value | 29.9 µs | 27.6 µs | 27.5 µs | 20 % | 25.5 µs | 30.3 µs | 1.09 times | 0.91 times | 0.93 times | one value is keyed on both |
| `k` | 10 rows | 4 values | 25.3 µs | 28.1 µs | 28.8 µs | 5 % | 85.8 µs | 30.9 µs | 0.89 times | 0.92 times | 3.02 times | set slower |
| `k` | 10 rows | 64 values | 67.6 µs | 91.1 µs | 101 µs | 21 % | 1,531 µs | 70.7 µs | 0.70 times | 1.36 times | 15.94 times | set slower |
| `k` | 10 rows | 1,024 values | 681 µs | 1,089 µs | 1,096 µs | 3 % | 24.6 ms | 560 µs | 0.62 times | 1.95 times | 22.48 times | set slower |
| `k` | 10 rows | 4,096 values | 4,698 µs | 6,426 µs | 6,426 µs | 1 % | 82.8 ms | 2,088 µs | 0.73 times | 3.08 times | 12.89 times | set slower |
| `k` | 100 rows | 1 value | 23.1 µs | 26.4 µs | 22.0 µs | 24 % | 23.9 µs | 28.6 µs | 0.95 times | 0.85 times | 0.99 times | one value is keyed on both |
| `k` | 100 rows | 4 values | 40.1 µs | 32.8 µs | 40.7 µs | 42 % | 108 µs | 41.0 µs | 1.09 times | 0.90 times | 2.93 times | inside noise |
| `k` | 100 rows | 64 values | 128 µs | 156 µs | 169 µs | 21 % | 1,511 µs | 141 µs | 0.79 times | 1.16 times | 9.29 times | set slower |
| `k` | 100 rows | 1,024 values | 1,181 µs | 2,041 µs | 2,051 µs | 4 % | 25.6 ms | 1,548 µs | 0.58 times | 1.32 times | 12.50 times | set slower |
| `k` | 100 rows | 4,096 values | 6,542 µs | 10.1 ms | 10.1 ms | 1 % | 90.0 ms | 5,993 µs | 0.65 times | 1.68 times | 8.95 times | set slower |

## Latency per list, client side

| column | rows | n | arm | groups | lists/s | p0 | p25 | p50 | p75 | p99 | max | rows/list |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `id` | 1 rows | 1 value | LA | 27,566 groups | 14,756.7 lists/s | 48.8 µs | 53.5 µs | 65.3 µs | 72.8 µs | 149 µs | 8,837 µs | 1 rows |
| `id` | 1 rows | 1 value | L | 27,857 groups | 14,868.3 lists/s | 48.9 µs | 56.0 µs | 63.7 µs | 72.2 µs | 146 µs | 5,769 µs | 1 rows |
| `id` | 1 rows | 1 value | L2 | 27,610 groups | 14,780.8 lists/s | 49.0 µs | 52.7 µs | 65.5 µs | 73.0 µs | 138 µs | 10.5 ms | 1 rows |
| `id` | 1 rows | 1 value | D | 28,283 groups | 15,185.5 lists/s | 47.6 µs | 52.6 µs | 64.9 µs | 71.4 µs | 136 µs | 1,515 µs | 1 rows |
| `id` | 1 rows | 1 value | J | 21,338 groups | 12,561.9 lists/s | 53.1 µs | 60.6 µs | 68.9 µs | 96.4 µs | 165 µs | 1,573 µs | 1 rows |
| `id` | 1 rows | 4 values | LA | 25,952 groups | 14,033.4 lists/s | 53.0 µs | 56.4 µs | 64.2 µs | 78.2 µs | 149 µs | 3,674 µs | 1 rows |
| `id` | 1 rows | 4 values | L | 25,758 groups | 13,900.0 lists/s | 52.9 µs | 56.6 µs | 63.4 µs | 80.8 µs | 150 µs | 4,136 µs | 1 rows |
| `id` | 1 rows | 4 values | L2 | 24,740 groups | 13,362.2 lists/s | 53.5 µs | 57.6 µs | 68.2 µs | 82.3 µs | 157 µs | 4,114 µs | 1 rows |
| `id` | 1 rows | 4 values | D | 7,764 groups | 3,999.2 lists/s | 186 µs | 201 µs | 250 µs | 273 µs | 478 µs | 3,185 µs | 1 rows |
| `id` | 1 rows | 4 values | J | 22,407 groups | 13,399.0 lists/s | 55.0 µs | 58.1 µs | 64.5 µs | 87.0 µs | 152 µs | 2,018 µs | 1 rows |
| `id` | 1 rows | 64 values | LA | 13,275 groups | 7,794.2 lists/s | 90.6 µs | 96.9 µs | 118 µs | 154 µs | 236 µs | 2,075 µs | 1 rows |
| `id` | 1 rows | 64 values | L | 11,767 groups | 6,783.6 lists/s | 106 µs | 126 µs | 138 µs | 171 µs | 261 µs | 3,216 µs | 1 rows |
| `id` | 1 rows | 64 values | L2 | 11,014 groups | 6,336.6 lists/s | 105 µs | 122 µs | 159 µs | 175 µs | 280 µs | 4,782 µs | 1 rows |
| `id` | 1 rows | 64 values | D | 535 groups | 269.4 lists/s | 2,993 µs | 3,170 µs | 3,571 µs | 4,052 µs | 6,531 µs | 7,894 µs | 1 rows |
| `id` | 1 rows | 64 values | J | 14,065 groups | 8,917.9 lists/s | 75.9 µs | 81.7 µs | 108 µs | 139 µs | 207 µs | 3,321 µs | 1 rows |
| `id` | 1 rows | 1,024 values | LA | 1,634 groups | 1,076.9 lists/s | 745 µs | 841 µs | 884 µs | 965 µs | 1,369 µs | 7,191 µs | 1 rows |
| `id` | 1 rows | 1,024 values | L | 1,297 groups | 815.4 lists/s | 982 µs | 1,117 µs | 1,176 µs | 1,266 µs | 1,786 µs | 6,378 µs | 1 rows |
| `id` | 1 rows | 1,024 values | L2 | 1,331 groups | 840.1 lists/s | 1,000 µs | 1,102 µs | 1,155 µs | 1,236 µs | 1,711 µs | 3,138 µs | 1 rows |
| `id` | 1 rows | 1,024 values | D | 33 groups | 15.8 lists/s | 52.8 ms | 59.6 ms | 63.2 ms | 67.0 ms | 71.6 ms | 78.5 ms | 1 rows |
| `id` | 1 rows | 1,024 values | J | 2,304 groups | 1,784.5 lists/s | 413 µs | 470 µs | 510 µs | 578 µs | 1,340 µs | 6,084 µs | 1 rows |
| `id` | 1 rows | 4,096 values | LA | 276 groups | 166.1 lists/s | 4,908 µs | 5,203 µs | 5,606 µs | 6,649 µs | 8,318 µs | 9,480 µs | 1 rows |
| `id` | 1 rows | 4,096 values | L | 239 groups | 140.3 lists/s | 5,907 µs | 6,276 µs | 6,811 µs | 7,953 µs | 9,612 µs | 9,887 µs | 1 rows |
| `id` | 1 rows | 4,096 values | L2 | 242 groups | 141.9 lists/s | 5,885 µs | 6,250 µs | 6,616 µs | 7,666 µs | 9,531 µs | 12.2 ms | 1 rows |
| `id` | 1 rows | 4,096 values | D | 8 groups | 3.8 lists/s | 261.0 ms | 261.4 ms | 265.0 ms | 268.1 ms | 269.9 ms | 272.3 ms | 1 rows |
| `id` | 1 rows | 4,096 values | J | 604 groups | 485.8 lists/s | 1,493 µs | 1,646 µs | 1,854 µs | 2,516 µs | 2,922 µs | 6,966 µs | 1 rows |
| `id` | 10 rows | 1 value | LA | 34,713 groups | 18,526.6 lists/s | 48.7 µs | 50.9 µs | 51.7 µs | 54.0 µs | 81.6 µs | 781 µs | 1 rows |
| `id` | 10 rows | 1 value | L | 34,376 groups | 18,333.3 lists/s | 48.7 µs | 51.1 µs | 52.2 µs | 54.9 µs | 78.5 µs | 515 µs | 1 rows |
| `id` | 10 rows | 1 value | L2 | 32,393 groups | 17,330.7 lists/s | 48.8 µs | 51.2 µs | 53.5 µs | 62.9 µs | 88.2 µs | 1,872 µs | 1 rows |
| `id` | 10 rows | 1 value | D | 34,315 groups | 18,381.8 lists/s | 47.9 µs | 50.1 µs | 51.2 µs | 56.3 µs | 76.9 µs | 363 µs | 1 rows |
| `id` | 10 rows | 1 value | J | 25,166 groups | 15,055.7 lists/s | 52.6 µs | 56.1 µs | 59.5 µs | 78.1 µs | 102 µs | 414 µs | 1 rows |
| `id` | 10 rows | 4 values | LA | 25,029 groups | 13,442.2 lists/s | 57.5 µs | 60.4 µs | 67.7 µs | 85.2 µs | 131 µs | 6,438 µs | 4 rows |
| `id` | 10 rows | 4 values | L | 27,891 groups | 15,002.0 lists/s | 56.6 µs | 59.8 µs | 61.0 µs | 65.3 µs | 107 µs | 4,091 µs | 4 rows |
| `id` | 10 rows | 4 values | L2 | 28,598 groups | 15,399.3 lists/s | 56.9 µs | 59.7 µs | 60.8 µs | 64.7 µs | 105 µs | 879 µs | 4 rows |
| `id` | 10 rows | 4 values | D | 9,287 groups | 4,770.9 lists/s | 193 µs | 201 µs | 204 µs | 210 µs | 280 µs | 721 µs | 4 rows |
| `id` | 10 rows | 4 values | J | 25,711 groups | 15,493.4 lists/s | 58.2 µs | 60.7 µs | 62.0 µs | 64.7 µs | 99.0 µs | 952 µs | 4 rows |
| `id` | 10 rows | 64 values | LA | 13,426 groups | 7,728.7 lists/s | 104 µs | 109 µs | 127 µs | 146 µs | 193 µs | 2,430 µs | 10 rows |
| `id` | 10 rows | 64 values | L | 12,507 groups | 7,094.7 lists/s | 116 µs | 122 µs | 126 µs | 159 µs | 217 µs | 886 µs | 10 rows |
| `id` | 10 rows | 64 values | L2 | 11,667 groups | 6,605.8 lists/s | 117 µs | 152 µs | 156 µs | 161 µs | 236 µs | 948 µs | 10 rows |
| `id` | 10 rows | 64 values | D | 496 groups | 249.4 lists/s | 3,034 µs | 3,642 µs | 4,185 µs | 4,217 µs | 4,511 µs | 4,873 µs | 10 rows |
| `id` | 10 rows | 64 values | J | 12,132 groups | 7,427.2 lists/s | 87.9 µs | 124 µs | 128 µs | 160 µs | 183 µs | 567 µs | 10 rows |
| `id` | 10 rows | 1,024 values | LA | 1,702 groups | 1,109.3 lists/s | 776 µs | 877 µs | 895 µs | 919 µs | 1,077 µs | 1,533 µs | 10 rows |
| `id` | 10 rows | 1,024 values | L | 1,425 groups | 888.0 lists/s | 993 µs | 1,101 µs | 1,121 µs | 1,144 µs | 1,244 µs | 1,534 µs | 10 rows |
| `id` | 10 rows | 1,024 values | L2 | 1,427 groups | 887.1 lists/s | 1,011 µs | 1,102 µs | 1,119 µs | 1,138 µs | 1,354 µs | 1,681 µs | 10 rows |
| `id` | 10 rows | 1,024 values | D | 33 groups | 16.2 lists/s | 49.1 ms | 56.6 ms | 65.1 ms | 66.0 ms | 69.1 ms | 69.5 ms | 10 rows |
| `id` | 10 rows | 1,024 values | J | 2,583 groups | 1,989.1 lists/s | 454 µs | 484 µs | 493 µs | 510 µs | 606 µs | 756 µs | 10 rows |
| `id` | 10 rows | 4,096 values | LA | 322 groups | 192.4 lists/s | 5,033 µs | 5,148 µs | 5,183 µs | 5,239 µs | 5,475 µs | 5,751 µs | 10 rows |
| `id` | 10 rows | 4,096 values | L | 279 groups | 162.5 lists/s | 5,914 µs | 6,094 µs | 6,151 µs | 6,202 µs | 6,447 µs | 6,758 µs | 10 rows |
| `id` | 10 rows | 4,096 values | L2 | 282 groups | 164.5 lists/s | 5,887 µs | 6,024 µs | 6,067 µs | 6,111 µs | 6,331 µs | 7,573 µs | 10 rows |
| `id` | 10 rows | 4,096 values | D | 9 groups | 4.0 lists/s | 231.8 ms | 243.1 ms | 250.2 ms | 259.8 ms | 261.4 ms | 261.9 ms | 10 rows |
| `id` | 10 rows | 4,096 values | J | 756 groups | 612.3 lists/s | 1,574 µs | 1,608 µs | 1,624 µs | 1,646 µs | 1,776 µs | 2,489 µs | 10 rows |
| `id` | 100 rows | 1 value | LA | 35,096 groups | 18,768.0 lists/s | 48.9 µs | 51.1 µs | 51.9 µs | 53.8 µs | 68.3 µs | 1,756 µs | 1 rows |
| `id` | 100 rows | 1 value | L | 33,607 groups | 17,960.8 lists/s | 49.2 µs | 51.3 µs | 52.3 µs | 55.2 µs | 84.9 µs | 4,136 µs | 1 rows |
| `id` | 100 rows | 1 value | L2 | 34,779 groups | 18,577.3 lists/s | 48.6 µs | 51.3 µs | 52.3 µs | 54.5 µs | 70.3 µs | 1,959 µs | 1 rows |
| `id` | 100 rows | 1 value | D | 35,462 groups | 18,948.3 lists/s | 47.8 µs | 50.3 µs | 51.1 µs | 53.0 µs | 70.7 µs | 1,948 µs | 1 rows |
| `id` | 100 rows | 1 value | J | 25,792 groups | 15,443.6 lists/s | 53.2 µs | 55.8 µs | 57.8 µs | 77.5 µs | 101 µs | 325 µs | 1 rows |
| `id` | 100 rows | 4 values | LA | 25,652 groups | 13,715.4 lists/s | 65.5 µs | 68.6 µs | 69.9 µs | 73.0 µs | 114 µs | 2,156 µs | 4 rows |
| `id` | 100 rows | 4 values | L | 29,609 groups | 15,980.9 lists/s | 57.2 µs | 59.7 µs | 60.6 µs | 62.9 µs | 93.5 µs | 396 µs | 4 rows |
| `id` | 100 rows | 4 values | L2 | 29,534 groups | 15,945.9 lists/s | 57.5 µs | 60.0 µs | 61.0 µs | 63.4 µs | 80.8 µs | 446 µs | 4 rows |
| `id` | 100 rows | 4 values | D | 8,968 groups | 4,616.6 lists/s | 194 µs | 205 µs | 209 µs | 215 µs | 318 µs | 1,101 µs | 4 rows |
| `id` | 100 rows | 4 values | J | 26,018 groups | 15,711.6 lists/s | 58.1 µs | 60.8 µs | 61.9 µs | 64.3 µs | 81.8 µs | 1,145 µs | 4 rows |
| `id` | 100 rows | 64 values | LA | 7,991 groups | 4,327.2 lists/s | 199 µs | 207 µs | 215 µs | 252 µs | 356 µs | 828 µs | 64 rows |
| `id` | 100 rows | 64 values | L | 9,131 groups | 4,998.3 lists/s | 172 µs | 182 µs | 187 µs | 223 µs | 291 µs | 695 µs | 64 rows |
| `id` | 100 rows | 64 values | L2 | 8,794 groups | 4,808.4 lists/s | 172 µs | 183 µs | 188 µs | 227 µs | 373 µs | 1,435 µs | 64 rows |
| `id` | 100 rows | 64 values | D | 596 groups | 299.9 lists/s | 3,165 µs | 3,236 µs | 3,268 µs | 3,307 µs | 4,171 µs | 4,930 µs | 64 rows |
| `id` | 100 rows | 64 values | J | 9,745 groups | 5,652.6 lists/s | 144 µs | 155 µs | 188 µs | 194 µs | 242 µs | 1,284 µs | 64 rows |
| `id` | 100 rows | 1,024 values | LA | 1,125 groups | 665.0 lists/s | 1,316 µs | 1,439 µs | 1,495 µs | 1,557 µs | 1,807 µs | 2,214 µs | 100 rows |
| `id` | 100 rows | 1,024 values | L | 1,279 groups | 775.6 lists/s | 1,138 µs | 1,225 µs | 1,276 µs | 1,344 µs | 1,527 µs | 2,109 µs | 100 rows |
| `id` | 100 rows | 1,024 values | L2 | 1,259 groups | 762.1 lists/s | 1,148 µs | 1,254 µs | 1,287 µs | 1,329 µs | 1,695 µs | 6,807 µs | 100 rows |
| `id` | 100 rows | 1,024 values | D | 40 groups | 19.9 lists/s | 49.2 ms | 49.5 ms | 49.8 ms | 50.0 ms | 54.8 ms | 58.2 ms | 100 rows |
| `id` | 100 rows | 1,024 values | J | 2,092 groups | 1,474.0 lists/s | 589 µs | 642 µs | 670 µs | 704 µs | 901 µs | 1,157 µs | 100 rows |
| `id` | 100 rows | 4,096 values | LA | 244 groups | 139.0 lists/s | 6,744 µs | 7,064 µs | 7,183 µs | 7,313 µs | 7,620 µs | 7,695 µs | 100 rows |
| `id` | 100 rows | 4,096 values | L | 268 groups | 155.1 lists/s | 6,156 µs | 6,324 µs | 6,390 µs | 6,471 µs | 8,410 µs | 9,374 µs | 100 rows |
| `id` | 100 rows | 4,096 values | L2 | 267 groups | 154.8 lists/s | 6,185 µs | 6,352 µs | 6,405 µs | 6,473 µs | 7,286 µs | 15.5 ms | 100 rows |
| `id` | 100 rows | 4,096 values | D | 12 groups | 5.0 lists/s | 195.3 ms | 197.2 ms | 197.7 ms | 199.1 ms | 199.9 ms | 201.1 ms | 100 rows |
| `id` | 100 rows | 4,096 values | J | 661 groups | 508.0 lists/s | 1,804 µs | 1,913 µs | 1,964 µs | 2,010 µs | 2,221 µs | 2,796 µs | 100 rows |
| `k` | 1 rows | 1 value | LA | 24,110 groups | 12,897.7 lists/s | 50.6 µs | 64.7 µs | 73.8 µs | 79.4 µs | 168 µs | 4,451 µs | 1 rows |
| `k` | 1 rows | 1 value | L | 24,200 groups | 12,942.5 lists/s | 50.8 µs | 63.2 µs | 73.6 µs | 80.0 µs | 174 µs | 958 µs | 1 rows |
| `k` | 1 rows | 1 value | L2 | 23,744 groups | 12,701.2 lists/s | 50.6 µs | 66.7 µs | 74.5 µs | 80.1 µs | 172 µs | 2,538 µs | 1 rows |
| `k` | 1 rows | 1 value | D | 25,381 groups | 13,585.0 lists/s | 49.9 µs | 62.7 µs | 70.4 µs | 76.2 µs | 160 µs | 1,179 µs | 1 rows |
| `k` | 1 rows | 1 value | J | 19,126 groups | 11,190.4 lists/s | 54.7 µs | 70.3 µs | 79.0 µs | 99.9 µs | 192 µs | 2,791 µs | 1 rows |
| `k` | 1 rows | 4 values | LA | 27,074 groups | 14,641.0 lists/s | 52.2 µs | 56.2 µs | 66.8 µs | 75.8 µs | 123 µs | 1,137 µs | 1 rows |
| `k` | 1 rows | 4 values | L | 23,150 groups | 12,412.4 lists/s | 55.4 µs | 64.1 µs | 74.3 µs | 88.5 µs | 180 µs | 3,726 µs | 1 rows |
| `k` | 1 rows | 4 values | L2 | 21,833 groups | 11,764.1 lists/s | 55.5 µs | 61.4 µs | 85.4 µs | 96.2 µs | 144 µs | 2,455 µs | 1 rows |
| `k` | 1 rows | 4 values | D | 8,764 groups | 4,509.4 lists/s | 190 µs | 198 µs | 206 µs | 241 µs | 374 µs | 1,608 µs | 1 rows |
| `k` | 1 rows | 4 values | J | 23,138 groups | 13,842.2 lists/s | 55.9 µs | 59.9 µs | 69.2 µs | 83.1 µs | 114 µs | 2,977 µs | 1 rows |
| `k` | 1 rows | 64 values | LA | 11,220 groups | 6,544.6 lists/s | 92.9 µs | 153 µs | 156 µs | 160 µs | 187 µs | 842 µs | 1 rows |
| `k` | 1 rows | 64 values | L | 11,787 groups | 6,718.9 lists/s | 114 µs | 149 µs | 153 µs | 158 µs | 208 µs | 909 µs | 1 rows |
| `k` | 1 rows | 64 values | L2 | 10,018 groups | 5,719.0 lists/s | 114 µs | 152 µs | 183 µs | 189 µs | 218 µs | 569 µs | 1 rows |
| `k` | 1 rows | 64 values | D | 628 groups | 315.9 lists/s | 3,011 µs | 3,078 µs | 3,104 µs | 3,140 µs | 3,979 µs | 5,374 µs | 1 rows |
| `k` | 1 rows | 64 values | J | 13,220 groups | 8,199.5 lists/s | 80.3 µs | 88.7 µs | 118 µs | 148 µs | 182 µs | 2,887 µs | 1 rows |
| `k` | 1 rows | 1,024 values | LA | 1,725 groups | 1,129.1 lists/s | 762 µs | 858 µs | 877 µs | 903 µs | 1,056 µs | 1,606 µs | 1 rows |
| `k` | 1 rows | 1,024 values | L | 1,305 groups | 796.7 lists/s | 1,119 µs | 1,228 µs | 1,245 µs | 1,268 µs | 1,451 µs | 1,887 µs | 1 rows |
| `k` | 1 rows | 1,024 values | L2 | 1,289 groups | 785.8 lists/s | 1,145 µs | 1,236 µs | 1,255 µs | 1,279 µs | 1,691 µs | 2,640 µs | 1 rows |
| `k` | 1 rows | 1,024 values | D | 33 groups | 16.4 lists/s | 49.5 ms | 58.6 ms | 58.9 ms | 67.5 ms | 68.2 ms | 68.4 ms | 1 rows |
| `k` | 1 rows | 1,024 values | J | 2,432 groups | 1,810.2 lists/s | 498 µs | 532 µs | 541 µs | 558 µs | 706 µs | 965 µs | 1 rows |
| `k` | 1 rows | 4,096 values | LA | 325 groups | 193.9 lists/s | 4,948 µs | 5,075 µs | 5,136 µs | 5,213 µs | 5,766 µs | 5,871 µs | 1 rows |
| `k` | 1 rows | 4,096 values | L | 261 groups | 150.2 lists/s | 6,405 µs | 6,558 µs | 6,618 µs | 6,694 µs | 7,202 µs | 7,244 µs | 1 rows |
| `k` | 1 rows | 4,096 values | L2 | 261 groups | 149.8 lists/s | 6,419 µs | 6,570 µs | 6,630 µs | 6,701 µs | 7,594 µs | 9,062 µs | 1 rows |
| `k` | 1 rows | 4,096 values | D | 10 groups | 5.0 lists/s | 198.7 ms | 199.7 ms | 199.8 ms | 200.3 ms | 202.4 ms | 203.7 ms | 1 rows |
| `k` | 1 rows | 4,096 values | J | 674 groups | 514.0 lists/s | 1,757 µs | 1,872 µs | 1,913 µs | 1,967 µs | 2,615 µs | 6,774 µs | 1 rows |
| `k` | 10 rows | 1 value | LA | 24,073 groups | 12,964.4 lists/s | 50.9 µs | 73.9 µs | 74.8 µs | 77.7 µs | 107 µs | 442 µs | 1 rows |
| `k` | 10 rows | 1 value | L | 26,411 groups | 14,185.7 lists/s | 51.1 µs | 64.0 µs | 65.8 µs | 76.8 µs | 109 µs | 360 µs | 1 rows |
| `k` | 10 rows | 1 value | L2 | 26,814 groups | 14,400.3 lists/s | 50.8 µs | 63.9 µs | 65.7 µs | 76.7 µs | 107 µs | 508 µs | 1 rows |
| `k` | 10 rows | 1 value | D | 29,725 groups | 15,949.5 lists/s | 49.4 µs | 62.1 µs | 63.0 µs | 64.8 µs | 84.7 µs | 549 µs | 1 rows |
| `k` | 10 rows | 1 value | J | 24,115 groups | 14,333.6 lists/s | 54.8 µs | 57.2 µs | 59.1 µs | 70.5 µs | 127 µs | 1,205 µs | 1 rows |
| `k` | 10 rows | 4 values | LA | 29,622 groups | 15,927.0 lists/s | 57.5 µs | 60.1 µs | 61.2 µs | 63.6 µs | 80.2 µs | 984 µs | 4 rows |
| `k` | 10 rows | 4 values | L | 28,167 groups | 15,106.2 lists/s | 60.6 µs | 63.1 µs | 64.2 µs | 66.9 µs | 97.0 µs | 330 µs | 4 rows |
| `k` | 10 rows | 4 values | L2 | 27,823 groups | 14,918.3 lists/s | 60.3 µs | 63.2 µs | 64.4 µs | 67.0 µs | 107 µs | 465 µs | 4 rows |
| `k` | 10 rows | 4 values | D | 9,084 groups | 4,663.0 lists/s | 198 µs | 208 µs | 211 µs | 217 µs | 252 µs | 1,337 µs | 4 rows |
| `k` | 10 rows | 4 values | J | 24,868 groups | 14,835.8 lists/s | 60.4 µs | 63.2 µs | 64.4 µs | 67.2 µs | 107 µs | 617 µs | 4 rows |
| `k` | 10 rows | 64 values | LA | 13,848 groups | 7,961.1 lists/s | 106 µs | 111 µs | 116 µs | 144 µs | 194 µs | 1,850 µs | 10 rows |
| `k` | 10 rows | 64 values | L | 12,402 groups | 6,971.0 lists/s | 132 µs | 136 µs | 139 µs | 144 µs | 219 µs | 1,420 µs | 10 rows |
| `k` | 10 rows | 64 values | L2 | 10,941 groups | 6,127.0 lists/s | 132 µs | 142 µs | 174 µs | 180 µs | 230 µs | 793 µs | 10 rows |
| `k` | 10 rows | 64 values | D | 503 groups | 252.8 lists/s | 3,075 µs | 3,229 µs | 4,294 µs | 4,329 µs | 4,737 µs | 4,904 µs | 10 rows |
| `k` | 10 rows | 64 values | J | 13,507 groups | 8,327.0 lists/s | 99.6 µs | 104 µs | 108 µs | 140 µs | 190 µs | 506 µs | 10 rows |
| `k` | 10 rows | 1,024 values | LA | 1,644 groups | 1,060.7 lists/s | 811 µs | 920 µs | 935 µs | 956 µs | 1,095 µs | 1,336 µs | 10 rows |
| `k` | 10 rows | 1,024 values | L | 1,230 groups | 737.9 lists/s | 1,242 µs | 1,334 µs | 1,349 µs | 1,368 µs | 1,488 µs | 1,990 µs | 10 rows |
| `k` | 10 rows | 1,024 values | L2 | 1,223 groups | 732.4 lists/s | 1,229 µs | 1,325 µs | 1,358 µs | 1,396 µs | 1,507 µs | 1,788 µs | 10 rows |
| `k` | 10 rows | 1,024 values | D | 33 groups | 16.2 lists/s | 49.9 ms | 55.7 ms | 64.2 ms | 67.6 ms | 69.2 ms | 69.8 ms | 10 rows |
| `k` | 10 rows | 1,024 values | J | 2,162 groups | 1,542.3 lists/s | 580 µs | 620 µs | 641 µs | 673 µs | 754 µs | 948 µs | 10 rows |
| `k` | 10 rows | 4,096 values | LA | 315 groups | 187.6 lists/s | 5,055 µs | 5,255 µs | 5,319 µs | 5,384 µs | 5,816 µs | 6,219 µs | 10 rows |
| `k` | 10 rows | 4,096 values | L | 249 groups | 142.5 lists/s | 6,770 µs | 6,943 µs | 6,990 µs | 7,067 µs | 7,420 µs | 7,573 µs | 10 rows |
| `k` | 10 rows | 4,096 values | L2 | 249 groups | 142.6 lists/s | 6,789 µs | 6,931 µs | 6,988 µs | 7,048 µs | 7,387 µs | 9,185 µs | 10 rows |
| `k` | 10 rows | 4,096 values | D | 11 groups | 5.0 lists/s | 195.7 ms | 198.5 ms | 199.3 ms | 199.6 ms | 202.4 ms | 203.6 ms | 10 rows |
| `k` | 10 rows | 4,096 values | J | 613 groups | 448.9 lists/s | 2,062 µs | 2,167 µs | 2,217 µs | 2,268 µs | 2,580 µs | 3,129 µs | 10 rows |
| `k` | 100 rows | 1 value | LA | 32,028 groups | 17,126.7 lists/s | 51.0 µs | 53.7 µs | 54.8 µs | 59.7 µs | 87.7 µs | 332 µs | 1 rows |
| `k` | 100 rows | 1 value | L | 28,362 groups | 15,181.5 lists/s | 51.0 µs | 53.9 µs | 65.2 µs | 76.6 µs | 107 µs | 562 µs | 1 rows |
| `k` | 100 rows | 1 value | L2 | 33,668 groups | 17,965.8 lists/s | 50.5 µs | 53.2 µs | 54.1 µs | 56.0 µs | 74.0 µs | 439 µs | 1 rows |
| `k` | 100 rows | 1 value | D | 30,728 groups | 16,465.2 lists/s | 49.9 µs | 52.7 µs | 63.0 µs | 65.2 µs | 93.5 µs | 1,130 µs | 1 rows |
| `k` | 100 rows | 1 value | J | 27,341 groups | 16,406.5 lists/s | 55.3 µs | 57.8 µs | 58.9 µs | 61.4 µs | 90.2 µs | 863 µs | 1 rows |
| `k` | 100 rows | 4 values | LA | 22,089 groups | 11,800.2 lists/s | 66.6 µs | 69.8 µs | 72.1 µs | 102 µs | 144 µs | 1,620 µs | 4 rows |
| `k` | 100 rows | 4 values | L | 25,939 groups | 13,904.2 lists/s | 62.0 µs | 65.6 µs | 67.0 µs | 70.3 µs | 113 µs | 928 µs | 4 rows |
| `k` | 100 rows | 4 values | L2 | 20,487 groups | 10,970.8 lists/s | 62.5 µs | 85.6 µs | 96.6 µs | 100 µs | 146 µs | 609 µs | 4 rows |
| `k` | 100 rows | 4 values | D | 7,559 groups | 3,884.2 lists/s | 204 µs | 254 µs | 258 µs | 266 µs | 467 µs | 2,828 µs | 4 rows |
| `k` | 100 rows | 4 values | J | 19,490 groups | 11,469.9 lists/s | 63.9 µs | 93.7 µs | 96.3 µs | 99.6 µs | 121 µs | 922 µs | 4 rows |
| `k` | 100 rows | 64 values | LA | 7,914 groups | 4,282.9 lists/s | 202 µs | 211 µs | 219 µs | 253 µs | 349 µs | 840 µs | 64 rows |
| `k` | 100 rows | 64 values | L | 7,057 groups | 3,786.3 lists/s | 232 µs | 245 µs | 251 µs | 268 µs | 394 µs | 602 µs | 64 rows |
| `k` | 100 rows | 64 values | L2 | 6,568 groups | 3,514.9 lists/s | 234 µs | 282 µs | 288 µs | 298 µs | 372 µs | 600 µs | 64 rows |
| `k` | 100 rows | 64 values | D | 544 groups | 273.2 lists/s | 3,308 µs | 3,388 µs | 3,438 µs | 4,089 µs | 4,906 µs | 5,213 µs | 64 rows |
| `k` | 100 rows | 64 values | J | 7,185 groups | 4,011.8 lists/s | 203 µs | 214 µs | 250 µs | 284 µs | 350 µs | 1,264 µs | 64 rows |
| `k` | 100 rows | 1,024 values | LA | 1,118 groups | 661.1 lists/s | 1,374 µs | 1,467 µs | 1,499 µs | 1,539 µs | 1,798 µs | 2,420 µs | 100 rows |
| `k` | 100 rows | 1,024 values | L | 740 groups | 412.9 lists/s | 2,249 µs | 2,366 µs | 2,394 µs | 2,446 µs | 2,780 µs | 3,907 µs | 100 rows |
| `k` | 100 rows | 1,024 values | L2 | 746 groups | 415.9 lists/s | 2,224 µs | 2,360 µs | 2,391 µs | 2,432 µs | 2,628 µs | 3,074 µs | 100 rows |
| `k` | 100 rows | 1,024 values | D | 34 groups | 16.4 lists/s | 51.0 ms | 60.5 ms | 60.9 ms | 62.5 ms | 71.3 ms | 71.6 ms | 100 rows |
| `k` | 100 rows | 1,024 values | J | 1,001 groups | 579.2 lists/s | 1,634 µs | 1,699 µs | 1,719 µs | 1,742 µs | 1,858 µs | 2,675 µs | 100 rows |
| `k` | 100 rows | 4,096 values | LA | 240 groups | 136.9 lists/s | 6,941 µs | 7,189 µs | 7,287 µs | 7,393 µs | 7,778 µs | 8,016 µs | 100 rows |
| `k` | 100 rows | 4,096 values | L | 171 groups | 93.2 lists/s | 10.4 ms | 10.6 ms | 10.7 ms | 10.8 ms | 11.7 ms | 12.0 ms | 100 rows |
| `k` | 100 rows | 4,096 values | L2 | 170 groups | 92.7 lists/s | 10.4 ms | 10.6 ms | 10.7 ms | 10.8 ms | 14.4 ms | 16.2 ms | 100 rows |
| `k` | 100 rows | 4,096 values | D | 10 groups | 4.6 lists/s | 203.1 ms | 206.1 ms | 221.0 ms | 224.3 ms | 239.8 ms | 267.6 ms | 100 rows |
| `k` | 100 rows | 4,096 values | J | 277 groups | 162.1 lists/s | 6,001 µs | 6,099 µs | 6,170 µs | 6,211 µs | 6,672 µs | 7,225 µs | 100 rows |

## Per value

| column | rows | n | L set CPU per value | J CPU per value | D CPU per value | LA CPU per row walked |
|---|---|---|---|---|---|---|
| `id` | 1 rows | 1 value | 25.40 µs | 32.80 µs | 24.75 µs | no walk (one value is keyed) |
| `id` | 1 rows | 4 values | 7.24 µs | 8.05 µs | 23.52 µs | 27,400 ns |
| `id` | 1 rows | 64 values | 1.33 µs | 0.97 µs | 22.77 µs | 65,550 ns |
| `id` | 1 rows | 1,024 values | 0.93 µs | 0.43 µs | 22.84 µs | 686,050 ns |
| `id` | 1 rows | 4,096 values | 1.56 µs | 0.47 µs | 24.11 µs | 5,256,900 ns |
| `id` | 10 rows | 1 value | 21.18 µs | 28.20 µs | 20.70 µs | no walk (one value is keyed) |
| `id` | 10 rows | 4 values | 6.44 µs | 6.91 µs | 20.46 µs | 2,995 ns |
| `id` | 10 rows | 64 values | 1.31 µs | 1.09 µs | 24.27 µs | 6,760 ns |
| `id` | 10 rows | 1,024 values | 0.88 µs | 0.41 µs | 22.82 µs | 66,985 ns |
| `id` | 10 rows | 4,096 values | 1.36 µs | 0.37 µs | 23.38 µs | 465,840 ns |
| `id` | 100 rows | 1 value | 21.05 µs | 28.15 µs | 20.00 µs | no walk (one value is keyed) |
| `id` | 100 rows | 4 values | 6.34 µs | 6.83 µs | 20.91 µs | 351 ns |
| `id` | 100 rows | 64 values | 1.58 µs | 1.22 µs | 20.19 µs | 1,254 ns |
| `id` | 100 rows | 1,024 values | 0.95 µs | 0.50 µs | 19.53 µs | 11,912 ns |
| `id` | 100 rows | 4,096 values | 1.41 µs | 0.43 µs | 19.33 µs | 64,754 ns |
| `k` | 1 rows | 1 value | 29.40 µs | 37.10 µs | 28.35 µs | no walk (one value is keyed) |
| `k` | 1 rows | 4 values | 8.72 µs | 8.39 µs | 21.75 µs | 26,700 ns |
| `k` | 1 rows | 64 values | 1.54 µs | 1.08 µs | 20.40 µs | 78,500 ns |
| `k` | 1 rows | 1,024 values | 0.98 µs | 0.46 µs | 24.47 µs | 637,800 ns |
| `k` | 1 rows | 4,096 values | 1.46 µs | 0.44 µs | 20.51 µs | 4,523,050 ns |
| `k` | 10 rows | 1 value | 27.50 µs | 30.30 µs | 25.50 µs | no walk (one value is keyed) |
| `k` | 10 rows | 4 values | 7.10 µs | 7.74 µs | 21.46 µs | 2,530 ns |
| `k` | 10 rows | 64 values | 1.50 µs | 1.10 µs | 23.92 µs | 6,760 ns |
| `k` | 10 rows | 1,024 values | 1.07 µs | 0.55 µs | 23.98 µs | 68,125 ns |
| `k` | 10 rows | 4,096 values | 1.57 µs | 0.51 µs | 20.22 µs | 469,845 ns |
| `k` | 100 rows | 1 value | 24.20 µs | 28.55 µs | 23.90 µs | no walk (one value is keyed) |
| `k` | 100 rows | 4 values | 9.19 µs | 10.25 µs | 26.94 µs | 401 ns |
| `k` | 100 rows | 64 values | 2.54 µs | 2.20 µs | 23.61 µs | 1,279 ns |
| `k` | 100 rows | 1,024 values | 2.00 µs | 1.51 µs | 24.98 µs | 11,807 ns |
| `k` | 100 rows | 4,096 values | 2.46 µs | 1.46 µs | 21.97 µs | 65,416 ns |

## Host, per arm run

load average (1 min) min 1.03 max 4.89; cc1plus processes min 0 max 0; 300 arm runs; checks all same_rows: True (30 cells)

## Plans

- `id`, 100 rows, n = 4, arm L: `SELECT * FROM cu_t WHERE id IN (20, 65, 35, 94)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=4 pages=4 opens=1 pattern_id=0x37dac98f166831de fetch_id=0x69d4d7cd6d180d15`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 Lookup cu_t key_set=4`
    - `  filter bool (0:0.0 = 20 OR 0:0.0 = 65 OR 0:0.0 = 35 OR 0:0.0 = 94)`
    - `step 0 Lookup cu_t opens=1 examined=4 matched=4 sel=100% pages=4`
    - `DB time 0.10 ms = CPU 0.10`
- `id`, 100 rows, n = 4, arm LA: `SELECT * FROM cu_t WHERE id IN (20, 65, 35, 94)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=100 pages=1 opens=1 pattern_id=0x37dac98f166831de fetch_id=0x69d4d7cd6d180d15`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 Scan cu_t`
    - `  filter bool (0:0.0 = 20 OR 0:0.0 = 65 OR 0:0.0 = 35 OR 0:0.0 = 94)`
    - `step 0 Scan cu_t opens=1 examined=100 matched=4 sel=4% pages=1`
    - `DB time 0.14 ms = CPU 0.14`
- `id`, 100 rows, n = 4, arm J: `SELECT t.id, t.k, t.a, t.b FROM cu_v_4 AS v JOIN cu_t AS t ON t.id = v.rid`
    - `analyze rows=4 class=JoinSelect steps=2 examined=8 pages=5 opens=5 pattern_id=0xc054b33f51ca54bc fetch_id=0x133730dc035e9b4`
    - `class=JoinSelect steps=2 hoisted=0`
    - `step 0 Scan cu_v_4 AS v`
    - `step 1 Probe cu_t AS t key=0:0.1`
    - `  filter 0:1.0 = 0:0.1`
    - `step 0 Scan cu_v_4 AS v opens=1 examined=4 matched=4 sel=100% pages=1`
    - `step 1 Probe cu_t AS t opens=4 examined=4 matched=4 sel=100% pages=4`
    - `DB time 0.12 ms = CPU 0.12`
- `id`, 100 rows, n = 4, arm D: `SELECT * FROM cu_t WHERE id = 77`
    - `analyze rows=1 class=PointSelect steps=1 examined=1 pages=1 opens=1 pattern_id=0x207dc3e32ec5053b fetch_id=0xd8b45cb18d55b324`
    - `class=PointSelect steps=1 hoisted=0`
    - `step 0 Lookup cu_t key=77`
    - `  filter 0:0.0 = 77`
    - `step 0 Lookup cu_t opens=1 examined=1 matched=1 sel=100% pages=1`
    - `DB time 0.03 ms = CPU 0.03`
- `k`, 100 rows, n = 4, arm L: `SELECT * FROM cu_t WHERE k IN (61, 196, 106, 283)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=4 pages=8 opens=1 pattern_id=0x1310e3352f39eb6b fetch_id=0x6b650e973563bea`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 IndexProbe cu_t key_set=4`
    - `  filter bool (0:0.1 = 61 OR 0:0.1 = 196 OR 0:0.1 = 106 OR 0:0.1 = 283)`
    - `step 0 IndexProbe cu_t opens=1 examined=4 matched=4 sel=100% pages=8 index_scanned=4 index_resolved=4`
    - `DB time 0.11 ms = CPU 0.11`
- `k`, 100 rows, n = 4, arm LA: `SELECT * FROM cu_t WHERE k IN (61, 196, 106, 283)`
    - `analyze rows=4 class=JoinSelect steps=1 examined=100 pages=1 opens=1 pattern_id=0x1310e3352f39eb6b fetch_id=0x6b650e973563bea`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 Scan cu_t`
    - `  filter bool (0:0.1 = 61 OR 0:0.1 = 196 OR 0:0.1 = 106 OR 0:0.1 = 283)`
    - `step 0 Scan cu_t opens=1 examined=100 matched=4 sel=4% pages=1`
    - `DB time 0.12 ms = CPU 0.12`
- `k`, 100 rows, n = 4, arm J: `SELECT t.id, t.k, t.a, t.b FROM cu_v_4 AS v JOIN cu_t AS t ON t.k = v.rk`
    - `analyze rows=4 class=JoinSelect steps=2 examined=8 pages=9 opens=5 pattern_id=0xb569831e9ad879b0 fetch_id=0x6890c43fabf52fc8`
    - `class=JoinSelect steps=2 hoisted=0`
    - `step 0 Scan cu_v_4 AS v`
    - `step 1 IndexProbe cu_t AS t key=0:0.2`
    - `  filter 0:1.1 = 0:0.2`
    - `step 0 Scan cu_v_4 AS v opens=1 examined=4 matched=4 sel=100% pages=1`
    - `step 1 IndexProbe cu_t AS t opens=4 examined=4 matched=4 sel=100% pages=8 index_scanned=4 index_resolved=4`
    - `DB time 0.07 ms = CPU 0.07`
- `k`, 100 rows, n = 4, arm D: `SELECT * FROM cu_t WHERE k = 232`
    - `analyze rows=1 class=JoinSelect steps=1 examined=1 pages=2 opens=1 pattern_id=0xe94e9ed3346cd4a2 fetch_id=0xa647081d39bbe5b7`
    - `class=JoinSelect steps=1 hoisted=0`
    - `step 0 IndexProbe cu_t`
    - `  filter 0:0.1 = 232`
    - `step 0 IndexProbe cu_t opens=1 examined=1 matched=1 sel=100% pages=2 index_scanned=1 index_resolved=1`
    - `DB time 0.03 ms = CPU 0.03`
