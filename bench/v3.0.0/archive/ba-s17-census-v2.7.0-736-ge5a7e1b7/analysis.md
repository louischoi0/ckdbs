B runs: 675 in 225 cells; S4 KDS runs: 675 in 225 cells; excluded as contaminated: 0

## Throughput per cell, B against BA-S4 (median of runs)

| shape | durability | cores | sessions | runs B | B stmt/s | S4 stmt/s | B vs S4 | B p50 | S4 p50 | B p99 | S4 p99 | B client-bound runs |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| insert1 | group | 1 | 1 | 3 | 851 stmt/s | 668 stmt/s | +27.4 % | 1,009 µs | 1,297 µs | 4,867 µs | 5,477 µs | 0 of 3 |
| insert1 | group | 1 | 2 | 3 | 861 stmt/s | 704 stmt/s | +22.3 % | 1,994 µs | 2,492 µs | 9,272 µs | 9,376 µs | 0 of 3 |
| insert1 | group | 1 | 4 | 3 | 1,687 stmt/s | 1,346 stmt/s | +25.3 % | 2,030 µs | 2,598 µs | 7,925 µs | 9,180 µs | 0 of 3 |
| insert1 | group | 1 | 8 | 3 | 3,237 stmt/s | 2,675 stmt/s | +21.0 % | 2,136 µs | 2,644 µs | 8,727 µs | 9,441 µs | 0 of 3 |
| insert1 | group | 1 | 16 | 3 | 5,921 stmt/s | 4,927 stmt/s | +20.2 % | 2,332 µs | 2,855 µs | 9,234 µs | 11,098 µs | 0 of 3 |
| insert1 | group | 2 | 1 | 3 | 842 stmt/s | 671 stmt/s | +25.5 % | 1,034 µs | 1,287 µs | 4,679 µs | 5,499 µs | 0 of 3 |
| insert1 | group | 2 | 2 | 3 | 915 stmt/s | 744 stmt/s | +22.9 % | 1,874 µs | 2,364 µs | 9,102 µs | 9,521 µs | 0 of 3 |
| insert1 | group | 2 | 4 | 3 | 1,467 stmt/s | 825 stmt/s | +77.8 % | 1,914 µs | 2,392 µs | 12,025 µs | 17,402 µs | 0 of 3 |
| insert1 | group | 2 | 8 | 3 | 2,494 stmt/s | 1,294 stmt/s | +92.8 % | 1,969 µs | 2,430 µs | 12,706 µs | 11,896 µs | 0 of 3 |
| insert1 | group | 2 | 16 | 3 | 6,208 stmt/s | 2,922 stmt/s | +112.5 % | 1,981 µs | 2,391 µs | 9,387 µs | 12,238 µs | 0 of 3 |
| insert1 | group | 4 | 1 | 3 | 819 stmt/s | 682 stmt/s | +20.2 % | 1,046 µs | 1,282 µs | 5,075 µs | 5,758 µs | 0 of 3 |
| insert1 | group | 4 | 2 | 3 | 926 stmt/s | 752 stmt/s | +23.1 % | 1,882 µs | 2,327 µs | 8,212 µs | 9,769 µs | 0 of 3 |
| insert1 | group | 4 | 4 | 3 | 1,808 stmt/s | 1,104 stmt/s | +63.7 % | 1,896 µs | 2,399 µs | 8,237 µs | 13,651 µs | 0 of 3 |
| insert1 | group | 4 | 8 | 3 | 3,418 stmt/s | 2,125 stmt/s | +60.8 % | 1,950 µs | 2,460 µs | 9,287 µs | 12,328 µs | 0 of 3 |
| insert1 | group | 4 | 16 | 3 | 6,328 stmt/s | 3,991 stmt/s | +58.5 % | 2,004 µs | 2,466 µs | 9,687 µs | 16,014 µs | 0 of 3 |
| insert1 | relaxed | 1 | 1 | 3 | 18,169 stmt/s | 17,799 stmt/s | +2.1 % | 49 µs | 49 µs | 72 µs | 71 µs | 0 of 3 |
| insert1 | relaxed | 1 | 2 | 3 | 31,053 stmt/s | 28,342 stmt/s | +9.6 % | 54 µs | 54 µs | 86 µs | 87 µs | 0 of 3 |
| insert1 | relaxed | 1 | 4 | 3 | 43,062 stmt/s | 38,884 stmt/s | +10.7 % | 75 µs | 75 µs | 119 µs | 114 µs | 0 of 3 |
| insert1 | relaxed | 1 | 8 | 3 | 42,516 stmt/s | 39,022 stmt/s | +9.0 % | 157 µs | 155 µs | 268 µs | 254 µs | 0 of 3 |
| insert1 | relaxed | 1 | 16 | 3 | 42,151 stmt/s | 38,166 stmt/s | +10.4 % | 259 µs | 254 µs | 630 µs | 570 µs | 0 of 3 |
| insert1 | relaxed | 2 | 1 | 3 | 18,062 stmt/s | 17,693 stmt/s | +2.1 % | 49 µs | 49 µs | 73 µs | 72 µs | 0 of 3 |
| insert1 | relaxed | 2 | 2 | 3 | 31,404 stmt/s | 28,389 stmt/s | +10.6 % | 54 µs | 54 µs | 84 µs | 86 µs | 0 of 3 |
| insert1 | relaxed | 2 | 4 | 3 | 51,469 stmt/s | 47,159 stmt/s | +9.1 % | 59 µs | 58 µs | 115 µs | 96 µs | 0 of 3 |
| insert1 | relaxed | 2 | 8 | 3 | 64,228 stmt/s | 54,525 stmt/s | +17.8 % | 95 µs | 111 µs | 181 µs | 205 µs | 0 of 3 |
| insert1 | relaxed | 2 | 16 | 3 | 69,538 stmt/s | 68,341 stmt/s | +1.8 % | 151 µs | 152 µs | 378 µs | 309 µs | 0 of 3 |
| insert1 | relaxed | 4 | 1 | 3 | 17,766 stmt/s | 17,318 stmt/s | +2.6 % | 50 µs | 50 µs | 74 µs | 72 µs | 0 of 3 |
| insert1 | relaxed | 4 | 2 | 3 | 29,527 stmt/s | 26,205 stmt/s | +12.7 % | 55 µs | 60 µs | 93 µs | 96 µs | 0 of 3 |
| insert1 | relaxed | 4 | 4 | 3 | 35,033 stmt/s | 36,352 stmt/s | -3.6 % | 82 µs | 73 µs | 292 µs | 190 µs | 0 of 3 |
| insert1 | relaxed | 4 | 8 | 3 | 50,028 stmt/s | 51,727 stmt/s | -3.3 % | 104 µs | 100 µs | 361 µs | 306 µs | 0 of 3 |
| insert1 | relaxed | 4 | 16 | 3 | 60,473 stmt/s | 56,560 stmt/s | +6.9 % | 150 µs | 135 µs | 1,315 µs | 1,162 µs | 3 of 3 |
| insert1 | strict | 1 | 1 | 3 | 809 stmt/s | 708 stmt/s | +14.3 % | 1,051 µs | 1,229 µs | 5,226 µs | 5,359 µs | 0 of 3 |
| insert1 | strict | 1 | 2 | 3 | 844 stmt/s | 721 stmt/s | +17.1 % | 2,004 µs | 2,436 µs | 9,054 µs | 9,711 µs | 0 of 3 |
| insert1 | strict | 1 | 4 | 3 | 872 stmt/s | 733 stmt/s | +19.0 % | 3,142 µs | 3,921 µs | 15,296 µs | 15,677 µs | 0 of 3 |
| insert1 | strict | 1 | 8 | 3 | 892 stmt/s | 712 stmt/s | +25.1 % | 7,030 µs | 8,978 µs | 23,315 µs | 27,430 µs | 0 of 3 |
| insert1 | strict | 1 | 16 | 3 | 873 stmt/s | 728 stmt/s | +20.0 % | 15,661 µs | 19,041 µs | 39,973 µs | 48,401 µs | 0 of 3 |
| insert1 | strict | 2 | 1 | 3 | 828 stmt/s | 702 stmt/s | +18.0 % | 1,036 µs | 1,256 µs | 4,914 µs | 5,342 µs | 0 of 3 |
| insert1 | strict | 2 | 2 | 3 | 946 stmt/s | 726 stmt/s | +30.2 % | 1,922 µs | 2,455 µs | 7,950 µs | 8,892 µs | 0 of 3 |
| insert1 | strict | 2 | 4 | 3 | 1,552 stmt/s | 739 stmt/s | +110.0 % | 2,031 µs | 4,818 µs | 9,483 µs | 16,919 µs | 0 of 3 |
| insert1 | strict | 2 | 8 | 3 | 3,189 stmt/s | 746 stmt/s | +327.8 % | 2,020 µs | 9,554 µs | 9,118 µs | 31,863 µs | 0 of 3 |
| insert1 | strict | 2 | 16 | 3 | 6,214 stmt/s | 703 stmt/s | +783.5 % | 2,076 µs | 18,951 µs | 9,607 µs | 53,426 µs | 0 of 3 |
| insert1 | strict | 4 | 1 | 3 | 834 stmt/s | 665 stmt/s | +25.5 % | 1,026 µs | 1,303 µs | 5,010 µs | 5,467 µs | 0 of 3 |
| insert1 | strict | 4 | 2 | 3 | 915 stmt/s | 696 stmt/s | +31.5 % | 1,896 µs | 2,492 µs | 7,910 µs | 10,494 µs | 0 of 3 |
| insert1 | strict | 4 | 4 | 3 | 1,701 stmt/s | 942 stmt/s | +80.5 % | 1,951 µs | 3,817 µs | 8,857 µs | 14,553 µs | 0 of 3 |
| insert1 | strict | 4 | 8 | 3 | 3,085 stmt/s | 1,175 stmt/s | +162.7 % | 2,054 µs | 5,977 µs | 9,573 µs | 19,980 µs | 0 of 3 |
| insert1 | strict | 4 | 16 | 3 | 6,014 stmt/s | 1,010 stmt/s | +495.6 % | 2,133 µs | 13,919 µs | 9,956 µs | 46,365 µs | 0 of 3 |
| insertN | group | 1 | 1 | 3 | 848 stmt/s | 701 stmt/s | +21.1 % | 1,005 µs | 1,257 µs | 4,948 µs | 5,474 µs | 0 of 3 |
| insertN | group | 1 | 2 | 3 | 862 stmt/s | 718 stmt/s | +20.0 % | 1,981 µs | 2,433 µs | 9,436 µs | 9,097 µs | 0 of 3 |
| insertN | group | 1 | 4 | 3 | 1,710 stmt/s | 1,376 stmt/s | +24.2 % | 2,038 µs | 2,546 µs | 8,554 µs | 10,010 µs | 0 of 3 |
| insertN | group | 1 | 8 | 3 | 3,310 stmt/s | 2,708 stmt/s | +22.2 % | 2,105 µs | 2,613 µs | 8,671 µs | 10,021 µs | 0 of 3 |
| insertN | group | 1 | 16 | 3 | 5,801 stmt/s | 4,918 stmt/s | +18.0 % | 2,389 µs | 2,821 µs | 9,256 µs | 10,569 µs | 0 of 3 |
| insertN | group | 2 | 1 | 3 | 834 stmt/s | 689 stmt/s | +21.0 % | 1,038 µs | 1,259 µs | 4,840 µs | 5,501 µs | 0 of 3 |
| insertN | group | 2 | 2 | 3 | 934 stmt/s | 717 stmt/s | +30.2 % | 1,889 µs | 2,413 µs | 8,073 µs | 10,143 µs | 0 of 3 |
| insertN | group | 2 | 4 | 3 | 1,518 stmt/s | 1,072 stmt/s | +41.7 % | 1,945 µs | 2,449 µs | 10,324 µs | 10,228 µs | 0 of 3 |
| insertN | group | 2 | 8 | 3 | 3,218 stmt/s | 1,441 stmt/s | +123.3 % | 1,967 µs | 2,426 µs | 9,054 µs | 12,266 µs | 0 of 3 |
| insertN | group | 2 | 16 | 3 | 6,102 stmt/s | 3,558 stmt/s | +71.5 % | 2,014 µs | 2,416 µs | 9,948 µs | 14,123 µs | 0 of 3 |
| insertN | group | 4 | 1 | 3 | 799 stmt/s | 687 stmt/s | +16.3 % | 1,052 µs | 1,260 µs | 5,618 µs | 5,783 µs | 0 of 3 |
| insertN | group | 4 | 2 | 3 | 908 stmt/s | 690 stmt/s | +31.6 % | 1,890 µs | 2,451 µs | 8,631 µs | 10,485 µs | 0 of 3 |
| insertN | group | 4 | 4 | 3 | 1,759 stmt/s | 1,440 stmt/s | +22.1 % | 1,955 µs | 2,393 µs | 9,071 µs | 9,734 µs | 0 of 3 |
| insertN | group | 4 | 8 | 3 | 3,324 stmt/s | 2,156 stmt/s | +54.2 % | 2,001 µs | 2,424 µs | 9,125 µs | 10,867 µs | 0 of 3 |
| insertN | group | 4 | 16 | 3 | 6,552 stmt/s | 4,226 stmt/s | +55.1 % | 2,009 µs | 2,428 µs | 8,981 µs | 13,752 µs | 0 of 3 |
| insertN | relaxed | 1 | 1 | 3 | 18,323 stmt/s | 17,955 stmt/s | +2.0 % | 49 µs | 49 µs | 72 µs | 70 µs | 0 of 3 |
| insertN | relaxed | 1 | 2 | 3 | 30,331 stmt/s | 28,463 stmt/s | +6.6 % | 54 µs | 54 µs | 86 µs | 86 µs | 0 of 3 |
| insertN | relaxed | 1 | 4 | 3 | 42,512 stmt/s | 38,410 stmt/s | +10.7 % | 76 µs | 75 µs | 122 µs | 117 µs | 0 of 3 |
| insertN | relaxed | 1 | 8 | 3 | 42,344 stmt/s | 38,691 stmt/s | +9.4 % | 158 µs | 155 µs | 274 µs | 252 µs | 0 of 3 |
| insertN | relaxed | 1 | 16 | 3 | 42,220 stmt/s | 37,721 stmt/s | +11.9 % | 262 µs | 256 µs | 636 µs | 589 µs | 0 of 3 |
| insertN | relaxed | 2 | 1 | 3 | 17,931 stmt/s | 17,582 stmt/s | +2.0 % | 50 µs | 49 µs | 74 µs | 72 µs | 0 of 3 |
| insertN | relaxed | 2 | 2 | 3 | 31,264 stmt/s | 28,798 stmt/s | +8.6 % | 54 µs | 54 µs | 85 µs | 83 µs | 0 of 3 |
| insertN | relaxed | 2 | 4 | 3 | 51,685 stmt/s | 46,430 stmt/s | +11.3 % | 56 µs | 56 µs | 114 µs | 116 µs | 0 of 3 |
| insertN | relaxed | 2 | 8 | 3 | 69,101 stmt/s | 60,343 stmt/s | +14.5 % | 83 µs | 80 µs | 171 µs | 168 µs | 0 of 3 |
| insertN | relaxed | 2 | 16 | 3 | 72,063 stmt/s | 64,647 stmt/s | +11.5 % | 152 µs | 153 µs | 323 µs | 324 µs | 0 of 3 |
| insertN | relaxed | 4 | 1 | 3 | 17,758 stmt/s | 17,344 stmt/s | +2.4 % | 50 µs | 50 µs | 75 µs | 73 µs | 0 of 3 |
| insertN | relaxed | 4 | 2 | 3 | 21,449 stmt/s | 24,560 stmt/s | -12.7 % | 83 µs | 61 µs | 132 µs | 102 µs | 0 of 3 |
| insertN | relaxed | 4 | 4 | 3 | 42,253 stmt/s | 35,718 stmt/s | +18.3 % | 69 µs | 81 µs | 147 µs | 217 µs | 0 of 3 |
| insertN | relaxed | 4 | 8 | 3 | 58,602 stmt/s | 46,171 stmt/s | +26.9 % | 97 µs | 95 µs | 300 µs | 417 µs | 1 of 3 |
| insertN | relaxed | 4 | 16 | 3 | 60,538 stmt/s | 53,013 stmt/s | +14.2 % | 117 µs | 115 µs | 1,375 µs | 1,096 µs | 2 of 3 |
| insertN | strict | 1 | 1 | 3 | 854 stmt/s | 688 stmt/s | +24.2 % | 1,010 µs | 1,281 µs | 4,992 µs | 5,397 µs | 0 of 3 |
| insertN | strict | 1 | 2 | 3 | 882 stmt/s | 729 stmt/s | +21.0 % | 1,962 µs | 2,441 µs | 8,125 µs | 9,021 µs | 0 of 3 |
| insertN | strict | 1 | 4 | 3 | 903 stmt/s | 744 stmt/s | +21.4 % | 3,061 µs | 3,847 µs | 13,549 µs | 15,714 µs | 0 of 3 |
| insertN | strict | 1 | 8 | 3 | 876 stmt/s | 740 stmt/s | +18.4 % | 7,077 µs | 8,626 µs | 23,559 µs | 28,042 µs | 0 of 3 |
| insertN | strict | 1 | 16 | 3 | 861 stmt/s | 726 stmt/s | +18.5 % | 15,833 µs | 19,157 µs | 41,370 µs | 49,557 µs | 0 of 3 |
| insertN | strict | 2 | 1 | 3 | 836 stmt/s | 706 stmt/s | +18.3 % | 1,028 µs | 1,237 µs | 4,962 µs | 5,305 µs | 0 of 3 |
| insertN | strict | 2 | 2 | 3 | 913 stmt/s | 715 stmt/s | +27.8 % | 1,931 µs | 2,464 µs | 7,889 µs | 9,699 µs | 0 of 3 |
| insertN | strict | 2 | 4 | 3 | 1,650 stmt/s | 767 stmt/s | +115.2 % | 1,999 µs | 4,333 µs | 9,489 µs | 16,606 µs | 0 of 3 |
| insertN | strict | 2 | 8 | 3 | 3,136 stmt/s | 756 stmt/s | +314.9 % | 2,030 µs | 9,494 µs | 9,288 µs | 30,657 µs | 0 of 3 |
| insertN | strict | 2 | 16 | 3 | 6,383 stmt/s | 752 stmt/s | +749.1 % | 2,051 µs | 18,746 µs | 8,991 µs | 52,532 µs | 0 of 3 |
| insertN | strict | 4 | 1 | 3 | 841 stmt/s | 692 stmt/s | +21.4 % | 1,025 µs | 1,271 µs | 4,736 µs | 5,500 µs | 0 of 3 |
| insertN | strict | 4 | 2 | 3 | 930 stmt/s | 747 stmt/s | +24.4 % | 1,886 µs | 2,377 µs | 7,825 µs | 9,353 µs | 0 of 3 |
| insertN | strict | 4 | 4 | 3 | 1,729 stmt/s | 1,008 stmt/s | +71.6 % | 1,942 µs | 3,660 µs | 9,014 µs | 12,944 µs | 0 of 3 |
| insertN | strict | 4 | 8 | 3 | 3,196 stmt/s | 1,126 stmt/s | +183.9 % | 2,028 µs | 5,062 µs | 9,740 µs | 21,408 µs | 0 of 3 |
| insertN | strict | 4 | 16 | 3 | 6,077 stmt/s | 985 stmt/s | +516.7 % | 2,113 µs | 14,271 µs | 9,828 µs | 48,358 µs | 0 of 3 |
| point | group | 1 | 1 | 3 | 14,253 stmt/s | 14,527 stmt/s | -1.9 % | 66 µs | 65 µs | 91 µs | 88 µs | 0 of 3 |
| point | group | 1 | 2 | 3 | 23,212 stmt/s | 23,644 stmt/s | -1.8 % | 81 µs | 80 µs | 113 µs | 109 µs | 0 of 3 |
| point | group | 1 | 4 | 3 | 42,442 stmt/s | 43,097 stmt/s | -1.5 % | 91 µs | 90 µs | 138 µs | 135 µs | 0 of 3 |
| point | group | 1 | 8 | 3 | 42,097 stmt/s | 44,498 stmt/s | -5.4 % | 176 µs | 170 µs | 289 µs | 261 µs | 0 of 3 |
| point | group | 1 | 16 | 3 | 41,944 stmt/s | 43,412 stmt/s | -3.4 % | 418 µs | 407 µs | 645 µs | 589 µs | 0 of 3 |
| point | group | 2 | 1 | 3 | 13,896 stmt/s | 14,144 stmt/s | -1.8 % | 67 µs | 66 µs | 93 µs | 84 µs | 0 of 3 |
| point | group | 2 | 2 | 3 | 21,291 stmt/s | 23,382 stmt/s | -8.9 % | 88 µs | 81 µs | 115 µs | 105 µs | 0 of 3 |
| point | group | 2 | 4 | 3 | 43,908 stmt/s | 44,469 stmt/s | -1.3 % | 83 µs | 84 µs | 117 µs | 122 µs | 0 of 3 |
| point | group | 2 | 8 | 3 | 56,745 stmt/s | 59,459 stmt/s | -4.6 % | 131 µs | 112 µs | 226 µs | 222 µs | 3 of 3 |
| point | group | 2 | 16 | 3 | 65,065 stmt/s | 65,331 stmt/s | -0.4 % | 232 µs | 234 µs | 427 µs | 428 µs | 3 of 3 |
| point | group | 4 | 1 | 3 | 13,726 stmt/s | 14,159 stmt/s | -3.1 % | 67 µs | 66 µs | 93 µs | 92 µs | 0 of 3 |
| point | group | 4 | 2 | 3 | 16,536 stmt/s | 21,071 stmt/s | -21.5 % | 113 µs | 91 µs | 141 µs | 116 µs | 0 of 3 |
| point | group | 4 | 4 | 3 | 35,178 stmt/s | 29,087 stmt/s | +20.9 % | 112 µs | 122 µs | 227 µs | 276 µs | 0 of 3 |
| point | group | 4 | 8 | 3 | 40,900 stmt/s | 41,032 stmt/s | -0.3 % | 151 µs | 154 µs | 483 µs | 498 µs | 3 of 3 |
| point | group | 4 | 16 | 3 | 49,417 stmt/s | 56,397 stmt/s | -12.4 % | 209 µs | 215 µs | 1,072 µs | 1,213 µs | 3 of 3 |
| point | relaxed | 1 | 1 | 3 | 14,092 stmt/s | 14,521 stmt/s | -3.0 % | 66 µs | 65 µs | 92 µs | 88 µs | 0 of 3 |
| point | relaxed | 1 | 2 | 3 | 23,070 stmt/s | 23,776 stmt/s | -3.0 % | 81 µs | 80 µs | 112 µs | 109 µs | 0 of 3 |
| point | relaxed | 1 | 4 | 3 | 43,458 stmt/s | 44,842 stmt/s | -3.1 % | 89 µs | 87 µs | 131 µs | 127 µs | 0 of 3 |
| point | relaxed | 1 | 8 | 3 | 43,158 stmt/s | 44,671 stmt/s | -3.4 % | 173 µs | 169 µs | 274 µs | 255 µs | 0 of 3 |
| point | relaxed | 1 | 16 | 3 | 41,560 stmt/s | 43,409 stmt/s | -4.3 % | 418 µs | 411 µs | 641 µs | 595 µs | 0 of 3 |
| point | relaxed | 2 | 1 | 3 | 13,840 stmt/s | 14,262 stmt/s | -3.0 % | 66 µs | 66 µs | 92 µs | 88 µs | 0 of 3 |
| point | relaxed | 2 | 2 | 3 | 21,401 stmt/s | 22,844 stmt/s | -6.3 % | 87 µs | 82 µs | 115 µs | 110 µs | 0 of 3 |
| point | relaxed | 2 | 4 | 3 | 41,917 stmt/s | 43,536 stmt/s | -3.7 % | 84 µs | 85 µs | 128 µs | 126 µs | 0 of 3 |
| point | relaxed | 2 | 8 | 3 | 59,073 stmt/s | 60,016 stmt/s | -1.6 % | 114 µs | 112 µs | 227 µs | 222 µs | 3 of 3 |
| point | relaxed | 2 | 16 | 3 | 64,908 stmt/s | 65,347 stmt/s | -0.7 % | 232 µs | 233 µs | 425 µs | 426 µs | 3 of 3 |
| point | relaxed | 4 | 1 | 3 | 13,237 stmt/s | 14,016 stmt/s | -5.6 % | 68 µs | 66 µs | 106 µs | 91 µs | 0 of 3 |
| point | relaxed | 4 | 2 | 3 | 18,900 stmt/s | 16,658 stmt/s | +13.5 % | 102 µs | 113 µs | 134 µs | 138 µs | 0 of 3 |
| point | relaxed | 4 | 4 | 3 | 34,772 stmt/s | 37,015 stmt/s | -6.1 % | 104 µs | 108 µs | 220 µs | 142 µs | 0 of 3 |
| point | relaxed | 4 | 8 | 3 | 48,309 stmt/s | 41,703 stmt/s | +15.8 % | 128 µs | 167 µs | 361 µs | 470 µs | 2 of 3 |
| point | relaxed | 4 | 16 | 3 | 53,816 stmt/s | 55,404 stmt/s | -2.9 % | 195 µs | 196 µs | 1,012 µs | 899 µs | 3 of 3 |
| point | strict | 1 | 1 | 3 | 13,988 stmt/s | 14,390 stmt/s | -2.8 % | 66 µs | 65 µs | 95 µs | 88 µs | 0 of 3 |
| point | strict | 1 | 2 | 3 | 23,128 stmt/s | 23,401 stmt/s | -1.2 % | 81 µs | 81 µs | 113 µs | 110 µs | 0 of 3 |
| point | strict | 1 | 4 | 3 | 43,145 stmt/s | 44,344 stmt/s | -2.7 % | 90 µs | 88 µs | 134 µs | 128 µs | 0 of 3 |
| point | strict | 1 | 8 | 3 | 42,952 stmt/s | 44,372 stmt/s | -3.2 % | 174 µs | 170 µs | 274 µs | 257 µs | 0 of 3 |
| point | strict | 1 | 16 | 3 | 42,066 stmt/s | 43,725 stmt/s | -3.8 % | 415 µs | 406 µs | 629 µs | 588 µs | 0 of 3 |
| point | strict | 2 | 1 | 3 | 13,919 stmt/s | 14,381 stmt/s | -3.2 % | 67 µs | 66 µs | 89 µs | 88 µs | 0 of 3 |
| point | strict | 2 | 2 | 3 | 22,519 stmt/s | 23,260 stmt/s | -3.2 % | 83 µs | 81 µs | 116 µs | 110 µs | 0 of 3 |
| point | strict | 2 | 4 | 3 | 42,660 stmt/s | 44,717 stmt/s | -4.6 % | 87 µs | 84 µs | 126 µs | 125 µs | 0 of 3 |
| point | strict | 2 | 8 | 3 | 58,901 stmt/s | 60,851 stmt/s | -3.2 % | 112 µs | 112 µs | 226 µs | 220 µs | 3 of 3 |
| point | strict | 2 | 16 | 3 | 65,094 stmt/s | 65,444 stmt/s | -0.5 % | 231 µs | 234 µs | 431 µs | 428 µs | 3 of 3 |
| point | strict | 4 | 1 | 3 | 13,671 stmt/s | 14,269 stmt/s | -4.2 % | 68 µs | 66 µs | 95 µs | 90 µs | 0 of 3 |
| point | strict | 4 | 2 | 3 | 20,933 stmt/s | 21,666 stmt/s | -3.4 % | 90 µs | 86 µs | 118 µs | 114 µs | 0 of 3 |
| point | strict | 4 | 4 | 3 | 27,911 stmt/s | 35,833 stmt/s | -22.1 % | 117 µs | 108 µs | 314 µs | 218 µs | 0 of 3 |
| point | strict | 4 | 8 | 3 | 40,593 stmt/s | 46,348 stmt/s | -12.4 % | 126 µs | 146 µs | 433 µs | 384 µs | 3 of 3 |
| point | strict | 4 | 16 | 3 | 55,290 stmt/s | 51,301 stmt/s | +7.8 % | 213 µs | 211 µs | 926 µs | 1,477 µs | 3 of 3 |
| recent | group | 1 | 1 | 3 | 1,540 stmt/s | 1,274 stmt/s | +20.9 % | 890 µs | 1,026 µs | 3,416 µs | 4,332 µs | 0 of 3 |
| recent | group | 1 | 2 | 3 | 1,633 stmt/s | 1,356 stmt/s | +20.4 % | 1,030 µs | 1,296 µs | 5,795 µs | 6,374 µs | 0 of 3 |
| recent | group | 1 | 4 | 3 | 2,196 stmt/s | 1,777 stmt/s | +23.6 % | 1,884 µs | 2,276 µs | 6,898 µs | 8,014 µs | 0 of 3 |
| recent | group | 1 | 8 | 3 | 4,110 stmt/s | 3,361 stmt/s | +22.3 % | 2,000 µs | 2,386 µs | 7,371 µs | 8,855 µs | 0 of 3 |
| recent | group | 1 | 16 | 3 | 7,226 stmt/s | 6,033 stmt/s | +19.8 % | 2,230 µs | 2,611 µs | 7,430 µs | 10,279 µs | 0 of 3 |
| recent | group | 2 | 1 | 3 | 1,521 stmt/s | 1,253 stmt/s | +21.4 % | 901 µs | 409 µs | 3,697 µs | 4,161 µs | 0 of 3 |
| recent | group | 2 | 2 | 3 | 1,797 stmt/s | 1,350 stmt/s | +33.1 % | 1,015 µs | 356 µs | 5,760 µs | 7,915 µs | 0 of 3 |
| recent | group | 2 | 4 | 3 | 3,558 stmt/s | 2,094 stmt/s | +69.9 % | 1,016 µs | 1,028 µs | 5,877 µs | 8,038 µs | 0 of 3 |
| recent | group | 2 | 8 | 3 | 6,114 stmt/s | 2,788 stmt/s | +119.3 % | 1,128 µs | 1,899 µs | 5,906 µs | 10,571 µs | 0 of 3 |
| recent | group | 2 | 16 | 3 | 12,102 stmt/s | 6,325 stmt/s | +91.3 % | 1,212 µs | 1,866 µs | 6,534 µs | 11,342 µs | 0 of 3 |
| recent | group | 4 | 1 | 3 | 1,496 stmt/s | 1,250 stmt/s | +19.7 % | 686 µs | 384 µs | 3,791 µs | 4,380 µs | 0 of 3 |
| recent | group | 4 | 2 | 3 | 1,817 stmt/s | 1,437 stmt/s | +26.4 % | 1,011 µs | 360 µs | 5,816 µs | 7,130 µs | 0 of 3 |
| recent | group | 4 | 4 | 3 | 3,525 stmt/s | 2,789 stmt/s | +26.4 % | 1,040 µs | 1,375 µs | 6,232 µs | 7,314 µs | 0 of 3 |
| recent | group | 4 | 8 | 3 | 6,792 stmt/s | 4,846 stmt/s | +40.2 % | 1,169 µs | 1,459 µs | 6,042 µs | 8,420 µs | 0 of 3 |
| recent | group | 4 | 16 | 3 | 12,627 stmt/s | 7,376 stmt/s | +71.2 % | 1,230 µs | 1,632 µs | 6,573 µs | 11,945 µs | 0 of 3 |
| recent | relaxed | 1 | 1 | 3 | 14,470 stmt/s | 14,344 stmt/s | +0.9 % | 67 µs | 67 µs | 93 µs | 90 µs | 0 of 3 |
| recent | relaxed | 1 | 2 | 3 | 23,036 stmt/s | 21,792 stmt/s | +5.7 % | 83 µs | 81 µs | 119 µs | 116 µs | 0 of 3 |
| recent | relaxed | 1 | 4 | 3 | 38,226 stmt/s | 33,341 stmt/s | +14.7 % | 91 µs | 90 µs | 145 µs | 135 µs | 0 of 3 |
| recent | relaxed | 1 | 8 | 3 | 38,549 stmt/s | 33,847 stmt/s | +13.9 % | 184 µs | 181 µs | 308 µs | 291 µs | 0 of 3 |
| recent | relaxed | 1 | 16 | 3 | 38,836 stmt/s | 34,206 stmt/s | +13.5 % | 316 µs | 342 µs | 655 µs | 616 µs | 0 of 3 |
| recent | relaxed | 2 | 1 | 3 | 14,293 stmt/s | 14,044 stmt/s | +1.8 % | 68 µs | 67 µs | 94 µs | 88 µs | 0 of 3 |
| recent | relaxed | 2 | 2 | 3 | 21,768 stmt/s | 21,064 stmt/s | +3.3 % | 86 µs | 84 µs | 118 µs | 117 µs | 0 of 3 |
| recent | relaxed | 2 | 4 | 3 | 40,887 stmt/s | 37,931 stmt/s | +7.8 % | 84 µs | 83 µs | 151 µs | 126 µs | 0 of 3 |
| recent | relaxed | 2 | 8 | 3 | 59,680 stmt/s | 47,495 stmt/s | +25.7 % | 108 µs | 108 µs | 219 µs | 225 µs | 3 of 3 |
| recent | relaxed | 2 | 16 | 3 | 66,407 stmt/s | 53,233 stmt/s | +24.7 % | 194 µs | 191 µs | 396 µs | 398 µs | 3 of 3 |
| recent | relaxed | 4 | 1 | 3 | 13,908 stmt/s | 13,785 stmt/s | +0.9 % | 68 µs | 69 µs | 95 µs | 93 µs | 0 of 3 |
| recent | relaxed | 4 | 2 | 3 | 21,573 stmt/s | 17,267 stmt/s | +24.9 % | 86 µs | 110 µs | 125 µs | 178 µs | 0 of 3 |
| recent | relaxed | 4 | 4 | 3 | 34,192 stmt/s | 26,473 stmt/s | +29.2 % | 93 µs | 108 µs | 212 µs | 299 µs | 0 of 3 |
| recent | relaxed | 4 | 8 | 3 | 39,274 stmt/s | 38,962 stmt/s | +0.8 % | 137 µs | 148 µs | 461 µs | 408 µs | 1 of 3 |
| recent | relaxed | 4 | 16 | 3 | 50,882 stmt/s | 46,590 stmt/s | +9.2 % | 172 µs | 182 µs | 1,097 µs | 1,412 µs | 3 of 3 |
| recent | strict | 1 | 1 | 3 | 1,474 stmt/s | 1,248 stmt/s | +18.1 % | 910 µs | 1,035 µs | 4,072 µs | 4,604 µs | 0 of 3 |
| recent | strict | 1 | 2 | 3 | 1,573 stmt/s | 1,337 stmt/s | +17.7 % | 1,060 µs | 1,294 µs | 5,831 µs | 6,657 µs | 0 of 3 |
| recent | strict | 1 | 4 | 3 | 1,687 stmt/s | 1,371 stmt/s | +23.0 % | 2,061 µs | 2,568 µs | 9,036 µs | 10,647 µs | 0 of 3 |
| recent | strict | 1 | 8 | 3 | 1,719 stmt/s | 1,372 stmt/s | +25.3 % | 4,059 µs | 5,226 µs | 15,180 µs | 18,407 µs | 0 of 3 |
| recent | strict | 1 | 16 | 3 | 1,708 stmt/s | 1,336 stmt/s | +27.8 % | 8,200 µs | 10,512 µs | 24,879 µs | 31,378 µs | 0 of 3 |
| recent | strict | 2 | 1 | 3 | 1,520 stmt/s | 1,248 stmt/s | +21.8 % | 912 µs | 1,052 µs | 3,660 µs | 4,619 µs | 0 of 3 |
| recent | strict | 2 | 2 | 3 | 1,685 stmt/s | 1,330 stmt/s | +26.7 % | 1,029 µs | 1,285 µs | 5,522 µs | 6,348 µs | 0 of 3 |
| recent | strict | 2 | 4 | 3 | 2,871 stmt/s | 1,376 stmt/s | +108.6 % | 1,093 µs | 2,453 µs | 6,146 µs | 11,155 µs | 0 of 3 |
| recent | strict | 2 | 8 | 3 | 5,727 stmt/s | 1,326 stmt/s | +331.9 % | 1,130 µs | 4,443 µs | 6,471 µs | 26,035 µs | 0 of 3 |
| recent | strict | 2 | 16 | 3 | 11,382 stmt/s | 1,472 stmt/s | +673.1 % | 1,152 µs | 9,933 µs | 6,624 µs | 30,700 µs | 0 of 3 |
| recent | strict | 4 | 1 | 3 | 1,502 stmt/s | 1,252 stmt/s | +19.9 % | 904 µs | 1,065 µs | 3,775 µs | 4,392 µs | 0 of 3 |
| recent | strict | 4 | 2 | 3 | 1,672 stmt/s | 1,349 stmt/s | +24.0 % | 1,023 µs | 1,284 µs | 5,586 µs | 6,598 µs | 0 of 3 |
| recent | strict | 4 | 4 | 3 | 3,020 stmt/s | 1,814 stmt/s | +66.4 % | 1,069 µs | 2,196 µs | 5,994 µs | 10,017 µs | 0 of 3 |
| recent | strict | 4 | 8 | 3 | 5,667 stmt/s | 1,867 stmt/s | +203.6 % | 1,153 µs | 3,427 µs | 6,302 µs | 16,606 µs | 0 of 3 |
| recent | strict | 4 | 16 | 3 | 10,102 stmt/s | 1,867 stmt/s | +441.2 % | 1,380 µs | 7,232 µs | 6,902 µs | 23,907 µs | 0 of 3 |
| trade | group | 1 | 1 | 3 | 819 stmt/s | 676 stmt/s | +21.2 % | 1,039 µs | 1,293 µs | 5,053 µs | 5,617 µs | 0 of 3 |
| trade | group | 1 | 2 | 3 | 873 stmt/s | 712 stmt/s | +22.6 % | 1,988 µs | 2,494 µs | 8,320 µs | 9,695 µs | 0 of 3 |
| trade | group | 1 | 4 | 3 | 1,687 stmt/s | 1,368 stmt/s | +23.4 % | 2,030 µs | 2,588 µs | 8,944 µs | 9,714 µs | 0 of 3 |
| trade | group | 1 | 8 | 3 | 3,020 stmt/s | 2,622 stmt/s | +15.2 % | 2,216 µs | 2,672 µs | 8,704 µs | 10,466 µs | 0 of 3 |
| trade | group | 1 | 16 | 3 | 5,569 stmt/s | 4,977 stmt/s | +11.9 % | 2,454 µs | 2,847 µs | 9,787 µs | 9,907 µs | 0 of 3 |
| trade | group | 2 | 1 | 3 | 792 stmt/s | 657 stmt/s | +20.5 % | 1,070 µs | 1,326 µs | 5,042 µs | 6,010 µs | 0 of 3 |
| trade | group | 2 | 2 | 3 | 915 stmt/s | 722 stmt/s | +26.6 % | 1,900 µs | 2,415 µs | 8,521 µs | 10,024 µs | 0 of 3 |
| trade | group | 2 | 4 | 3 | 1,781 stmt/s | 811 stmt/s | +119.7 % | 1,896 µs | 2,516 µs | 7,753 µs | 11,101 µs | 0 of 3 |
| trade | group | 2 | 8 | 3 | 3,418 stmt/s | 1,862 stmt/s | +83.5 % | 1,928 µs | 2,577 µs | 7,896 µs | 13,414 µs | 0 of 3 |
| trade | group | 2 | 16 | 3 | 6,606 stmt/s | 1,656 stmt/s | +299.0 % | 1,995 µs | 2,571 µs | 8,643 µs | 15,314 µs | 0 of 3 |
| trade | group | 4 | 1 | 3 | 793 stmt/s | 654 stmt/s | +21.2 % | 1,067 µs | 1,310 µs | 4,981 µs | 5,818 µs | 0 of 3 |
| trade | group | 4 | 2 | 3 | 856 stmt/s | 720 stmt/s | +18.8 % | 1,935 µs | 2,417 µs | 9,104 µs | 9,905 µs | 0 of 3 |
| trade | group | 4 | 4 | 3 | 1,747 stmt/s | 1,434 stmt/s | +21.8 % | 1,904 µs | 2,429 µs | 8,647 µs | 10,200 µs | 0 of 3 |
| trade | group | 4 | 8 | 3 | 3,428 stmt/s | 2,125 stmt/s | +61.3 % | 1,985 µs | 2,453 µs | 8,200 µs | 11,009 µs | 0 of 3 |
| trade | group | 4 | 16 | 3 | 6,793 stmt/s | 3,905 stmt/s | +74.0 % | 2,002 µs | 2,428 µs | 8,354 µs | 13,706 µs | 0 of 3 |
| trade | relaxed | 1 | 1 | 3 | 16,588 stmt/s | 16,436 stmt/s | +0.9 % | 53 µs | 53 µs | 78 µs | 75 µs | 0 of 3 |
| trade | relaxed | 1 | 2 | 3 | 27,161 stmt/s | 27,429 stmt/s | -1.0 % | 60 µs | 59 µs | 101 µs | 98 µs | 0 of 3 |
| trade | relaxed | 1 | 4 | 3 | 38,757 stmt/s | 37,600 stmt/s | +3.1 % | 85 µs | 84 µs | 134 µs | 128 µs | 0 of 3 |
| trade | relaxed | 1 | 8 | 3 | 38,552 stmt/s | 37,323 stmt/s | +3.3 % | 187 µs | 182 µs | 304 µs | 287 µs | 0 of 3 |
| trade | relaxed | 1 | 16 | 3 | 38,426 stmt/s | 37,118 stmt/s | +3.5 % | 287 µs | 280 µs | 687 µs | 638 µs | 0 of 3 |
| trade | relaxed | 2 | 1 | 3 | 16,140 stmt/s | 16,110 stmt/s | +0.2 % | 54 µs | 54 µs | 78 µs | 76 µs | 0 of 3 |
| trade | relaxed | 2 | 2 | 3 | 26,695 stmt/s | 26,642 stmt/s | +0.2 % | 64 µs | 62 µs | 94 µs | 92 µs | 0 of 3 |
| trade | relaxed | 2 | 4 | 3 | 47,445 stmt/s | 41,920 stmt/s | +13.2 % | 66 µs | 65 µs | 144 µs | 124 µs | 0 of 3 |
| trade | relaxed | 2 | 8 | 3 | 64,704 stmt/s | 53,805 stmt/s | +20.3 % | 93 µs | 100 µs | 191 µs | 196 µs | 0 of 3 |
| trade | relaxed | 2 | 16 | 3 | 63,915 stmt/s | 60,113 stmt/s | +6.3 % | 172 µs | 168 µs | 502 µs | 362 µs | 1 of 3 |
| trade | relaxed | 4 | 1 | 3 | 16,112 stmt/s | 15,905 stmt/s | +1.3 % | 55 µs | 54 µs | 80 µs | 78 µs | 0 of 3 |
| trade | relaxed | 4 | 2 | 3 | 26,211 stmt/s | 26,016 stmt/s | +0.7 % | 64 µs | 65 µs | 103 µs | 97 µs | 0 of 3 |
| trade | relaxed | 4 | 4 | 3 | 27,498 stmt/s | 28,568 stmt/s | -3.7 % | 127 µs | 130 µs | 256 µs | 296 µs | 0 of 3 |
| trade | relaxed | 4 | 8 | 3 | 42,767 stmt/s | 41,876 stmt/s | +2.1 % | 111 µs | 109 µs | 900 µs | 461 µs | 0 of 3 |
| trade | relaxed | 4 | 16 | 3 | 49,903 stmt/s | 51,024 stmt/s | -2.2 % | 133 µs | 134 µs | 2,311 µs | 1,192 µs | 2 of 3 |
| trade | strict | 1 | 1 | 3 | 808 stmt/s | 680 stmt/s | +18.8 % | 1,036 µs | 1,294 µs | 5,230 µs | 5,658 µs | 0 of 3 |
| trade | strict | 1 | 2 | 3 | 860 stmt/s | 695 stmt/s | +23.7 % | 1,982 µs | 2,536 µs | 8,393 µs | 9,091 µs | 0 of 3 |
| trade | strict | 1 | 4 | 3 | 842 stmt/s | 700 stmt/s | +20.4 % | 3,289 µs | 4,085 µs | 15,465 µs | 17,202 µs | 0 of 3 |
| trade | strict | 1 | 8 | 3 | 874 stmt/s | 690 stmt/s | +26.8 % | 7,144 µs | 9,275 µs | 25,625 µs | 28,613 µs | 0 of 3 |
| trade | strict | 1 | 16 | 3 | 862 stmt/s | 684 stmt/s | +26.1 % | 15,515 µs | 20,170 µs | 42,525 µs | 51,472 µs | 0 of 3 |
| trade | strict | 2 | 1 | 3 | 825 stmt/s | 653 stmt/s | +26.4 % | 1,042 µs | 1,320 µs | 4,794 µs | 5,891 µs | 0 of 3 |
| trade | strict | 2 | 2 | 3 | 926 stmt/s | 682 stmt/s | +35.7 % | 1,897 µs | 2,592 µs | 8,114 µs | 9,620 µs | 0 of 3 |
| trade | strict | 2 | 4 | 3 | 1,635 stmt/s | 717 stmt/s | +128.0 % | 1,985 µs | 4,961 µs | 9,175 µs | 15,781 µs | 0 of 3 |
| trade | strict | 2 | 8 | 3 | 3,026 stmt/s | 729 stmt/s | +315.2 % | 2,061 µs | 9,677 µs | 9,636 µs | 29,807 µs | 0 of 3 |
| trade | strict | 2 | 16 | 3 | 6,239 stmt/s | 721 stmt/s | +765.2 % | 2,081 µs | 19,474 µs | 10,167 µs | 51,848 µs | 0 of 3 |
| trade | strict | 4 | 1 | 3 | 813 stmt/s | 655 stmt/s | +24.1 % | 1,057 µs | 1,318 µs | 5,028 µs | 5,553 µs | 0 of 3 |
| trade | strict | 4 | 2 | 3 | 887 stmt/s | 717 stmt/s | +23.7 % | 1,953 µs | 2,457 µs | 8,880 µs | 9,493 µs | 0 of 3 |
| trade | strict | 4 | 4 | 3 | 1,619 stmt/s | 1,000 stmt/s | +61.8 % | 1,990 µs | 3,709 µs | 9,036 µs | 12,767 µs | 0 of 3 |
| trade | strict | 4 | 8 | 3 | 3,156 stmt/s | 1,133 stmt/s | +178.6 % | 2,043 µs | 5,724 µs | 9,526 µs | 22,337 µs | 0 of 3 |
| trade | strict | 4 | 16 | 3 | 5,767 stmt/s | 857 stmt/s | +573.0 % | 2,202 µs | 16,545 µs | 10,189 µs | 51,460 µs | 0 of 3 |

