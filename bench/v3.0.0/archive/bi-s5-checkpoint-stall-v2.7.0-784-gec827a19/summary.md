
### B-c1-group-r1.json: cores=1 durability=group 600.0 s, 49698816 rows inserted, instance anchor gap max 5.51 s
host before: load 2.57 3.33 3.82 1/404 49135; io pressure some avg10=3.94 avg60=22.50 avg300=31.68 total=48661059260

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 120 | 0 | 0 | 137 | 8724/9636 | 611.9/1016.2 | 404.8/387.3 | 1841/5567 | 42/602 | 0/0 | 5.51 | 425630.6 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1846 (24.9); log_gate_batch 0 (0.0); log_gate_run 0 (0.0); page_write 6446 (262.0); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 328 (9.9); publish_sync 0 (0.0); one step max 262.3, its page writes max 262.1 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 4160/27340 | 15.50/11.23 | 363.41/316.74 | 395.3/345.4 | 432.8/378.8 | 0 |
| 0 | ping | 4160/27340 | 12.24/6.26 | 352.15/309.96 | 392.4/340.8 | 413.1/377.9 | 0 |

### B-c1-group-r2.json: cores=1 durability=group 600.0 s, 44261376 rows inserted, instance anchor gap max 5.56 s
host before: load 3.09 4.23 4.11 1/408 59135; io pressure some avg10=6.07 avg60=27.34 avg300=37.19 total=49708666585

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 120 | 0 | 0 | 121 | 7608/10148 | 720.0/1236.5 | 565.0/535.4 | 2030/5189 | 55/542 | 0/0 | 5.56 | 443460.4 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1789 (24.5); log_gate_batch 0 (0.0); log_gate_run 0 (0.0); page_write 7000 (150.9); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 289 (3.0); publish_sync 0 (0.0); one step max 151.1, its page writes max 151.0 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 4829/26671 | 24.72/37.81 | 486.42/438.73 | 540.5/484.4 | 554.5/529.8 | 0 |
| 0 | ping | 4829/26671 | 19.84/8.47 | 470.06/431.11 | 529.5/479.8 | 546.4/529.1 | 0 |

### B-c1-group-r3.json: cores=1 durability=group 600.0 s, 44300288 rows inserted, instance anchor gap max 5.60 s
host before: load 2.86 3.76 3.53 1/403 69633; io pressure some avg10=3.14 avg60=22.52 avg300=33.46 total=50777619219

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 120 | 0 | 0 | 120 | 7615/10349 | 711.8/1262.7 | 598.3/528.7 | 2002/4832 | 58/537 | 0/0 | 5.60 | 443489.6 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1806 (24.6); log_gate_batch 0 (0.0); log_gate_run 0 (0.0); page_write 6555 (228.6); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 301 (7.8); publish_sync 0 (0.0); one step max 228.8, its page writes max 228.7 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 4937/26563 | 26.57/44.78 | 497.95/454.07 | 562.1/493.7 | 610.5/519.1 | 0 |
| 0 | ping | 4937/26563 | 21.49/9.02 | 479.60/447.30 | 544.0/487.9 | 589.5/518.4 | 0 |

### B-c1-relaxed-r1.json: cores=1 durability=relaxed 600.0 s, 57684992 rows inserted, instance anchor gap max 11.21 s
host before: load 6.35 6.04 5.72 2/410 44142; io pressure some avg10=0.03 avg60=16.25 avg300=24.44 total=48147930509

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 114 | 0 | 0 | 138 | 7593/18231 | 3506.7/6568.6 | 1125.9/589.2 | 1458/710 | 620/277 | 4/0 | 11.21 | 42122.1 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1926 (25.0); log_gate_batch 3068 (436.8); log_gate_run 0 (0.0); page_write 5960 (278.9); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 2933 (444.5); publish_sync 0 (0.0); one step max 438.5, its page writes max 279.0 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 19459/12041 | 147.31/110.05 | 725.75/478.06 | 990.3/523.9 | 1123.3/589.7 | 0 |
| 0 | ping | 19459/12041 | 123.17/72.58 | 713.23/473.53 | 984.3/513.8 | 1120.5/586.2 | 0 |

