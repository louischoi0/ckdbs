## Throughput per cell (median of runs)

| engine | shape | durability | place | sessions | runs | statements/s | p50 | p99 | refusals | client-bound runs |
|---|---|---|---|---|---|---|---|---|---|---|
| kds | insert1 | group | k1 | 1 sessions | 3 runs | 668 stmt/s | 1,296.6 µs | 5,476.9 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k1 | 2 sessions | 3 runs | 704 stmt/s | 2,492.4 µs | 9,376.5 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k1 | 4 sessions | 3 runs | 1,346 stmt/s | 2,597.5 µs | 9,180.0 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k1 | 8 sessions | 3 runs | 2,675 stmt/s | 2,643.6 µs | 9,440.9 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k1 | 16 sessions | 3 runs | 4,927 stmt/s | 2,854.9 µs | 11,097.8 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k2 | 1 sessions | 3 runs | 671 stmt/s | 1,286.9 µs | 5,499.0 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k2 | 2 sessions | 3 runs | 744 stmt/s | 2,363.5 µs | 9,520.6 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k2 | 4 sessions | 3 runs | 825 stmt/s | 2,392.1 µs | 17,401.6 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k2 | 8 sessions | 3 runs | 1,294 stmt/s | 2,430.0 µs | 11,895.7 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k2 | 16 sessions | 3 runs | 2,922 stmt/s | 2,390.7 µs | 12,237.9 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k4 | 1 sessions | 3 runs | 682 stmt/s | 1,282.5 µs | 5,758.3 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k4 | 2 sessions | 3 runs | 752 stmt/s | 2,326.8 µs | 9,768.8 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k4 | 4 sessions | 3 runs | 1,104 stmt/s | 2,398.6 µs | 13,651.4 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k4 | 8 sessions | 3 runs | 2,125 stmt/s | 2,459.7 µs | 12,328.0 µs | 0 refusals | 0 of 3 |
| kds | insert1 | group | k4 | 16 sessions | 3 runs | 3,991 stmt/s | 2,466.2 µs | 16,014.1 µs | 0 refusals | 0 of 3 |
| kds | insert1 | relaxed | k1 | 1 sessions | 3 runs | 17,799 stmt/s | 48.6 µs | 71.0 µs | 0 refusals | 0 of 3 |
| kds | insert1 | relaxed | k1 | 2 sessions | 3 runs | 28,342 stmt/s | 54.5 µs | 86.9 µs | 0 refusals | 0 of 3 |
| kds | insert1 | relaxed | k1 | 4 sessions | 3 runs | 38,884 stmt/s | 75.2 µs | 113.6 µs | 0 refusals | 0 of 3 |
| kds | insert1 | relaxed | k1 | 8 sessions | 3 runs | 39,022 stmt/s | 154.9 µs | 254.0 µs | 0 refusals | 0 of 3 |
| kds | insert1 | relaxed | k1 | 16 sessions | 3 runs | 38,166 stmt/s | 254.2 µs | 569.7 µs | 0 refusals | 0 of 3 |
| kds | insert1 | relaxed | k2 | 1 sessions | 3 runs | 17,693 stmt/s | 49.3 µs | 71.9 µs | 0 refusals | 0 of 3 |
| kds | insert1 | relaxed | k2 | 2 sessions | 3 runs | 28,389 stmt/s | 54.0 µs | 85.6 µs | 0 refusals | 0 of 3 |
| kds | insert1 | relaxed | k2 | 4 sessions | 3 runs | 47,159 stmt/s | 58.4 µs | 96.1 µs | 0 refusals | 0 of 3 |
| kds | insert1 | relaxed | k2 | 8 sessions | 3 runs | 54,525 stmt/s | 110.6 µs | 205.4 µs | 0 refusals | 0 of 3 |
| kds | insert1 | relaxed | k2 | 16 sessions | 3 runs | 68,341 stmt/s | 151.8 µs | 308.7 µs | 0 refusals | 0 of 3 |
| kds | insert1 | relaxed | k4 | 1 sessions | 3 runs | 17,318 stmt/s | 50.0 µs | 72.5 µs | 0 refusals | 0 of 3 |
| kds | insert1 | relaxed | k4 | 2 sessions | 3 runs | 26,205 stmt/s | 60.2 µs | 96.3 µs | 2 refusals | 0 of 3 |
| kds | insert1 | relaxed | k4 | 4 sessions | 3 runs | 36,352 stmt/s | 73.4 µs | 190.3 µs | 3 refusals | 0 of 3 |
| kds | insert1 | relaxed | k4 | 8 sessions | 3 runs | 51,727 stmt/s | 99.9 µs | 306.5 µs | 1 refusals | 0 of 3 |
| kds | insert1 | relaxed | k4 | 16 sessions | 3 runs | 56,560 stmt/s | 135.0 µs | 1,162.3 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k1 | 1 sessions | 3 runs | 708 stmt/s | 1,228.7 µs | 5,359.4 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k1 | 2 sessions | 3 runs | 721 stmt/s | 2,435.7 µs | 9,711.0 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k1 | 4 sessions | 3 runs | 733 stmt/s | 3,920.9 µs | 15,676.8 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k1 | 8 sessions | 3 runs | 712 stmt/s | 8,978.2 µs | 27,429.9 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k1 | 16 sessions | 3 runs | 728 stmt/s | 19,040.8 µs | 48,401.1 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k2 | 1 sessions | 3 runs | 702 stmt/s | 1,256.2 µs | 5,341.7 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k2 | 2 sessions | 3 runs | 726 stmt/s | 2,455.1 µs | 8,891.8 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k2 | 4 sessions | 3 runs | 739 stmt/s | 4,818.0 µs | 16,918.7 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k2 | 8 sessions | 3 runs | 746 stmt/s | 9,553.8 µs | 31,863.4 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k2 | 16 sessions | 3 runs | 703 stmt/s | 18,950.6 µs | 53,426.0 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k4 | 1 sessions | 3 runs | 665 stmt/s | 1,303.1 µs | 5,466.6 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k4 | 2 sessions | 3 runs | 696 stmt/s | 2,492.4 µs | 10,494.5 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k4 | 4 sessions | 3 runs | 942 stmt/s | 3,816.6 µs | 14,553.1 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k4 | 8 sessions | 3 runs | 1,175 stmt/s | 5,977.3 µs | 19,979.5 µs | 0 refusals | 0 of 3 |
| kds | insert1 | strict | k4 | 16 sessions | 3 runs | 1,010 stmt/s | 13,919.2 µs | 46,365.3 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k1 | 1 sessions | 3 runs | 701 stmt/s | 1,256.7 µs | 5,474.5 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k1 | 2 sessions | 3 runs | 718 stmt/s | 2,433.1 µs | 9,097.3 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k1 | 4 sessions | 3 runs | 1,376 stmt/s | 2,546.2 µs | 10,010.0 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k1 | 8 sessions | 3 runs | 2,708 stmt/s | 2,613.4 µs | 10,021.0 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k1 | 16 sessions | 3 runs | 4,918 stmt/s | 2,821.0 µs | 10,568.9 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k2 | 1 sessions | 3 runs | 689 stmt/s | 1,259.2 µs | 5,500.7 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k2 | 2 sessions | 3 runs | 717 stmt/s | 2,413.4 µs | 10,142.9 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k2 | 4 sessions | 3 runs | 1,072 stmt/s | 2,448.6 µs | 10,227.9 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k2 | 8 sessions | 3 runs | 1,441 stmt/s | 2,425.6 µs | 12,266.3 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k2 | 16 sessions | 3 runs | 3,558 stmt/s | 2,416.5 µs | 14,123.3 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k4 | 1 sessions | 3 runs | 687 stmt/s | 1,259.7 µs | 5,782.9 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k4 | 2 sessions | 3 runs | 690 stmt/s | 2,450.7 µs | 10,484.6 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k4 | 4 sessions | 3 runs | 1,440 stmt/s | 2,393.3 µs | 9,733.6 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k4 | 8 sessions | 3 runs | 2,156 stmt/s | 2,423.6 µs | 10,867.0 µs | 0 refusals | 0 of 3 |
| kds | insertN | group | k4 | 16 sessions | 3 runs | 4,226 stmt/s | 2,428.1 µs | 13,752.4 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k1 | 1 sessions | 3 runs | 17,955 stmt/s | 48.8 µs | 70.0 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k1 | 2 sessions | 3 runs | 28,463 stmt/s | 54.5 µs | 86.0 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k1 | 4 sessions | 3 runs | 38,410 stmt/s | 75.2 µs | 117.1 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k1 | 8 sessions | 3 runs | 38,691 stmt/s | 155.1 µs | 251.7 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k1 | 16 sessions | 3 runs | 37,721 stmt/s | 256.5 µs | 588.6 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k2 | 1 sessions | 3 runs | 17,582 stmt/s | 49.4 µs | 71.6 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k2 | 2 sessions | 3 runs | 28,798 stmt/s | 54.2 µs | 83.2 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k2 | 4 sessions | 3 runs | 46,430 stmt/s | 55.8 µs | 115.7 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k2 | 8 sessions | 3 runs | 60,343 stmt/s | 80.2 µs | 168.4 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k2 | 16 sessions | 3 runs | 64,647 stmt/s | 153.0 µs | 324.3 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k4 | 1 sessions | 3 runs | 17,344 stmt/s | 49.8 µs | 72.7 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k4 | 2 sessions | 3 runs | 24,560 stmt/s | 60.9 µs | 101.9 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k4 | 4 sessions | 3 runs | 35,718 stmt/s | 80.9 µs | 217.2 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k4 | 8 sessions | 3 runs | 46,171 stmt/s | 95.1 µs | 416.6 µs | 0 refusals | 0 of 3 |
| kds | insertN | relaxed | k4 | 16 sessions | 3 runs | 53,013 stmt/s | 115.3 µs | 1,095.7 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k1 | 1 sessions | 3 runs | 688 stmt/s | 1,281.4 µs | 5,396.9 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k1 | 2 sessions | 3 runs | 729 stmt/s | 2,440.6 µs | 9,021.3 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k1 | 4 sessions | 3 runs | 744 stmt/s | 3,847.0 µs | 15,713.5 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k1 | 8 sessions | 3 runs | 740 stmt/s | 8,625.9 µs | 28,042.4 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k1 | 16 sessions | 3 runs | 726 stmt/s | 19,156.6 µs | 49,556.8 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k2 | 1 sessions | 3 runs | 706 stmt/s | 1,237.0 µs | 5,305.4 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k2 | 2 sessions | 3 runs | 715 stmt/s | 2,463.9 µs | 9,698.9 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k2 | 4 sessions | 3 runs | 767 stmt/s | 4,333.3 µs | 16,605.5 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k2 | 8 sessions | 3 runs | 756 stmt/s | 9,494.0 µs | 30,657.1 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k2 | 16 sessions | 3 runs | 752 stmt/s | 18,745.7 µs | 52,531.7 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k4 | 1 sessions | 3 runs | 692 stmt/s | 1,271.0 µs | 5,499.7 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k4 | 2 sessions | 3 runs | 747 stmt/s | 2,377.4 µs | 9,353.1 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k4 | 4 sessions | 3 runs | 1,008 stmt/s | 3,660.4 µs | 12,944.5 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k4 | 8 sessions | 3 runs | 1,126 stmt/s | 5,062.1 µs | 21,408.2 µs | 0 refusals | 0 of 3 |
| kds | insertN | strict | k4 | 16 sessions | 3 runs | 985 stmt/s | 14,270.8 µs | 48,357.9 µs | 0 refusals | 0 of 3 |
| kds | point | group | k1 | 1 sessions | 3 runs | 14,527 stmt/s | 65.0 µs | 88.4 µs | 0 refusals | 0 of 3 |
| kds | point | group | k1 | 2 sessions | 3 runs | 23,644 stmt/s | 80.1 µs | 108.9 µs | 0 refusals | 0 of 3 |
| kds | point | group | k1 | 4 sessions | 3 runs | 43,097 stmt/s | 89.7 µs | 135.3 µs | 0 refusals | 0 of 3 |
| kds | point | group | k1 | 8 sessions | 3 runs | 44,498 stmt/s | 169.7 µs | 261.0 µs | 0 refusals | 0 of 3 |
| kds | point | group | k1 | 16 sessions | 3 runs | 43,412 stmt/s | 406.6 µs | 589.1 µs | 0 refusals | 0 of 3 |
| kds | point | group | k2 | 1 sessions | 3 runs | 14,144 stmt/s | 66.3 µs | 84.3 µs | 0 refusals | 0 of 3 |
| kds | point | group | k2 | 2 sessions | 3 runs | 23,382 stmt/s | 80.6 µs | 105.3 µs | 0 refusals | 0 of 3 |
| kds | point | group | k2 | 4 sessions | 3 runs | 44,469 stmt/s | 83.5 µs | 122.0 µs | 0 refusals | 0 of 3 |
| kds | point | group | k2 | 8 sessions | 3 runs | 59,459 stmt/s | 112.3 µs | 222.2 µs | 0 refusals | 3 of 3 |
| kds | point | group | k2 | 16 sessions | 3 runs | 65,331 stmt/s | 234.5 µs | 427.8 µs | 0 refusals | 3 of 3 |
| kds | point | group | k4 | 1 sessions | 3 runs | 14,159 stmt/s | 66.4 µs | 91.7 µs | 0 refusals | 0 of 3 |
| kds | point | group | k4 | 2 sessions | 3 runs | 21,071 stmt/s | 91.0 µs | 116.1 µs | 0 refusals | 0 of 3 |
| kds | point | group | k4 | 4 sessions | 3 runs | 29,087 stmt/s | 122.3 µs | 276.3 µs | 0 refusals | 0 of 3 |
| kds | point | group | k4 | 8 sessions | 3 runs | 41,032 stmt/s | 154.1 µs | 498.3 µs | 0 refusals | 2 of 3 |
| kds | point | group | k4 | 16 sessions | 3 runs | 56,397 stmt/s | 215.2 µs | 1,213.1 µs | 0 refusals | 3 of 3 |
| kds | point | relaxed | k1 | 1 sessions | 3 runs | 14,521 stmt/s | 65.1 µs | 87.9 µs | 0 refusals | 0 of 3 |
| kds | point | relaxed | k1 | 2 sessions | 3 runs | 23,776 stmt/s | 79.6 µs | 109.1 µs | 0 refusals | 0 of 3 |
| kds | point | relaxed | k1 | 4 sessions | 3 runs | 44,842 stmt/s | 87.1 µs | 126.7 µs | 0 refusals | 0 of 3 |
| kds | point | relaxed | k1 | 8 sessions | 3 runs | 44,671 stmt/s | 168.9 µs | 254.7 µs | 0 refusals | 0 of 3 |
| kds | point | relaxed | k1 | 16 sessions | 3 runs | 43,409 stmt/s | 411.0 µs | 594.6 µs | 0 refusals | 0 of 3 |
| kds | point | relaxed | k2 | 1 sessions | 3 runs | 14,262 stmt/s | 66.0 µs | 88.3 µs | 0 refusals | 0 of 3 |
| kds | point | relaxed | k2 | 2 sessions | 3 runs | 22,844 stmt/s | 82.3 µs | 110.2 µs | 0 refusals | 0 of 3 |
| kds | point | relaxed | k2 | 4 sessions | 3 runs | 43,536 stmt/s | 85.1 µs | 125.9 µs | 0 refusals | 0 of 3 |
| kds | point | relaxed | k2 | 8 sessions | 3 runs | 60,016 stmt/s | 111.5 µs | 222.5 µs | 0 refusals | 3 of 3 |
| kds | point | relaxed | k2 | 16 sessions | 3 runs | 65,347 stmt/s | 233.0 µs | 425.5 µs | 0 refusals | 3 of 3 |
| kds | point | relaxed | k4 | 1 sessions | 3 runs | 14,016 stmt/s | 66.5 µs | 90.6 µs | 0 refusals | 0 of 3 |
| kds | point | relaxed | k4 | 2 sessions | 3 runs | 16,658 stmt/s | 112.8 µs | 138.5 µs | 0 refusals | 0 of 3 |
| kds | point | relaxed | k4 | 4 sessions | 3 runs | 37,015 stmt/s | 108.2 µs | 141.9 µs | 0 refusals | 0 of 3 |
| kds | point | relaxed | k4 | 8 sessions | 3 runs | 41,703 stmt/s | 166.9 µs | 469.5 µs | 0 refusals | 3 of 3 |
| kds | point | relaxed | k4 | 16 sessions | 3 runs | 55,404 stmt/s | 196.5 µs | 899.0 µs | 0 refusals | 3 of 3 |
| kds | point | strict | k1 | 1 sessions | 3 runs | 14,390 stmt/s | 65.2 µs | 88.2 µs | 0 refusals | 0 of 3 |
| kds | point | strict | k1 | 2 sessions | 3 runs | 23,401 stmt/s | 80.7 µs | 109.7 µs | 0 refusals | 0 of 3 |
| kds | point | strict | k1 | 4 sessions | 3 runs | 44,344 stmt/s | 87.9 µs | 128.4 µs | 0 refusals | 0 of 3 |
| kds | point | strict | k1 | 8 sessions | 3 runs | 44,372 stmt/s | 170.0 µs | 256.9 µs | 0 refusals | 0 of 3 |
| kds | point | strict | k1 | 16 sessions | 3 runs | 43,725 stmt/s | 405.8 µs | 587.6 µs | 0 refusals | 0 of 3 |
| kds | point | strict | k2 | 1 sessions | 3 runs | 14,381 stmt/s | 65.7 µs | 88.5 µs | 0 refusals | 0 of 3 |
| kds | point | strict | k2 | 2 sessions | 3 runs | 23,260 stmt/s | 81.4 µs | 110.0 µs | 0 refusals | 0 of 3 |
| kds | point | strict | k2 | 4 sessions | 3 runs | 44,717 stmt/s | 83.8 µs | 124.9 µs | 0 refusals | 0 of 3 |
| kds | point | strict | k2 | 8 sessions | 3 runs | 60,851 stmt/s | 112.3 µs | 220.0 µs | 0 refusals | 3 of 3 |
| kds | point | strict | k2 | 16 sessions | 3 runs | 65,444 stmt/s | 233.8 µs | 428.2 µs | 0 refusals | 3 of 3 |
| kds | point | strict | k4 | 1 sessions | 3 runs | 14,269 stmt/s | 66.2 µs | 90.0 µs | 0 refusals | 0 of 3 |
| kds | point | strict | k4 | 2 sessions | 3 runs | 21,666 stmt/s | 86.5 µs | 114.2 µs | 0 refusals | 0 of 3 |
| kds | point | strict | k4 | 4 sessions | 3 runs | 35,833 stmt/s | 107.6 µs | 218.5 µs | 0 refusals | 0 of 3 |
| kds | point | strict | k4 | 8 sessions | 3 runs | 46,348 stmt/s | 146.0 µs | 383.8 µs | 0 refusals | 3 of 3 |
| kds | point | strict | k4 | 16 sessions | 3 runs | 51,301 stmt/s | 210.7 µs | 1,476.9 µs | 0 refusals | 3 of 3 |
| kds | recent | group | k1 | 1 sessions | 3 runs | 1,274 stmt/s | 1,026.1 µs | 4,331.7 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k1 | 2 sessions | 3 runs | 1,356 stmt/s | 1,295.7 µs | 6,373.6 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k1 | 4 sessions | 3 runs | 1,777 stmt/s | 2,276.3 µs | 8,013.6 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k1 | 8 sessions | 3 runs | 3,361 stmt/s | 2,386.4 µs | 8,855.3 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k1 | 16 sessions | 3 runs | 6,033 stmt/s | 2,611.3 µs | 10,279.2 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k2 | 1 sessions | 3 runs | 1,253 stmt/s | 409.0 µs | 4,161.4 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k2 | 2 sessions | 3 runs | 1,350 stmt/s | 356.4 µs | 7,915.4 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k2 | 4 sessions | 3 runs | 2,094 stmt/s | 1,027.5 µs | 8,038.3 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k2 | 8 sessions | 3 runs | 2,788 stmt/s | 1,898.9 µs | 10,571.0 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k2 | 16 sessions | 3 runs | 6,325 stmt/s | 1,866.3 µs | 11,342.0 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k4 | 1 sessions | 3 runs | 1,250 stmt/s | 383.9 µs | 4,379.5 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k4 | 2 sessions | 3 runs | 1,437 stmt/s | 359.5 µs | 7,130.4 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k4 | 4 sessions | 3 runs | 2,789 stmt/s | 1,375.0 µs | 7,314.5 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k4 | 8 sessions | 3 runs | 4,846 stmt/s | 1,459.0 µs | 8,419.9 µs | 0 refusals | 0 of 3 |
| kds | recent | group | k4 | 16 sessions | 3 runs | 7,376 stmt/s | 1,631.8 µs | 11,944.9 µs | 0 refusals | 0 of 3 |
| kds | recent | relaxed | k1 | 1 sessions | 3 runs | 14,344 stmt/s | 66.8 µs | 90.1 µs | 0 refusals | 0 of 3 |
| kds | recent | relaxed | k1 | 2 sessions | 3 runs | 21,792 stmt/s | 81.4 µs | 116.1 µs | 0 refusals | 0 of 3 |
| kds | recent | relaxed | k1 | 4 sessions | 3 runs | 33,341 stmt/s | 89.9 µs | 135.1 µs | 0 refusals | 0 of 3 |
| kds | recent | relaxed | k1 | 8 sessions | 3 runs | 33,847 stmt/s | 180.7 µs | 291.3 µs | 0 refusals | 0 of 3 |
| kds | recent | relaxed | k1 | 16 sessions | 3 runs | 34,206 stmt/s | 342.4 µs | 616.3 µs | 0 refusals | 0 of 3 |
| kds | recent | relaxed | k2 | 1 sessions | 3 runs | 14,044 stmt/s | 67.0 µs | 88.3 µs | 0 refusals | 0 of 3 |
| kds | recent | relaxed | k2 | 2 sessions | 3 runs | 21,064 stmt/s | 83.7 µs | 117.4 µs | 0 refusals | 0 of 3 |
| kds | recent | relaxed | k2 | 4 sessions | 3 runs | 37,931 stmt/s | 82.6 µs | 125.9 µs | 3 refusals | 0 of 3 |
| kds | recent | relaxed | k2 | 8 sessions | 3 runs | 47,495 stmt/s | 108.5 µs | 224.8 µs | 1 refusals | 0 of 3 |
| kds | recent | relaxed | k2 | 16 sessions | 3 runs | 53,233 stmt/s | 190.7 µs | 398.2 µs | 1 refusals | 0 of 3 |
| kds | recent | relaxed | k4 | 1 sessions | 3 runs | 13,785 stmt/s | 68.6 µs | 92.8 µs | 0 refusals | 0 of 3 |
| kds | recent | relaxed | k4 | 2 sessions | 3 runs | 17,267 stmt/s | 109.9 µs | 177.5 µs | 0 refusals | 0 of 3 |
| kds | recent | relaxed | k4 | 4 sessions | 3 runs | 26,473 stmt/s | 107.5 µs | 298.9 µs | 0 refusals | 0 of 3 |
| kds | recent | relaxed | k4 | 8 sessions | 3 runs | 38,962 stmt/s | 148.4 µs | 408.2 µs | 0 refusals | 0 of 3 |
| kds | recent | relaxed | k4 | 16 sessions | 3 runs | 46,590 stmt/s | 182.0 µs | 1,412.0 µs | 0 refusals | 2 of 3 |
| kds | recent | strict | k1 | 1 sessions | 3 runs | 1,248 stmt/s | 1,034.7 µs | 4,604.4 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k1 | 2 sessions | 3 runs | 1,337 stmt/s | 1,294.4 µs | 6,657.2 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k1 | 4 sessions | 3 runs | 1,371 stmt/s | 2,567.8 µs | 10,646.9 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k1 | 8 sessions | 3 runs | 1,372 stmt/s | 5,225.5 µs | 18,407.2 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k1 | 16 sessions | 3 runs | 1,336 stmt/s | 10,512.5 µs | 31,378.0 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k2 | 1 sessions | 3 runs | 1,248 stmt/s | 1,051.9 µs | 4,618.6 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k2 | 2 sessions | 3 runs | 1,330 stmt/s | 1,284.9 µs | 6,347.8 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k2 | 4 sessions | 3 runs | 1,376 stmt/s | 2,453.4 µs | 11,155.4 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k2 | 8 sessions | 3 runs | 1,326 stmt/s | 4,443.0 µs | 26,035.4 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k2 | 16 sessions | 3 runs | 1,472 stmt/s | 9,933.1 µs | 30,699.6 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k4 | 1 sessions | 3 runs | 1,252 stmt/s | 1,064.7 µs | 4,392.1 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k4 | 2 sessions | 3 runs | 1,349 stmt/s | 1,283.6 µs | 6,597.9 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k4 | 4 sessions | 3 runs | 1,814 stmt/s | 2,196.5 µs | 10,017.1 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k4 | 8 sessions | 3 runs | 1,867 stmt/s | 3,427.4 µs | 16,606.1 µs | 0 refusals | 0 of 3 |
| kds | recent | strict | k4 | 16 sessions | 3 runs | 1,867 stmt/s | 7,231.6 µs | 23,906.8 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k1 | 1 sessions | 3 runs | 676 stmt/s | 1,293.3 µs | 5,617.4 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k1 | 2 sessions | 3 runs | 712 stmt/s | 2,494.0 µs | 9,695.4 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k1 | 4 sessions | 3 runs | 1,368 stmt/s | 2,588.2 µs | 9,713.7 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k1 | 8 sessions | 3 runs | 2,622 stmt/s | 2,671.7 µs | 10,465.6 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k1 | 16 sessions | 3 runs | 4,977 stmt/s | 2,847.2 µs | 9,906.9 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k2 | 1 sessions | 3 runs | 657 stmt/s | 1,326.0 µs | 6,010.5 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k2 | 2 sessions | 3 runs | 722 stmt/s | 2,414.7 µs | 10,024.4 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k2 | 4 sessions | 3 runs | 811 stmt/s | 2,515.7 µs | 11,100.8 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k2 | 8 sessions | 3 runs | 1,862 stmt/s | 2,577.3 µs | 13,414.3 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k2 | 16 sessions | 3 runs | 1,656 stmt/s | 2,570.6 µs | 15,314.0 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k4 | 1 sessions | 3 runs | 654 stmt/s | 1,310.5 µs | 5,817.5 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k4 | 2 sessions | 3 runs | 720 stmt/s | 2,417.2 µs | 9,904.8 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k4 | 4 sessions | 3 runs | 1,434 stmt/s | 2,428.9 µs | 10,199.9 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k4 | 8 sessions | 3 runs | 2,125 stmt/s | 2,453.1 µs | 11,009.1 µs | 0 refusals | 0 of 3 |
| kds | trade | group | k4 | 16 sessions | 3 runs | 3,905 stmt/s | 2,427.8 µs | 13,705.9 µs | 0 refusals | 0 of 3 |
| kds | trade | relaxed | k1 | 1 sessions | 3 runs | 16,436 stmt/s | 53.3 µs | 75.4 µs | 0 refusals | 0 of 3 |
| kds | trade | relaxed | k1 | 2 sessions | 3 runs | 27,429 stmt/s | 59.2 µs | 98.4 µs | 0 refusals | 0 of 3 |
| kds | trade | relaxed | k1 | 4 sessions | 3 runs | 37,600 stmt/s | 84.1 µs | 127.8 µs | 0 refusals | 0 of 3 |
| kds | trade | relaxed | k1 | 8 sessions | 3 runs | 37,323 stmt/s | 182.1 µs | 286.8 µs | 0 refusals | 0 of 3 |
| kds | trade | relaxed | k1 | 16 sessions | 3 runs | 37,118 stmt/s | 280.2 µs | 637.7 µs | 0 refusals | 0 of 3 |
| kds | trade | relaxed | k2 | 1 sessions | 3 runs | 16,110 stmt/s | 54.1 µs | 76.3 µs | 0 refusals | 0 of 3 |
| kds | trade | relaxed | k2 | 2 sessions | 3 runs | 26,642 stmt/s | 62.3 µs | 92.1 µs | 0 refusals | 0 of 3 |
| kds | trade | relaxed | k2 | 4 sessions | 3 runs | 41,920 stmt/s | 64.8 µs | 124.1 µs | 2 refusals | 0 of 3 |
| kds | trade | relaxed | k2 | 8 sessions | 3 runs | 53,805 stmt/s | 100.4 µs | 195.8 µs | 1 refusals | 0 of 3 |
| kds | trade | relaxed | k2 | 16 sessions | 3 runs | 60,113 stmt/s | 168.2 µs | 362.4 µs | 0 refusals | 0 of 3 |
| kds | trade | relaxed | k4 | 1 sessions | 3 runs | 15,905 stmt/s | 54.4 µs | 78.0 µs | 0 refusals | 0 of 3 |
| kds | trade | relaxed | k4 | 2 sessions | 3 runs | 26,016 stmt/s | 64.7 µs | 96.6 µs | 1 refusals | 0 of 3 |
| kds | trade | relaxed | k4 | 4 sessions | 3 runs | 28,568 stmt/s | 130.3 µs | 296.4 µs | 1 refusals | 0 of 3 |
| kds | trade | relaxed | k4 | 8 sessions | 3 runs | 41,876 stmt/s | 109.4 µs | 461.3 µs | 2 refusals | 0 of 3 |
| kds | trade | relaxed | k4 | 16 sessions | 3 runs | 51,024 stmt/s | 133.7 µs | 1,192.1 µs | 2 refusals | 0 of 3 |
| kds | trade | strict | k1 | 1 sessions | 3 runs | 680 stmt/s | 1,294.3 µs | 5,657.7 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k1 | 2 sessions | 3 runs | 695 stmt/s | 2,535.8 µs | 9,090.8 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k1 | 4 sessions | 3 runs | 700 stmt/s | 4,085.1 µs | 17,202.1 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k1 | 8 sessions | 3 runs | 690 stmt/s | 9,275.0 µs | 28,613.4 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k1 | 16 sessions | 3 runs | 684 stmt/s | 20,169.9 µs | 51,471.9 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k2 | 1 sessions | 3 runs | 653 stmt/s | 1,319.8 µs | 5,890.9 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k2 | 2 sessions | 3 runs | 682 stmt/s | 2,592.1 µs | 9,619.8 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k2 | 4 sessions | 3 runs | 717 stmt/s | 4,960.7 µs | 15,781.2 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k2 | 8 sessions | 3 runs | 729 stmt/s | 9,676.9 µs | 29,807.4 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k2 | 16 sessions | 3 runs | 721 stmt/s | 19,473.7 µs | 51,848.5 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k4 | 1 sessions | 3 runs | 655 stmt/s | 1,318.1 µs | 5,552.8 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k4 | 2 sessions | 3 runs | 717 stmt/s | 2,456.6 µs | 9,492.7 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k4 | 4 sessions | 3 runs | 1,000 stmt/s | 3,708.6 µs | 12,766.8 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k4 | 8 sessions | 3 runs | 1,133 stmt/s | 5,723.9 µs | 22,336.9 µs | 0 refusals | 0 of 3 |
| kds | trade | strict | k4 | 16 sessions | 3 runs | 857 stmt/s | 16,544.9 µs | 51,460.0 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0 | 1 sessions | 3 runs | 17,332 stmt/s | 53.7 µs | 82.0 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0 | 2 sessions | 3 runs | 31,384 stmt/s | 58.2 µs | 111.0 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0 | 4 sessions | 3 runs | 29,454 stmt/s | 126.9 µs | 245.6 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0 | 8 sessions | 3 runs | 27,775 stmt/s | 271.5 µs | 578.6 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0 | 16 sessions | 3 runs | 27,186 stmt/s | 559.3 µs | 1,192.5 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0-2 | 1 sessions | 3 runs | 17,296 stmt/s | 53.9 µs | 80.9 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0-2 | 2 sessions | 3 runs | 29,595 stmt/s | 63.3 µs | 90.7 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0-2 | 4 sessions | 3 runs | 61,344 stmt/s | 57.3 µs | 127.6 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0-2 | 8 sessions | 3 runs | 58,062 stmt/s | 126.9 µs | 264.9 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0-2 | 16 sessions | 3 runs | 54,628 stmt/s | 273.4 µs | 651.6 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0-2-4-6 | 1 sessions | 3 runs | 17,368 stmt/s | 53.8 µs | 80.7 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0-2-4-6 | 2 sessions | 3 runs | 29,731 stmt/s | 62.8 µs | 90.3 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0-2-4-6 | 4 sessions | 3 runs | 47,489 stmt/s | 78.3 µs | 138.9 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0-2-4-6 | 8 sessions | 3 runs | 73,334 stmt/s | 78.4 µs | 274.5 µs | 0 refusals | 0 of 3 |
| pg | insert1 | off | cpus 0-2-4-6 | 16 sessions | 3 runs | 77,684 stmt/s | 144.2 µs | 606.4 µs | 0 refusals | 3 of 3 |
| pg | insert1 | on | cpus 0 | 1 sessions | 3 runs | 708 stmt/s | 1,240.8 µs | 5,050.9 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0 | 2 sessions | 3 runs | 734 stmt/s | 2,399.5 µs | 9,434.6 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0 | 4 sessions | 3 runs | 1,385 stmt/s | 2,583.3 µs | 9,467.5 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0 | 8 sessions | 3 runs | 2,698 stmt/s | 2,644.0 µs | 9,961.0 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0 | 16 sessions | 3 runs | 5,120 stmt/s | 2,815.5 µs | 10,024.0 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0-2 | 1 sessions | 3 runs | 691 stmt/s | 1,264.2 µs | 5,506.6 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0-2 | 2 sessions | 3 runs | 749 stmt/s | 2,359.4 µs | 9,033.7 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0-2 | 4 sessions | 3 runs | 1,448 stmt/s | 2,441.7 µs | 8,858.4 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0-2 | 8 sessions | 3 runs | 2,771 stmt/s | 2,582.2 µs | 9,231.4 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0-2 | 16 sessions | 3 runs | 5,325 stmt/s | 2,711.2 µs | 9,050.7 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0-2-4-6 | 1 sessions | 3 runs | 674 stmt/s | 1,299.1 µs | 5,440.6 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0-2-4-6 | 2 sessions | 3 runs | 691 stmt/s | 2,495.7 µs | 9,283.6 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0-2-4-6 | 4 sessions | 3 runs | 1,426 stmt/s | 2,451.4 µs | 10,185.6 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0-2-4-6 | 8 sessions | 3 runs | 2,715 stmt/s | 2,603.6 µs | 9,080.0 µs | 0 refusals | 0 of 3 |
| pg | insert1 | on | cpus 0-2-4-6 | 16 sessions | 3 runs | 5,220 stmt/s | 2,717.9 µs | 9,528.9 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0 | 1 sessions | 3 runs | 17,299 stmt/s | 53.9 µs | 81.8 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0 | 2 sessions | 3 runs | 31,136 stmt/s | 58.9 µs | 111.3 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0 | 4 sessions | 3 runs | 29,122 stmt/s | 128.5 µs | 245.9 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0 | 8 sessions | 3 runs | 27,595 stmt/s | 273.1 µs | 590.4 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0 | 16 sessions | 3 runs | 26,933 stmt/s | 566.2 µs | 1,186.8 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0-2 | 1 sessions | 3 runs | 17,431 stmt/s | 53.8 µs | 80.0 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0-2 | 2 sessions | 3 runs | 29,954 stmt/s | 62.8 µs | 89.0 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0-2 | 4 sessions | 3 runs | 61,180 stmt/s | 58.0 µs | 116.1 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0-2 | 8 sessions | 3 runs | 57,862 stmt/s | 127.7 µs | 256.6 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0-2 | 16 sessions | 3 runs | 54,282 stmt/s | 276.7 µs | 616.1 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0-2-4-6 | 1 sessions | 3 runs | 17,267 stmt/s | 53.9 µs | 83.2 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0-2-4-6 | 2 sessions | 3 runs | 29,890 stmt/s | 62.5 µs | 89.6 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0-2-4-6 | 4 sessions | 3 runs | 47,639 stmt/s | 78.4 µs | 123.6 µs | 0 refusals | 0 of 3 |
| pg | insertN | off | cpus 0-2-4-6 | 8 sessions | 3 runs | 82,793 stmt/s | 62.9 µs | 235.3 µs | 0 refusals | 3 of 3 |
| pg | insertN | off | cpus 0-2-4-6 | 16 sessions | 3 runs | 79,473 stmt/s | 144.6 µs | 567.5 µs | 0 refusals | 3 of 3 |
| pg | insertN | on | cpus 0 | 1 sessions | 3 runs | 647 stmt/s | 1,305.6 µs | 5,766.1 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0 | 2 sessions | 3 runs | 702 stmt/s | 2,488.0 µs | 9,842.6 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0 | 4 sessions | 3 runs | 1,364 stmt/s | 2,593.1 µs | 9,443.2 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0 | 8 sessions | 3 runs | 2,622 stmt/s | 2,728.6 µs | 9,420.8 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0 | 16 sessions | 3 runs | 4,857 stmt/s | 2,938.4 µs | 10,635.4 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0-2 | 1 sessions | 3 runs | 686 stmt/s | 1,268.7 µs | 5,326.5 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0-2 | 2 sessions | 3 runs | 733 stmt/s | 2,448.3 µs | 8,866.9 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0-2 | 4 sessions | 3 runs | 1,403 stmt/s | 2,499.8 µs | 9,822.7 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0-2 | 8 sessions | 3 runs | 2,666 stmt/s | 2,638.7 µs | 9,426.9 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0-2 | 16 sessions | 3 runs | 5,035 stmt/s | 2,851.8 µs | 10,584.8 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0-2-4-6 | 1 sessions | 3 runs | 672 stmt/s | 1,289.5 µs | 5,249.8 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0-2-4-6 | 2 sessions | 3 runs | 729 stmt/s | 2,431.0 µs | 8,703.6 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0-2-4-6 | 4 sessions | 3 runs | 1,423 stmt/s | 2,461.6 µs | 9,197.3 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0-2-4-6 | 8 sessions | 3 runs | 2,764 stmt/s | 2,547.3 µs | 9,372.2 µs | 0 refusals | 0 of 3 |
| pg | insertN | on | cpus 0-2-4-6 | 16 sessions | 3 runs | 5,173 stmt/s | 2,742.8 µs | 9,796.1 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0 | 1 sessions | 3 runs | 16,974 stmt/s | 56.5 µs | 80.9 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0 | 2 sessions | 3 runs | 30,116 stmt/s | 62.2 µs | 104.4 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0 | 4 sessions | 3 runs | 28,987 stmt/s | 132.3 µs | 235.7 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0 | 8 sessions | 3 runs | 27,438 stmt/s | 280.2 µs | 544.3 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0 | 16 sessions | 3 runs | 26,960 stmt/s | 572.5 µs | 1,100.0 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0-2 | 1 sessions | 3 runs | 16,929 stmt/s | 56.7 µs | 79.5 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0-2 | 2 sessions | 3 runs | 29,184 stmt/s | 67.0 µs | 90.3 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0-2 | 4 sessions | 3 runs | 60,665 stmt/s | 61.1 µs | 101.9 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0-2 | 8 sessions | 3 runs | 57,128 stmt/s | 133.1 µs | 242.3 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0-2 | 16 sessions | 3 runs | 54,433 stmt/s | 280.9 µs | 583.2 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0-2-4-6 | 1 sessions | 3 runs | 16,998 stmt/s | 56.3 µs | 76.0 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0-2-4-6 | 2 sessions | 3 runs | 29,382 stmt/s | 66.7 µs | 89.2 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0-2-4-6 | 4 sessions | 3 runs | 46,968 stmt/s | 81.2 µs | 130.5 µs | 0 refusals | 0 of 3 |
| pg | point | off | cpus 0-2-4-6 | 8 sessions | 3 runs | 84,410 stmt/s | 65.4 µs | 228.1 µs | 0 refusals | 3 of 3 |
| pg | point | off | cpus 0-2-4-6 | 16 sessions | 3 runs | 80,327 stmt/s | 147.4 µs | 558.7 µs | 0 refusals | 3 of 3 |
| pg | point | on | cpus 0 | 1 sessions | 3 runs | 16,965 stmt/s | 56.6 µs | 80.0 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0 | 2 sessions | 3 runs | 30,621 stmt/s | 62.2 µs | 96.5 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0 | 4 sessions | 3 runs | 28,884 stmt/s | 133.1 µs | 235.8 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0 | 8 sessions | 3 runs | 27,376 stmt/s | 281.5 µs | 542.8 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0 | 16 sessions | 3 runs | 27,051 stmt/s | 572.0 µs | 1,083.4 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0-2 | 1 sessions | 3 runs | 16,963 stmt/s | 56.7 µs | 75.0 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0-2 | 2 sessions | 3 runs | 29,259 stmt/s | 67.1 µs | 88.2 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0-2 | 4 sessions | 3 runs | 60,785 stmt/s | 60.9 µs | 102.5 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0-2 | 8 sessions | 3 runs | 56,891 stmt/s | 133.1 µs | 242.5 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0-2 | 16 sessions | 3 runs | 54,758 stmt/s | 279.3 µs | 557.3 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0-2-4-6 | 1 sessions | 3 runs | 17,056 stmt/s | 56.5 µs | 73.5 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0-2-4-6 | 2 sessions | 3 runs | 29,542 stmt/s | 65.9 µs | 88.7 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0-2-4-6 | 4 sessions | 3 runs | 46,970 stmt/s | 81.2 µs | 135.9 µs | 0 refusals | 0 of 3 |
| pg | point | on | cpus 0-2-4-6 | 8 sessions | 3 runs | 84,713 stmt/s | 65.4 µs | 227.5 µs | 0 refusals | 3 of 3 |
| pg | point | on | cpus 0-2-4-6 | 16 sessions | 3 runs | 80,366 stmt/s | 147.6 µs | 559.5 µs | 0 refusals | 3 of 3 |
| pg | recent | off | cpus 0 | 1 sessions | 3 runs | 14,173 stmt/s | 67.4 µs | 94.5 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0 | 2 sessions | 3 runs | 23,963 stmt/s | 77.9 µs | 148.9 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0 | 4 sessions | 3 runs | 23,176 stmt/s | 166.3 µs | 307.0 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0 | 8 sessions | 3 runs | 22,280 stmt/s | 343.9 µs | 762.6 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0 | 16 sessions | 3 runs | 21,652 stmt/s | 717.3 µs | 1,531.7 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0-2 | 1 sessions | 3 runs | 14,222 stmt/s | 67.5 µs | 92.3 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0-2 | 2 sessions | 3 runs | 25,031 stmt/s | 75.5 µs | 103.3 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0-2 | 4 sessions | 3 runs | 46,609 stmt/s | 78.6 µs | 151.4 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0-2 | 8 sessions | 3 runs | 45,710 stmt/s | 167.6 µs | 315.4 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0-2 | 16 sessions | 3 runs | 43,880 stmt/s | 348.7 µs | 800.9 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0-2-4-6 | 1 sessions | 3 runs | 14,166 stmt/s | 67.6 µs | 93.2 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0-2-4-6 | 2 sessions | 3 runs | 24,978 stmt/s | 75.7 µs | 103.0 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0-2-4-6 | 4 sessions | 3 runs | 40,510 stmt/s | 91.1 µs | 148.6 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0-2-4-6 | 8 sessions | 3 runs | 59,427 stmt/s | 98.3 µs | 304.5 µs | 0 refusals | 0 of 3 |
| pg | recent | off | cpus 0-2-4-6 | 16 sessions | 3 runs | 63,365 stmt/s | 190.2 µs | 683.2 µs | 0 refusals | 2 of 3 |
| pg | recent | on | cpus 0 | 1 sessions | 3 runs | 1,232 stmt/s | 125.6 µs | 4,618.2 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0 | 2 sessions | 3 runs | 1,417 stmt/s | 299.1 µs | 6,692.1 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0 | 4 sessions | 3 runs | 2,466 stmt/s | 1,009.8 µs | 8,548.1 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0 | 8 sessions | 3 runs | 5,143 stmt/s | 1,669.7 µs | 6,825.0 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0 | 16 sessions | 3 runs | 9,733 stmt/s | 1,503.3 µs | 6,843.7 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0-2 | 1 sessions | 3 runs | 1,238 stmt/s | 455.7 µs | 4,386.5 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0-2 | 2 sessions | 3 runs | 1,420 stmt/s | 289.7 µs | 6,731.7 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0-2 | 4 sessions | 3 runs | 2,702 stmt/s | 389.2 µs | 7,245.6 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0-2 | 8 sessions | 3 runs | 5,058 stmt/s | 1,876.5 µs | 7,231.8 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0-2 | 16 sessions | 3 runs | 9,942 stmt/s | 1,539.9 µs | 7,318.6 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0-2-4-6 | 1 sessions | 3 runs | 1,257 stmt/s | 449.2 µs | 4,481.4 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0-2-4-6 | 2 sessions | 3 runs | 1,431 stmt/s | 166.9 µs | 6,809.6 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0-2-4-6 | 4 sessions | 3 runs | 2,773 stmt/s | 1,002.7 µs | 6,657.2 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0-2-4-6 | 8 sessions | 3 runs | 5,179 stmt/s | 1,750.3 µs | 7,287.1 µs | 0 refusals | 0 of 3 |
| pg | recent | on | cpus 0-2-4-6 | 16 sessions | 3 runs | 10,171 stmt/s | 1,618.5 µs | 7,196.4 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0 | 1 sessions | 3 runs | 13,812 stmt/s | 70.6 µs | 97.4 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0 | 2 sessions | 3 runs | 21,473 stmt/s | 77.6 µs | 168.0 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0 | 4 sessions | 3 runs | 19,897 stmt/s | 195.2 µs | 369.6 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0 | 8 sessions | 3 runs | 18,740 stmt/s | 416.4 µs | 890.1 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0 | 16 sessions | 3 runs | 18,540 stmt/s | 845.7 µs | 1,829.3 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0-2 | 1 sessions | 3 runs | 13,823 stmt/s | 70.6 µs | 96.5 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0-2 | 2 sessions | 3 runs | 24,204 stmt/s | 78.2 µs | 109.4 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0-2 | 4 sessions | 3 runs | 41,634 stmt/s | 85.5 µs | 174.6 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0-2 | 8 sessions | 3 runs | 39,222 stmt/s | 196.2 µs | 383.2 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0-2 | 16 sessions | 3 runs | 37,593 stmt/s | 410.8 µs | 912.5 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0-2-4-6 | 1 sessions | 3 runs | 13,688 stmt/s | 70.6 µs | 98.7 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0-2-4-6 | 2 sessions | 3 runs | 24,018 stmt/s | 78.4 µs | 114.5 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0-2-4-6 | 4 sessions | 3 runs | 38,766 stmt/s | 95.7 µs | 152.2 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0-2-4-6 | 8 sessions | 3 runs | 53,026 stmt/s | 117.3 µs | 339.8 µs | 0 refusals | 0 of 3 |
| pg | trade | off | cpus 0-2-4-6 | 16 sessions | 3 runs | 55,594 stmt/s | 223.3 µs | 775.9 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0 | 1 sessions | 3 runs | 673 stmt/s | 1,315.2 µs | 5,452.1 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0 | 2 sessions | 3 runs | 709 stmt/s | 2,505.2 µs | 9,191.1 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0 | 4 sessions | 3 runs | 1,313 stmt/s | 2,670.5 µs | 10,750.8 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0 | 8 sessions | 3 runs | 2,529 stmt/s | 2,773.6 µs | 10,190.9 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0 | 16 sessions | 3 runs | 4,902 stmt/s | 2,876.4 µs | 11,082.4 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0-2 | 1 sessions | 3 runs | 678 stmt/s | 1,289.0 µs | 5,501.3 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0-2 | 2 sessions | 3 runs | 743 stmt/s | 2,395.4 µs | 9,329.1 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0-2 | 4 sessions | 3 runs | 1,416 stmt/s | 2,489.3 µs | 9,464.0 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0-2 | 8 sessions | 3 runs | 2,661 stmt/s | 2,677.5 µs | 9,819.2 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0-2 | 16 sessions | 3 runs | 5,150 stmt/s | 2,752.8 µs | 9,738.7 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0-2-4-6 | 1 sessions | 3 runs | 568 stmt/s | 1,383.6 µs | 7,387.0 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0-2-4-6 | 2 sessions | 3 runs | 686 stmt/s | 2,495.4 µs | 9,778.4 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0-2-4-6 | 4 sessions | 3 runs | 1,423 stmt/s | 2,463.4 µs | 8,844.3 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0-2-4-6 | 8 sessions | 3 runs | 2,616 stmt/s | 2,610.9 µs | 11,661.6 µs | 0 refusals | 0 of 3 |
| pg | trade | on | cpus 0-2-4-6 | 16 sessions | 3 runs | 5,286 stmt/s | 2,719.6 µs | 9,529.5 µs | 0 refusals | 0 of 3 |