## Item shares, B (contended wait over cores x span; median of runs per cell)

| item | max share, all cells | cell | max share, clean cells (cores <= 2) | clean cell |
|---|---|---|---|---|
| P1 frame table | 0.86 % | point relaxed k4 s16 | 0.49 % | recent relaxed k2 s16 |
| P2 window | 2.66 % | trade relaxed k4 s16 | 0.38 % | insertN relaxed k2 s16 |
| P3 statistics | 0.00 % | point relaxed k4 s8 | 0.00 % | point group k2 s16 |
| P4 relation borrow | 0.10 % | insert1 relaxed k4 s16 | 0.05 % | insert1 relaxed k2 s16 |
| P6 WAL append | 0.49 % | insertN relaxed k4 s16 | 0.38 % | insertN relaxed k2 s16 |
| P7 sync gate | 9.60 % | insert1 group k2 s8 | 9.60 % | insert1 group k2 s8 |
| P7 snapshot ceiling | 113.48 % | insert1 strict k2 s16 | 113.48 % | insert1 strict k2 s16 |
| P8 assertion dir | 0.00 % | insertN relaxed k4 s16 | 0.00 % | insert1 relaxed k2 s8 |
| superblock | 0.00 % | insert1 group k1 s1 | 0.00 % | insert1 group k1 s1 |