### B-c1-relaxed-r2.json: cores=1 durability=relaxed 600.0 s, 45218816 rows inserted, instance anchor gap max 15.15 s
host before: load 2.78 3.46 3.32 1/395 54125; io pressure some avg10=2.27 avg60=16.73 avg300=26.52 total=49136874065

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 72 | 0 | 0 | 164 | 9369/17048 | 6091.7/11710.4 | 1525.3/784.6 | 998/347 | 473/156 | 61/0 | 15.15 | 35258.5 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1047 (23.7); log_gate_batch 1659 (384.2); log_gate_run 0 (0.0); page_write 3180 (349.5); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 2894 (368.8); publish_sync 0 (0.0); one step max 384.6, its page writes max 349.6 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 22366/9134 | 326.14/210.12 | 1232.62/853.03 | 1425.5/1179.1 | 1513.2/1269.4 | 0 |
| 0 | ping | 22366/9134 | 269.36/149.28 | 1202.79/721.09 | 1421.4/749.6 | 1512.9/767.9 | 0 |

### B-c1-relaxed-r3.json: cores=1 durability=relaxed 600.0 s, 44703744 rows inserted, instance anchor gap max 16.31 s
host before: load 1.86 2.54 2.79 1/405 64470; io pressure some avg10=2.68 avg60=21.17 avg300=31.88 total=50235589768

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 66 | 0 | 0 | 190 | 11042/17188 | 6246.2/12112.7 | 1745.2/804.3 | 987/311 | 433/177 | 64/0 | 16.31 | 33825.3 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 942 (28.1); log_gate_batch 1595 (632.7); log_gate_run 0 (0.0); page_write 3772 (345.9); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 2742 (597.7); publish_sync 0 (0.0); one step max 634.9, its page writes max 346.0 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 21324/10176 | 359.58/227.41 | 1345.82/737.22 | 1589.7/816.7 | 1731.5/902.9 | 0 |
| 0 | ping | 21324/10176 | 290.69/172.62 | 1296.30/729.24 | 1569.6/778.0 | 1727.8/801.5 | 0 |

### B-c4-group-r1.json: cores=4 durability=group 600.0 s, 55973888 rows inserted, instance anchor gap max 6.71 s
host before: load 2.18 2.32 2.97 1/398 51638; io pressure some avg10=4.22 avg60=30.94 avg300=43.30 total=48967826740

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 118 | 0 | 0 | 45 | 2452/15420 | 646.4/1323.4 | 421.5/318.3 | 1191/9160 | 14/58 | 0/0 | 14.28 | 46479.2 |
| 1 | 117 | 0 | 0 | 48 | 2499/15802 | 637.9/1772.6 | 368.9/286.7 | 1255/9389 | 21/44 | 0/0 | 11.11 | 43764.1 |
| 2 | 117 | 0 | 0 | 49 | 2467/13168 | 434.7/1115.4 | 278.5/172.6 | 248/334 | 6/4 | 0/0 | 15.50 | 2127.9 |
| 3 | 117 | 0 | 0 | 53 | 2675/4262 | 573.6/1036.9 | 251.6/399.1 | 659/3329 | 10/54 | 0/0 | 14.35 | 43718.6 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1787 (32.9); log_gate_batch 403 (359.8); log_gate_run 518 (373.1); page_write 858 (32.6); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 937 (349.1); publish_sync 0 (0.0); one step max 421.4, its page writes max 32.8 |
| 1 | start 1743 (22.4); log_gate_batch 372 (356.7); log_gate_run 25 (4.0); page_write 1787 (210.7); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 1003 (145.8); publish_sync 0 (0.0); one step max 361.8, its page writes max 210.8 |
| 2 | start 1719 (21.9); log_gate_batch 36 (13.8); log_gate_run 290 (253.6); page_write 2231 (264.1); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 765 (51.4); publish_sync 0 (0.0); one step max 278.5, its page writes max 264.2 |
| 3 | start 1734 (22.3); log_gate_batch 64 (13.8); log_gate_run 130 (23.1); page_write 893 (55.5); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 761 (43.4); publish_sync 0 (0.0); one step max 55.8, its page writes max 55.6 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 3718/27782 | 0.73/0.70 | 202.47/124.38 | 384.4/246.5 | 413.2/324.0 | 0 |
| 0 | ping | 3718/27782 | 0.68/0.65 | 198.60/123.44 | 373.6/244.7 | 412.8/313.7 | 0 |
| 1 | stmt | 3606/27894 | 0.67/0.63 | 200.59/84.81 | 305.0/226.8 | 363.2/293.1 | 0 |
| 1 | ping | 3606/27894 | 0.63/0.59 | 199.74/84.75 | 304.3/228.9 | 362.6/276.0 | 0 |
| 2 | stmt | 2673/28827 | 0.54/0.53 | 147.67/13.82 | 247.7/57.6 | 265.5/167.1 | 0 |
| 2 | ping | 2673/28827 | 0.50/0.48 | 148.13/9.20 | 247.5/52.0 | 265.5/167.0 | 0 |
| 3 | stmt | 3450/28050 | 0.65/0.62 | 106.27/118.97 | 217.2/243.7 | 238.2/398.9 | 0 |
| 3 | ping | 3450/28050 | 0.61/0.58 | 106.76/117.64 | 217.6/243.3 | 238.0/398.8 | 0 |