## Item shares, KDS (contended wait over cores x span)

| item | max share, all cells | cell at max | max share, clean cells (cores <= 2) | clean cell at max | median share over cells | material (>= 5 %) |
|---|---|---|---|---|---|---|
| P1 frame table | 0.91 % | recent relaxed k4 16 | 0.67 % | recent relaxed k2 16 | 0.001 % | no |
| P2 window | 1.80 % | insertN relaxed k4 16 | 0.26 % | insertN relaxed k2 16 | 0.000 % | no |
| P3 statistics | 0.00 % | point group k4 16 | 0.00 % | point group k2 16 | 0.000 % | no |
| P4 relation borrow | 0.09 % | insert1 relaxed k4 16 | 0.06 % | insert1 relaxed k2 4 | 0.000 % | no |
| P6 WAL append | 11.36 % | insertN relaxed k4 16 | 10.01 % | insertN relaxed k2 16 | 0.001 % | yes |
| P7 sync gate | 55.35 % | recent group k2 8 | 55.35 % | recent group k2 8 | 0.103 % | yes |
| P7 snapshot ceiling | 39.44 % | recent strict k4 16 | 39.33 % | recent strict k2 4 | 0.000 % | yes |
| P8 assertion dir | 0.00 % | trade relaxed k4 16 | 0.00 % | insertN relaxed k2 8 | 0.000 % | no |
| superblock | 0.00 % | insertN group k4 2 | 0.00 % | insert1 strict k2 16 | 0.000 % | no |