## Item shares, S4 (contended wait over cores x span; median of runs per cell)

| item | max share, all cells | cell | max share, clean cells (cores <= 2) | clean cell |
|---|---|---|---|---|
| P1 frame table | 0.91 % | recent relaxed k4 s16 | 0.67 % | recent relaxed k2 s16 |
| P2 window | 1.80 % | insertN relaxed k4 s16 | 0.26 % | insertN relaxed k2 s16 |
| P3 statistics | 0.00 % | point group k4 s16 | 0.00 % | point group k2 s16 |
| P4 relation borrow | 0.09 % | insert1 relaxed k4 s16 | 0.06 % | insert1 relaxed k2 s4 |
| P6 WAL append | 11.36 % | insertN relaxed k4 s16 | 10.01 % | insertN relaxed k2 s16 |
| P7 sync gate | 55.35 % | recent group k2 s8 | 55.35 % | recent group k2 s8 |
| P7 snapshot ceiling | 39.44 % | recent strict k4 s16 | 39.33 % | recent strict k2 s4 |
| P8 assertion dir | 0.00 % | trade relaxed k4 s16 | 0.00 % | insertN relaxed k2 s8 |
| superblock | 0.00 % | insert1 group k1 s1 | 0.00 % | insert1 group k1 s1 |