### B-c4-group-r2.json: cores=4 durability=group 600.0 s, 43626496 rows inserted, instance anchor gap max 6.76 s
host before: load 1.15 1.85 2.85 1/410 61612; io pressure some avg10=4.04 avg60=32.33 avg300=46.10 total=50030476549

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 101 | 0 | 0 | 39 | 1984/15288 | 765.7/2092.5 | 554.2/605.8 | 652/3540 | 15/53 | 0/0 | 20.81 | 52449.3 |
| 1 | 100 | 0 | 0 | 37 | 1804/16066 | 514.0/1667.4 | 547.5/577.0 | 207/408 | 4/10 | 0/0 | 20.15 | 1891.2 |
| 2 | 100 | 0 | 0 | 38 | 1962/10365 | 686.3/2175.9 | 614.7/573.6 | 404/1497 | 26/35 | 0/0 | 19.40 | 50011.4 |
| 3 | 100 | 0 | 0 | 38 | 1896/11073 | 521.7/1674.6 | 583.9/577.1 | 205/410 | 3/12 | 0/0 | 20.35 | 1839.5 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1540 (24.0); log_gate_batch 68 (24.5); log_gate_run 4 (2.2); page_write 612 (44.2); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 1322 (545.0); publish_sync 0 (0.0); one step max 44.4, its page writes max 44.3 |
| 1 | start 1537 (27.6); log_gate_batch 108 (89.3); log_gate_run 256 (49.4); page_write 942 (41.0); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 709 (65.5); publish_sync 0 (0.0); one step max 184.5, its page writes max 41.2 |
| 2 | start 1511 (22.7); log_gate_batch 110 (32.4); log_gate_run 236 (25.7); page_write 920 (248.7); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 898 (66.0); publish_sync 0 (0.0); one step max 249.0, its page writes max 248.9 |
| 3 | start 1504 (22.8); log_gate_batch 44 (17.7); log_gate_run 241 (31.1); page_write 914 (50.8); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 1195 (583.9); publish_sync 0 (0.0); one step max 85.4, its page writes max 50.9 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 4058/27442 | 0.67/0.65 | 254.61/168.64 | 480.6/408.0 | 552.9/602.8 | 0 |
| 0 | ping | 4058/27442 | 0.63/0.61 | 254.45/170.36 | 479.8/382.0 | 552.5/602.6 | 0 |
| 1 | stmt | 2934/28566 | 0.56/0.55 | 105.94/21.96 | 508.3/274.8 | 528.0/577.4 | 0 |
| 1 | ping | 2934/28566 | 0.51/0.49 | 92.67/18.34 | 508.1/264.7 | 547.8/557.4 | 0 |
| 2 | stmt | 3501/27999 | 0.64/0.61 | 313.54/95.17 | 554.1/323.8 | 613.8/571.9 | 0 |
| 2 | ping | 3501/27999 | 0.61/0.56 | 311.24/98.30 | 553.9/326.0 | 613.7/571.6 | 0 |
| 3 | stmt | 2933/28567 | 0.56/0.55 | 114.25/23.36 | 532.7/314.3 | 571.8/577.3 | 0 |
| 3 | ping | 2933/28567 | 0.51/0.50 | 114.18/19.72 | 532.6/299.5 | 571.7/557.4 | 0 |