## Other counters, KDS (sum over every run)

- `contention_carves`: 13,786
- `contention_ceiling_wait_us`: 306,149,337
- `contention_ceiling_waits`: 151,270
- `contention_checkpoint_runs`: 2,356
- `contention_drain_passes_pending`: 2,197,224,334
- `contention_page_latch_spin_turns`: 98,466,594
- `contention_page_latch_waits`: 2,099,742
- `contention_refused_btree_descend`: 20
- `contention_refused_btree_lookup`: 0
- `contention_refused_btree_parent`: 0
- `contention_refused_btree_secure`: 0
- `contention_refused_index_descend`: 0
- `contention_refused_index_parent`: 0
- `contention_refused_index_secure`: 0
- `contention_syncs_inline`: 991,774
- `contention_syncs_writer`: 1,334,172
- `contention_carve_longest_us` (over the server's life): max 2,242.1 ms; runs over 10 ms: 340 of 675, 13 of them with no such event in the measured span
- `contention_checkpoint_longest_us` (over the server's life): max 3,093.7 ms; runs over 10 ms: 596 of 675, 0 of them with no such event in the measured span

### Refusals by shape and cores (sum over runs)

- insert1 k4: 6
- recent k2: 5
- trade k2: 3
- trade k4: 6

## BA-R4's premise: a session's latency by its core, `cores = 2`

| shape | durability | core 0 p50 | core 0 p99 | peer p50 | peer p99 | clients on core 0 / peers | statements per client, core 0 / peers (median) |
|---|---|---|---|---|---|---|---|
| insert1 | group | 12,905.8 µs | 1,395,463.6 µs | 2,387.8 µs | 9,507.4 µs | 47 / 46 clients | 44 / 2,900 statements |
| insert1 | strict | 17,891.3 µs | 36,927.3 µs | 17,749.8 µs | 35,902.9 µs | 41 / 52 clients | 412 / 411 statements |
| insertN | group | 11,450.9 µs | 1,300,254.6 µs | 2,409.4 µs | 9,896.8 µs | 39 / 54 clients | 28 / 2,861 statements |
| insertN | strict | 14,162.3 µs | 33,691.6 µs | 14,484.9 µs | 34,022.5 µs | 43 / 50 clients | 510 / 499 statements |
| trade | group | 42,666.6 µs | 1,638,382.4 µs | 2,532.7 µs | 9,900.9 µs | 49 / 44 clients | 16 / 2,764 statements |
| trade | strict | 15,067.3 µs | 33,121.7 µs | 17,793.8 µs | 35,675.1 µs | 43 / 50 clients | 480 / 412 statements |

## P11 proxy: runs with a session on every core against the rest, same cell

- insert1 group k4 8: 2 runs against 1, -1.1 %
- insert1 relaxed k2 8: 2 runs against 1, +52.8 %
- insert1 relaxed k4 4: 1 runs against 2, -5.3 %
- insert1 relaxed k4 8: 2 runs against 1, -19.7 %
- insertN group k2 2: 2 runs against 1, -4.9 %
- insertN group k2 4: 2 runs against 1, -33.8 %
- insertN relaxed k4 8: 1 runs against 2, +3.1 %
- insertN strict k2 2: 1 runs against 2, -6.6 %
- insertN strict k2 4: 2 runs against 1, +15.8 %
- insertN strict k4 8: 2 runs against 1, +6.0 %
- point group k4 4: 1 runs against 2, +43.9 %
- point group k4 8: 2 runs against 1, +9.4 %
- point relaxed k2 2: 1 runs against 2, -5.3 %
- point relaxed k2 4: 2 runs against 1, +4.0 %
- point relaxed k4 8: 2 runs against 1, +9.6 %
- point strict k2 2: 1 runs against 2, -9.2 %
- point strict k2 4: 2 runs against 1, +5.9 %
- recent group k2 2: 2 runs against 1, -6.0 %
- recent group k4 8: 2 runs against 1, +22.1 %
- recent relaxed k2 2: 1 runs against 2, +0.8 %
- recent relaxed k4 8: 1 runs against 2, -7.0 %
- recent relaxed k4 16: 2 runs against 1, +38.7 %
- recent strict k2 2: 2 runs against 1, +1.4 %
- recent strict k4 8: 1 runs against 2, +27.0 %
- trade group k2 2: 2 runs against 1, -1.0 %
- trade group k2 4: 2 runs against 1, -46.1 %
- trade group k4 8: 2 runs against 1, +49.1 %
- trade relaxed k2 2: 2 runs against 1, -1.0 %
- trade strict k2 2: 1 runs against 2, +8.2 %
- trade strict k4 4: 1 runs against 2, +29.0 %

30 cells; the every-core runs faster by over 10 % in 8, slower by over 10 % in 3

## Host

- runs: 1125; 1-min load before a run: median 2.57, max 6.00; KDS runs alone: median 2.05, max 5.99
- runs with a competing build or ctest before or after: 0
- runs where pgrep matched only a wait loop naming a build: 1125
- binaries: ['ed5927dc9b4e3702e2e4c758511b386ab896ba004559e3060fee2f8f3e1326b6']
- data devices: ['/dev/root ext4']