## P7 shares by durability (max over cells with cores >= 2 of the median share), B and S4

| item | durability | B max | B cell | S4 max | S4 cell |
|---|---|---|---|---|---|
| P7 sync gate | relaxed | 0.04 % | recent k2 s8 | 1.47 % | trade k4 s16 |
| P7 sync gate | group | 9.60 % | insert1 k2 s8 | 55.35 % | recent k2 s8 |
| P7 sync gate | strict | 0.10 % | insert1 k4 s2 | 47.96 % | insert1 k2 s16 |
| P7 snapshot ceiling | relaxed | 0.16 % | insertN k2 s16 | 0.09 % | insert1 k4 s16 |
| P7 snapshot ceiling | group | 0.00 % | insert1 k2 s1 | 0.00 % | insert1 k2 s1 |
| P7 snapshot ceiling | strict | 113.48 % | insert1 k2 s16 | 39.44 % | recent k4 s16 |
| P6 WAL append | relaxed | 0.49 % | insertN k4 s16 | 11.36 % | insertN k4 s16 |
| P6 WAL append | group | 0.01 % | insertN k4 s16 | 6.28 % | insertN k4 s16 |
| P6 WAL append | strict | 0.01 % | insertN k4 s16 | 0.30 % | insert1 k4 s8 |