### B-c4-group-r3.json: cores=4 durability=group 600.0 s, 43688960 rows inserted, instance anchor gap max 6.71 s
host before: load 1.11 1.81 2.60 1/408 72435; io pressure some avg10=4.47 avg60=32.77 avg300=45.53 total=51094900296

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 107 | 0 | 0 | 38 | 1776/15349 | 530.6/1718.3 | 595.6/369.4 | 341/1019 | 20/62 | 0/0 | 15.35 | 47601.0 |
| 1 | 106 | 0 | 0 | 38 | 1899/15192 | 819.5/1783.3 | 516.3/392.9 | 696/3783 | 22/68 | 0/0 | 15.57 | 56975.5 |
| 2 | 106 | 0 | 0 | 35 | 1688/12876 | 525.2/1333.8 | 330.4/178.8 | 234/367 | 3/4 | 0/0 | 20.75 | 1495.5 |
| 3 | 106 | 0 | 0 | 39 | 1927/10562 | 723.0/1665.6 | 372.4/521.8 | 403/1585 | 24/61 | 0/0 | 19.46 | 54036.3 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1640 (23.5); log_gate_batch 145 (35.6); log_gate_run 969 (573.9); page_write 943 (46.1); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 698 (233.6); publish_sync 0 (0.0); one step max 595.5, its page writes max 46.2 |
| 1 | start 1621 (27.4); log_gate_batch 151 (48.0); log_gate_run 26 (7.2); page_write 988 (251.7); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 1105 (516.2); publish_sync 0 (0.0); one step max 252.0, its page writes max 251.9 |
| 2 | start 1613 (28.3); log_gate_batch 173 (47.6); log_gate_run 322 (41.4); page_write 1741 (269.8); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 652 (78.9); publish_sync 0 (0.0); one step max 270.1, its page writes max 270.0 |
| 3 | start 1599 (23.0); log_gate_batch 106 (42.2); log_gate_run 285 (37.0); page_write 1084 (372.1); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 805 (55.8); publish_sync 0 (0.0); one step max 372.4, its page writes max 372.3 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 3310/28190 | 0.59/0.56 | 274.37/160.72 | 520.9/311.8 | 579.0/369.7 | 0 |
| 0 | ping | 3310/28190 | 0.55/0.52 | 274.32/160.61 | 520.8/311.6 | 578.8/369.3 | 0 |
| 1 | stmt | 4189/27311 | 0.67/0.65 | 274.92/173.00 | 431.6/321.8 | 510.5/408.6 | 0 |
| 1 | ping | 4189/27311 | 0.65/0.61 | 270.98/176.55 | 431.5/324.1 | 510.4/428.2 | 0 |
| 2 | stmt | 3079/28421 | 0.56/0.55 | 84.26/16.10 | 273.2/68.6 | 331.1/179.1 | 0 |
| 2 | ping | 3079/28421 | 0.52/0.50 | 83.99/12.69 | 274.2/61.7 | 331.8/159.0 | 0 |
| 3 | stmt | 3787/27713 | 0.63/0.61 | 256.25/175.96 | 338.9/326.7 | 369.6/518.3 | 0 |
| 3 | ping | 3787/27713 | 0.59/0.57 | 253.06/177.06 | 339.0/330.3 | 371.6/518.3 | 0 |

### B-c4-relaxed-r1.json: cores=4 durability=relaxed 600.0 s, 56145920 rows inserted, instance anchor gap max 29.45 s
host before: load 1.80 2.92 4.16 1/409 46635; io pressure some avg10=3.91 avg60=30.56 avg300=41.99 total=48453340761

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 11 | 0 | 0 | 295 | 18017/31643 | 10208.0/16186.6 | 1718.2/1992.5 | 159/685 | 130/585 | 17/37 | 72.19 | 290208.4 |
| 1 | 11 | 0 | 0 | 302 | 17155/25861 | 10723.9/14070.9 | 2006.3/1998.5 | 950/3898 | 141/583 | 13/41 | 69.40 | 16640.1 |
| 2 | 10 | 0 | 0 | 249 | 13854/21332 | 11611.1/15821.2 | 2014.9/2009.8 | 2620/11622 | 135/578 | 16/42 | 72.47 | 18381.4 |
| 3 | 10 | 0 | 0 | 400 | 22060/27940 | 15162.8/24782.2 | 1999.5/2006.6 | 3181/11170 | 189/539 | 15/40 | 66.27 | 17356.1 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 586 (474.0); log_gate_batch 292 (49.6); log_gate_run 4259 (605.8); page_write 699 (6.4); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 6294 (1455.6); publish_sync 0 (0.0); one step max 834.5, its page writes max 6.5 |
| 1 | start 114 (15.9); log_gate_batch 414 (51.4); log_gate_run 4786 (651.2); page_write 906 (252.5); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 5949 (753.1); publish_sync 0 (0.0); one step max 899.6, its page writes max 252.6 |
| 2 | start 101 (14.4); log_gate_batch 1355 (588.1); log_gate_run 4549 (650.2); page_write 1017 (251.0); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 5584 (1586.0); publish_sync 0 (0.0); one step max 1565.5, its page writes max 251.1 |
| 3 | start 107 (14.6); log_gate_batch 3547 (627.8); log_gate_run 6110 (652.5); page_write 3069 (296.1); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 10524 (1337.0); publish_sync 0 (0.0); one step max 1450.0, its page writes max 296.3 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 5745/25755 | 165.88/80.48 | 1384.10/1352.95 | 1648.2/1825.5 | 1707.5/1992.7 | 0 |
| 0 | ping | 5745/25755 | 164.08/77.39 | 1384.07/1349.29 | 1647.9/1825.5 | 1707.3/1992.8 | 0 |
| 1 | stmt | 5895/25605 | 198.88/136.14 | 1502.13/1396.71 | 1913.0/1799.2 | 2000.7/1997.6 | 0 |
| 1 | ping | 5895/25605 | 184.80/122.03 | 1498.68/1373.32 | 1912.6/1790.9 | 2000.0/1997.5 | 0 |
| 2 | stmt | 5963/25537 | 213.39/146.18 | 1611.55/1432.84 | 1959.9/1834.8 | 2010.2/2002.3 | 0 |
| 2 | ping | 5963/25537 | 199.19/129.18 | 1606.30/1416.95 | 1959.8/1829.2 | 2010.0/2002.1 | 0 |
| 3 | stmt | 7635/23865 | 55591.63/64616.23 | 125374.68/133183.11 | 126334.0/135195.0 | 126470.7/135358.7 | 0 |
| 3 | ping | 7635/23865 | 353.87/356.24 | 3504.99/5215.01 | 3830.2/5765.7 | 3895.7/5984.1 | 0 |

### B-c4-relaxed-r2.json: cores=4 durability=relaxed 600.0 s, 46760960 rows inserted, instance anchor gap max 36.79 s
host before: load 2.86 3.95 3.78 1/411 56643; io pressure some avg10=4.66 avg60=33.17 avg300=45.82 total=49465154995

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 8 | 0 | 0 | 271 | 15239/40256 | 13819.2/30189.5 | 2878.2/2689.1 | 127/495 | 106/447 | 20/45 | 107.83 | 338704.8 |
| 1 | 8 | 0 | 0 | 253 | 15114/33403 | 15796.9/24794.3 | 2274.7/2699.3 | 179/765 | 117/441 | 16/43 | 112.74 | 14682.7 |
| 2 | 8 | 0 | 0 | 292 | 17657/22747 | 19011.0/29137.5 | 2341.6/2699.8 | 1151/2508 | 140/418 | 20/43 | 112.47 | 16495.1 |
| 3 | 8 | 0 | 0 | 291 | 17596/38831 | 14630.6/31719.4 | 4194.9/2725.4 | 134/446 | 113/423 | 24/44 | 109.77 | 7440.8 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 71 (16.1); log_gate_batch 607 (350.8); log_gate_run 4995 (534.1); page_write 627 (60.2); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 5360 (1041.6); publish_sync 0 (0.0); one step max 2161.9, its page writes max 60.4 |
| 1 | start 67 (16.4); log_gate_batch 1968 (901.3); log_gate_run 1055 (854.5); page_write 354 (45.7); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 4070 (1006.5); publish_sync 0 (0.0); one step max 947.9, its page writes max 45.8 |
| 2 | start 69 (14.7); log_gate_batch 1969 (898.6); log_gate_run 1936 (845.3); page_write 2175 (353.6); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 6922 (1054.7); publish_sync 0 (0.0); one step max 1043.0, its page writes max 353.7 |
| 3 | start 77 (16.6); log_gate_batch 361 (104.2); log_gate_run 4927 (905.6); page_write 505 (3.5); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 12225 (4194.8); publish_sync 0 (0.0); one step max 1073.8, its page writes max 3.7 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 6045/25455 | 317.64/213.22 | 2050.31/1717.70 | 2759.2/2323.6 | 2871.2/2673.2 | 0 |
| 0 | ping | 6045/25455 | 313.73/209.85 | 2044.59/1713.30 | 2755.9/2323.2 | 2870.4/2672.8 | 0 |
| 1 | stmt | 6373/25127 | 330.57/256.74 | 1845.08/1768.01 | 2231.6/2371.1 | 2273.0/2695.5 | 0 |
| 1 | ping | 6373/25127 | 320.85/245.27 | 1840.63/1764.74 | 2222.3/2372.9 | 2272.9/2695.3 | 0 |
| 2 | stmt | 7298/24202 | 55269.16/63988.46 | 138342.47/160356.88 | 139217.9/161809.0 | 139357.3/162280.6 | 0 |
| 2 | ping | 7298/24202 | 1109.59/1172.98 | 5281.55/6155.91 | 6184.9/6860.9 | 6299.1/7100.7 | 0 |
| 3 | stmt | 7118/24382 | 316.44/211.64 | 2792.84/1747.07 | 4042.7/2284.0 | 4179.8/2709.2 | 0 |
| 3 | ping | 7118/24382 | 308.25/200.45 | 2792.73/1736.19 | 4042.6/2271.0 | 4179.7/2709.1 | 0 |