## P12: carve and checkpoint, B

- `contention_carve_longest_us` (max over each server's life): max 352.4 ms, median 6.36 ms, p99 over runs 330.2 ms; runs over 10 ms: 243 of 675
- `contention_checkpoint_longest_us` (max over each server's life): max 1,555.3 ms, median 92.07 ms, p99 over runs 996.0 ms; runs over 10 ms: 648 of 675
- S4 `contention_carve_longest_us`: max 2,242.1 ms, median 10.06 ms; runs over 10 ms: 340 of 675
- S4 `contention_checkpoint_longest_us`: max 3,093.7 ms, median 46.52 ms; runs over 10 ms: 596 of 675

Carve longest by (durability, cores), B, max and median over runs (ms):

| durability | cores | runs | carve max | carve median | carve runs > 10 ms | checkpoint max | checkpoint median | checkpoint runs > 10 ms |
|---|---|---|---|---|---|---|---|---|
| group | 1 | 75 | 19.2 ms | 5.43 ms | 6 | 199.1 ms | 21.32 ms | 63 |
| group | 2 | 75 | 30.7 ms | 5.02 ms | 11 | 1,555.3 ms | 200.48 ms | 75 |
| group | 4 | 75 | 230.2 ms | 5.24 ms | 18 | 348.4 ms | 67.52 ms | 75 |
| relaxed | 1 | 75 | 296.2 ms | 225.55 ms | 62 | 379.1 ms | 122.83 ms | 75 |
| relaxed | 2 | 75 | 333.7 ms | 257.90 ms | 59 | 510.5 ms | 187.92 ms | 75 |
| relaxed | 4 | 75 | 352.4 ms | 251.54 ms | 59 | 647.4 ms | 209.06 ms | 75 |
| strict | 1 | 75 | 17.4 ms | 3.56 ms | 6 | 210.2 ms | 15.85 ms | 60 |
| strict | 2 | 75 | 16.4 ms | 4.74 ms | 10 | 247.6 ms | 28.68 ms | 75 |
| strict | 4 | 75 | 48.1 ms | 5.87 ms | 12 | 441.5 ms | 34.62 ms | 75 |

Carves and checkpoint runs inside the measured span (sum over runs), B: carves 16,345, checkpoint runs 2,393

## Refusals and re-runs, B

- `contention_refused_btree_descend`: 29
- `contention_refused_btree_lookup`: 0
- `contention_refused_btree_parent`: 0
- `contention_refused_btree_secure`: 0
- `contention_refused_index_descend`: 0
- `contention_refused_index_parent`: 0
- `contention_refused_index_secure`: 0
- `contention_structural_reruns`: 29
- client-visible `refusals` field (TXN_CONFLICT retried by the driver): 0; errors: 0; first_error values: []

Structural re-runs by (shape, durability, cores):

- ('insert1', 'relaxed', 2): 2
- ('insert1', 'relaxed', 4): 7
- ('recent', 'relaxed', 2): 2
- ('recent', 'relaxed', 4): 3
- ('trade', 'relaxed', 2): 5
- ('trade', 'relaxed', 4): 10

S4 refusals (client-visible `refusals`) for comparison: 20; S4 contention_refused_*: {'contention_refused_btree_descend': 20.0}

## BA-R4's premise: a session's latency by its core, cores = 2

| census | shape | durability | core 0 p50 | core 0 p99 | peer p50 | peer p99 | clients core 0 / peers | statements per client core 0 / peers |
|---|---|---|---|---|---|---|---|---|
| B | insert1 | group | 1,967 µs | 9,663 µs | 1,965 µs | 9,554 µs | 48 / 45 | 2933.5 / 3173 |
| B | insert1 | strict | 2,065 µs | 9,739 µs | 2,047 µs | 9,262 µs | 45 / 48 | 3127 / 3128.0 |
| B | insertN | group | 1,996 µs | 9,631 µs | 2,000 µs | 9,984 µs | 48 / 45 | 2961.0 / 3298 |
| B | insertN | strict | 2,027 µs | 9,178 µs | 2,030 µs | 9,197 µs | 42 / 51 | 3193.5 / 3195 |
| B | trade | group | 1,985 µs | 8,511 µs | 1,934 µs | 8,469 µs | 47 / 46 | 3252 / 3504.0 |
| B | trade | strict | 2,055 µs | 9,650 µs | 2,061 µs | 9,847 µs | 30 / 63 | 3124.0 / 3124 |
| S4 | insert1 | group | 12,906 µs | 1,395,464 µs | 2,388 µs | 9,507 µs | 47 / 46 | 44 / 2899.5 |
| S4 | insert1 | strict | 17,891 µs | 36,927 µs | 17,750 µs | 35,903 µs | 41 / 52 | 412 / 411.0 |
| S4 | insertN | group | 11,451 µs | 1,300,255 µs | 2,409 µs | 9,897 µs | 39 / 54 | 28 / 2861.0 |
| S4 | insertN | strict | 14,162 µs | 33,692 µs | 14,485 µs | 34,022 µs | 43 / 50 | 510 / 499.0 |
| S4 | trade | group | 42,667 µs | 1,638,382 µs | 2,533 µs | 9,901 µs | 49 / 44 | 16 / 2764.0 |
| S4 | trade | strict | 15,067 µs | 33,122 µs | 17,794 µs | 35,675 µs | 43 / 50 | 480 / 412.0 |

## P11 proxy: runs with a session on every core against the rest, same cell

- B: 36 cells with both kinds of run; every-core runs faster by over 10 % in 8, slower by over 10 % in 5; median gain -0.7 %
  - B group: 13 cells, median -0.2 %, faster>10%: 1, slower>10%: 2
  - B relaxed: 11 cells, median +0.2 %, faster>10%: 4, slower>10%: 1
  - B strict: 12 cells, median -1.8 %, faster>10%: 3, slower>10%: 2
- S4: 30 cells with both kinds of run; every-core runs faster by over 10 % in 8, slower by over 10 % in 3; median gain +2.2 %
  - S4 group: 10 cells, median -1.0 %, faster>10%: 3, slower>10%: 2
  - S4 relaxed: 11 cells, median +0.8 %, faster>10%: 2, slower>10%: 1
  - S4 strict: 9 cells, median +6.0 %, faster>10%: 3, slower>10%: 0

Between-run spread (max/min - 1) of statements/s inside a cell, median over cells: B 3.8 %, S4 3.5 %

## Synced write shapes at 16 sessions: B, S4, PostgreSQL `on`

| shape | durability | B cores=1 | B cores=2 | B cores=4 | S4 cores=1 | S4 cores=2 | S4 cores=4 | PG on, CPUs 0 | PG on, CPUs 0,2 | PG on, CPUs 0,2,4,6 |
|---|---|---|---|---|---|---|---|---|---|---|
| trade | group | 5,569 stmt/s | 6,606 stmt/s | 6,793 stmt/s | 4,977 stmt/s | 1,656 stmt/s | 3,905 stmt/s | 4,902 stmt/s | 5,150 stmt/s | 5,286 stmt/s |
| trade | strict | 862 stmt/s | 6,239 stmt/s | 5,767 stmt/s | 684 stmt/s | 721 stmt/s | 857 stmt/s | 4,902 stmt/s | 5,150 stmt/s | 5,286 stmt/s |
| insert1 | group | 5,921 stmt/s | 6,208 stmt/s | 6,328 stmt/s | 4,927 stmt/s | 2,922 stmt/s | 3,991 stmt/s | 5,120 stmt/s | 5,325 stmt/s | 5,220 stmt/s |
| insert1 | strict | 873 stmt/s | 6,214 stmt/s | 6,014 stmt/s | 728 stmt/s | 703 stmt/s | 1,010 stmt/s | 5,120 stmt/s | 5,325 stmt/s | 5,220 stmt/s |
| insertN | group | 5,801 stmt/s | 6,102 stmt/s | 6,552 stmt/s | 4,918 stmt/s | 3,558 stmt/s | 4,226 stmt/s | 4,857 stmt/s | 5,035 stmt/s | 5,173 stmt/s |
| insertN | strict | 861 stmt/s | 6,383 stmt/s | 6,077 stmt/s | 726 stmt/s | 752 stmt/s | 985 stmt/s | 4,857 stmt/s | 5,035 stmt/s | 5,173 stmt/s |
| recent | group | 7,226 stmt/s | 12,102 stmt/s | 12,627 stmt/s | 6,033 stmt/s | 6,325 stmt/s | 7,376 stmt/s | 9,733 stmt/s | 9,942 stmt/s | 10,171 stmt/s |
| recent | strict | 1,708 stmt/s | 11,382 stmt/s | 10,102 stmt/s | 1,336 stmt/s | 1,472 stmt/s | 1,867 stmt/s | 9,733 stmt/s | 9,942 stmt/s | 10,171 stmt/s |

Unsynced (relaxed) at 16 sessions: B / S4 / PG off

| shape | B k1 | B k2 | B k4 | S4 k1 | S4 k2 | S4 k4 | PG off CPUs 0 | PG off 0,2 | PG off 0,2,4,6 |
|---|---|---|---|---|---|---|---|---|---|
| point | 41,560 stmt/s | 64,908 stmt/s | 53,816 stmt/s | 43,409 stmt/s | 65,347 stmt/s | 55,404 stmt/s | 26,960 stmt/s | 54,433 stmt/s | 80,327 stmt/s |
| trade | 38,426 stmt/s | 63,915 stmt/s | 49,903 stmt/s | 37,118 stmt/s | 60,113 stmt/s | 51,024 stmt/s | 18,540 stmt/s | 37,593 stmt/s | 55,594 stmt/s |
| insert1 | 42,151 stmt/s | 69,538 stmt/s | 60,473 stmt/s | 38,166 stmt/s | 68,341 stmt/s | 56,560 stmt/s | 27,186 stmt/s | 54,628 stmt/s | 77,684 stmt/s |
| insertN | 42,220 stmt/s | 72,063 stmt/s | 60,538 stmt/s | 37,721 stmt/s | 64,647 stmt/s | 53,013 stmt/s | 26,933 stmt/s | 54,282 stmt/s | 79,473 stmt/s |
| recent | 38,836 stmt/s | 66,407 stmt/s | 50,882 stmt/s | 34,206 stmt/s | 53,233 stmt/s | 46,590 stmt/s | 21,652 stmt/s | 43,880 stmt/s | 63,365 stmt/s |

## Other counters (sum over runs), B and S4

- B: `contention_carves` 16,345; `contention_checkpoint_runs` 2,393; `contention_drain_passes_pending` 15,746,932; `contention_page_latch_spin_turns` 90,213,311; `contention_refused_btree_descend` 29; `contention_refused_btree_lookup` 0; `contention_refused_btree_parent` 0; `contention_refused_btree_secure` 0; `contention_refused_index_descend` 0; `contention_refused_index_parent` 0; `contention_refused_index_secure` 0; `contention_structural_reruns` 29; `contention_syncs_inline` 796,261; `contention_syncs_writer` 1,852,504
  - wal sync counters: {'wal_interval_syncs': 174552.0, 'wal_sync_failures': 0.0, 'wal_syncs': 1689954.0}
- S4: `contention_carves` 13,786; `contention_checkpoint_runs` 2,356; `contention_drain_passes_pending` 2,197,224,334; `contention_page_latch_spin_turns` 98,466,594; `contention_refused_btree_descend` 20; `contention_refused_btree_lookup` 0; `contention_refused_btree_parent` 0; `contention_refused_btree_secure` 0; `contention_refused_index_descend` 0; `contention_refused_index_parent` 0; `contention_refused_index_secure` 0; `contention_syncs_inline` 991,774; `contention_syncs_writer` 1,334,172
  - wal sync counters: {'wal_interval_syncs': 152214.0, 'wal_sync_failures': 0.0, 'wal_syncs': 1324493.0}

## Host

- B runs: 675; 1-min load before a run: median 2.00, max 6.00; wait_s max 40
- binaries: ['d2a6977f220a32147b6a2df985aec10b60ee7411fe3fca3381b78e46e525223e']; devices: ['/dev/root ext4']