### B-c4-relaxed-r3.json: cores=4 durability=relaxed 600.0 s, 43183104 rows inserted, instance anchor gap max 21.58 s
host before: load 2.20 2.97 2.99 1/396 67100; io pressure some avg10=5.03 avg60=33.31 avg300=46.01 total=50559179848

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 13 | 0 | 0 | 218 | 12829/21688 | 9863.1/17253.4 | 1963.9/2114.7 | 645/2424 | 128/445 | 19/52 | 77.40 | 28525.0 |
| 1 | 12 | 0 | 0 | 201 | 11018/21566 | 9152.5/18964.2 | 2043.8/2112.7 | 473/1582 | 118/453 | 24/52 | 73.24 | 19676.7 |
| 2 | 12 | 0 | 0 | 191 | 10876/20960 | 8502.2/16800.4 | 2084.6/2097.2 | 123/492 | 94/442 | 21/52 | 69.87 | 3636.8 |
| 3 | 12 | 0 | 0 | 263 | 15455/23689 | 9027.4/19492.1 | 2171.5/2028.3 | 136/488 | 101/434 | 23/49 | 74.41 | 1185.5 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 148 (18.8); log_gate_batch 2721 (903.4); log_gate_run 1848 (840.8); page_write 1759 (353.5); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 3501 (952.1); publish_sync 0 (0.0); one step max 1047.9, its page writes max 353.7 |
| 1 | start 135 (18.7); log_gate_batch 2145 (843.1); log_gate_run 8092 (862.7); page_write 500 (53.2); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 6521 (1005.9); publish_sync 0 (0.0); one step max 1181.9, its page writes max 53.4 |
| 2 | start 140 (18.9); log_gate_batch 1152 (840.8); log_gate_run 5491 (958.6); page_write 473 (60.4); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 3665 (1067.2); publish_sync 0 (0.0); one step max 2084.6, its page writes max 60.6 |
| 3 | start 154 (19.1); log_gate_batch 1054 (459.1); log_gate_run 4649 (950.1); page_write 507 (45.3); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 7773 (2013.2); publish_sync 0 (0.0); one step max 2171.5, its page writes max 45.5 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 6390/25110 | 18752.61/21774.02 | 48991.83/49913.30 | 49435.0/51007.3 | 49494.6/51458.8 | 0 |
| 0 | ping | 6390/25110 | 347.36/336.95 | 1419.80/1631.43 | 1866.4/1955.2 | 1957.3/2103.3 | 0 |
| 1 | stmt | 6511/24989 | 323.99/200.89 | 1748.42/1427.36 | 1957.1/1908.8 | 2032.2/2103.3 | 0 |
| 1 | ping | 6511/24989 | 307.34/186.82 | 1748.14/1424.91 | 1955.3/1907.2 | 2031.9/2103.3 | 0 |
| 2 | stmt | 5521/25979 | 243.67/140.74 | 1605.92/1476.33 | 1986.0/1912.9 | 2083.8/2097.6 | 0 |
| 2 | ping | 5521/25979 | 233.69/130.16 | 1597.70/1463.41 | 1985.9/1909.6 | 2074.8/2097.7 | 0 |
| 3 | stmt | 5917/25583 | 269.05/135.60 | 1780.34/1450.59 | 2091.3/1873.0 | 2170.8/2028.8 | 0 |
| 3 | ping | 5917/25583 | 259.75/125.06 | 1769.93/1437.20 | 2077.7/1853.8 | 2170.7/2009.2 | 0 |

### A-c4-group-idle.json: cores=4 durability=group 600.0 s, 43487232 rows inserted, instance anchor gap max 2.10 s
host before: load 3.32 4.07 3.76 1/396 77435; io pressure some avg10=3.84 avg60=22.98 avg300=31.96 total=51513585296

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 120 | 0 | 0 | 25 | 1540/13882 | 739.8/3430.3 | 3430.6/446.7 | 126/8834 | 120/79 | 17/0 | 5.66 | 138771.7 |
| 1 | 118 | 0 | 0 | 21 | 1322/3562 | 639.9/985.5 | 1004.3/418.1 | 124/9085 | 118/83 | 1/0 | 5.67 | 122927.8 |
| 2 | 118 | 0 | 0 | 25 | 1540/3599 | 727.4/1131.9 | 1131.9/416.2 | 124/8492 | 118/79 | 16/0 | 5.71 | 134577.4 |
| 3 | 119 | 0 | 0 | 22 | 1345/3386 | 579.9/1112.2 | 1112.4/395.6 | 125/8999 | 119/95 | 3/0 | 5.73 | 124229.5 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1346 (16.7); log_gate_batch 19812 (535.5); log_gate_run 3779 (387.1); page_write 579 (6.0); map_write 29 (0.1); claim_wait 0 (0.0); data_sync 46081 (493.8); end_wait 1456 (158.4); publish_writeback 7052 (570.4); publish_sync 7728 (481.3); one step max 577.8, its page writes max 6.1 |
| 1 | start 1358 (16.9); log_gate_batch 16832 (525.1); log_gate_run 3401 (380.6); page_write 576 (36.6); map_write 22 (0.0); claim_wait 0 (0.0); data_sync 36254 (519.3); end_wait 1097 (154.0); publish_writeback 5284 (491.8); publish_sync 6591 (405.3); one step max 559.1, its page writes max 37.5 |
| 2 | start 1331 (20.1); log_gate_batch 17120 (525.2); log_gate_run 4035 (434.7); page_write 552 (8.0); map_write 24 (0.0); claim_wait 0 (0.0); data_sync 43188 (539.4); end_wait 1791 (487.1); publish_writeback 5693 (552.8); publish_sync 8223 (606.1); one step max 587.3, its page writes max 8.2 |
| 3 | start 1355 (17.1); log_gate_batch 16825 (487.6); log_gate_run 4642 (410.1); page_write 550 (60.8); map_write 21 (0.0); claim_wait 0 (0.0); data_sync 35328 (422.5); end_wait 1323 (517.4); publish_writeback 4792 (534.2); publish_sync 5516 (268.4); one step max 580.2, its page writes max 61.1 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 4452/27048 | 395.87/0.61 | 2546.88/241.85 | 3340.4/852.1 | 3413.6/3433.5 | 0 |
| 0 | ping | 4452/27048 | 393.39/0.57 | 2544.30/219.87 | 3339.8/346.5 | 3413.5/891.4 | 0 |
| 1 | stmt | 3595/27905 | 325.07/0.60 | 903.66/211.68 | 968.4/395.5 | 992.0/1003.2 | 0 |
| 1 | ping | 3595/27905 | 323.81/0.56 | 902.03/195.02 | 968.3/329.3 | 991.8/971.4 | 0 |
| 2 | stmt | 4156/27344 | 372.92/0.60 | 1001.47/215.63 | 1066.6/736.5 | 1112.3/1132.1 | 0 |
| 2 | ping | 4156/27344 | 369.30/0.56 | 1000.85/199.92 | 1066.5/333.3 | 1112.1/1132.0 | 0 |
| 3 | stmt | 3552/27948 | 318.65/0.60 | 916.85/210.89 | 1051.7/380.5 | 1111.4/1131.3 | 0 |
| 3 | ping | 3552/27948 | 316.32/0.56 | 910.38/194.64 | 1051.6/324.7 | 1111.3/422.3 | 0 |

### A-c4-relaxed-idle.json: cores=4 durability=relaxed 600.0 s, 52056064 rows inserted, instance anchor gap max 12.61 s
host before: load 3.16 3.30 3.07 2/403 74947; io pressure some avg10=2.59 avg60=20.21 avg300=30.85 total=51292301313

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 9 | 0 | 0 | 37 | 2334/14336 | 1845.4/6484.5 | 6484.6/1746.6 | 15/10966 | 10/687 | 9/55 | 25.69 | 72851.0 |
| 1 | 25 | 0 | 0 | 82 | 5211/7664 | 8246.7/12452.9 | 12626.5/1639.0 | 31/9428 | 27/512 | 25/38 | 185.83 | 196074.1 |
| 2 | 50 | 0 | 0 | 60 | 3804/8223 | 7046.1/12594.4 | 12594.4/1582.9 | 55/5288 | 51/316 | 50/17 | 156.91 | 353349.7 |
| 3 | 8 | 0 | 0 | 24 | 1489/7072 | 1877.0/7784.2 | 7784.3/1665.0 | 14/9011 | 9/677 | 8/54 | 20.97 | 37219.8 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 32 (5.1); log_gate_batch 1998 (532.9); log_gate_run 1024 (303.2); page_write 95 (2.1); map_write 6 (0.0); claim_wait 0 (0.0); data_sync 10478 (494.7); end_wait 856 (343.4); publish_writeback 3784 (777.8); publish_sync 3324 (1256.7); one step max 719.9, its page writes max 2.1 |
| 1 | start 3255 (728.5); log_gate_batch 9583 (854.9); log_gate_run 27990 (866.3); page_write 450 (6.8); map_write 18 (0.9); claim_wait 0 (0.0); data_sync 70957 (762.9); end_wait 3181 (944.6); publish_writeback 39956 (3191.8); publish_sync 27526 (2265.8); one step max 2938.5, its page writes max 6.9 |
| 2 | start 3815 (750.4); log_gate_batch 16133 (936.5); log_gate_run 58344 (936.5); page_write 1158 (203.2); map_write 31 (0.1); claim_wait 0 (0.0); data_sync 128399 (855.1); end_wait 6984 (946.4); publish_writeback 75304 (3673.5); publish_sync 55074 (1769.5); one step max 3015.4, its page writes max 203.5 |
| 3 | start 544 (514.7); log_gate_batch 2092 (619.8); log_gate_run 1071 (356.4); page_write 85 (1.4); map_write 4 (0.0); claim_wait 0 (0.0); data_sync 12689 (556.9); end_wait 724 (551.5); publish_writeback 3393 (1483.6); publish_sync 3873 (1353.9); one step max 788.7, its page writes max 1.5 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 1097/30403 | 2512.37/213.61 | 8175.82/2747.54 | 8238.3/4492.0 | 8258.1/4768.8 | 0 |
| 0 | ping | 1097/30403 | 1939.21/174.77 | 8076.23/1564.69 | 8219.2/3265.2 | 8238.3/3521.6 | 0 |
| 1 | stmt | 9172/22328 | 100773.87/300.04 | 151106.91/66017.47 | 152019.3/152695.8 | 152168.0/153071.3 | 0 |
| 1 | ping | 9172/22328 | 88918.02/206.02 | 150561.04/46434.30 | 151921.9/152499.1 | 152086.5/152891.2 | 0 |
| 2 | stmt | 17328/14172 | 114293.69/9147.39 | 171835.38/163280.48 | 173807.5/170463.8 | 174100.5/174105.3 | 0 |
| 2 | ping | 17328/14172 | 106587.98/364.05 | 171115.73/161014.53 | 173515.6/169442.9 | 173823.7/169562.7 | 0 |
| 3 | stmt | 1227/30273 | 6009.02/154.18 | 14684.90/2026.08 | 14867.3/12835.5 | 14870.0/13323.2 | 0 |
| 3 | ping | 1227/30273 | 4125.45/130.68 | 13985.75/1335.19 | 14187.1/12591.0 | 14190.0/13136.2 | 0 |
