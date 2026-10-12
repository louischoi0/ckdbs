
### c1-relaxed-r1.json: cores=1 durability=relaxed 600.0 s, 54474752 rows inserted, instance anchor gap max 5.68 s
host before: load 2.06 2.80 3.64 1/512 4012766; io pressure some avg10=3.51 avg60=27.19 avg300=36.25 total=42409352981

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 120 | 0 | 0 | 160 | 10227/13313 | 827.7/1404.0 | 1404.0/520.9 | 120/1746 | 120/639 | 27/0 | 5.68 | 145319.1 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1967 (51.2); log_gate_batch 8889 (465.0); log_gate_run 0 (0.0); page_write 2342 (39.6); map_write 10 (0.0); claim_wait 0 (0.0); data_sync 91412 (363.6); end_wait 309 (9.9); publish_writeback 1943 (25.1); publish_sync 481 (13.4); one step max 471.4, its page writes max 39.7 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 5444/24556 | 646.57/189.45 | 1253.00/1259.14 | 1352.4/1421.8 | 1390.0/1543.5 | 0 |
| 0 | ping | 5444/24556 | 562.97/128.67 | 1242.03/1233.18 | 1338.3/1400.4 | 1389.9/1523.5 | 0 |

### c1-relaxed-r2.json: cores=1 durability=relaxed 600.0 s, 54119424 rows inserted, instance anchor gap max 5.68 s
host before: load 3.06 2.90 2.51 1/543 4028226; io pressure some avg10=25.41 avg60=26.12 avg300=28.20 total=43344931749

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 120 | 0 | 0 | 160 | 10187/11504 | 826.3/1421.4 | 1421.4/647.3 | 120/1398 | 120/638 | 24/0 | 5.68 | 142075.4 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1871 (24.3); log_gate_batch 7947 (522.4); log_gate_run 0 (0.0); page_write 2367 (0.2); map_write 10 (0.0); claim_wait 0 (0.0); data_sync 90564 (53.2); end_wait 318 (9.1); publish_writeback 1928 (25.1); publish_sync 402 (11.5); one step max 528.7, its page writes max 0.3 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 5342/24658 | 617.81/185.76 | 1268.57/1266.59 | 1362.8/1410.9 | 1407.4/1513.5 | 0 |
| 0 | ping | 5342/24658 | 545.51/130.33 | 1257.09/1237.03 | 1358.0/1390.9 | 1407.3/1493.5 | 0 |

### c1-relaxed-r3.json: cores=1 durability=relaxed 600.0 s, 42448896 rows inserted, instance anchor gap max 5.95 s
host before: load 2.34 2.46 2.53 1/516 4040197; io pressure some avg10=30.38 avg60=33.60 avg300=35.52 total=44338173290

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 120 | 0 | 0 | 121 | 7701/13223 | 655.9/2005.1 | 2005.3/811.6 | 120/1265 | 120/489 | 23/0 | 5.95 | 124869.5 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1699 (40.2); log_gate_batch 16107 (747.6); log_gate_run 0 (0.0); page_write 1832 (0.3); map_write 7 (0.0); claim_wait 0 (0.0); data_sync 72967 (44.9); end_wait 293 (16.3); publish_writeback 1739 (24.6); publish_sync 483 (15.5); one step max 753.0, its page writes max 0.5 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 4791/25209 | 644.29/409.17 | 1567.71/1407.06 | 1956.1/1707.0 | 2015.4/2008.8 | 0 |
| 0 | ping | 4791/25209 | 533.55/294.74 | 1457.59/1321.56 | 1945.6/1431.0 | 1992.4/1565.8 | 0 |

### c4-relaxed-r1.json: cores=4 durability=relaxed 600.0 s, 45523968 rows inserted, instance anchor gap max 17.70 s
host before: load 2.23 3.61 4.50 3/556 4067349; io pressure some avg10=7.69 avg60=21.96 avg300=29.35 total=45402575715

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 4 | 0 | 0 | 53 | 3376/14589 | 2630.0/7632.5 | 7632.7/2338.1 | 4/9595 | 4/737 | 4/50 | 34.45 | 63114.1 |
| 1 | 20 | 0 | 0 | 61 | 3886/6753 | 5584.9/11788.7 | 11788.9/2337.4 | 20/9295 | 20/587 | 20/39 | 21.99 | 129119.5 |
| 2 | 2 | 0 | 0 | 34 | 2165/3098 | 2880.5/4155.5 | 4155.7/2333.1 | 2/5276 | 2/737 | 2/51 | 41.31 | 21530.3 |
| 3 | 42 | 0 | 0 | 96 | 6100/11770 | 10557.0/17697.0 | 17697.2/1638.3 | 43/187 | 43/158 | 42/11 | 114.49 | 442024.5 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 8 (3.5); log_gate_batch 1569 (744.2); log_gate_run 601 (412.0); page_write 63 (0.7); map_write 4 (0.0); claim_wait 0 (0.0); data_sync 6484 (409.6); end_wait 784 (350.4); publish_writeback 3916 (1491.1); publish_sync 1650 (1250.1); one step max 764.5, its page writes max 0.9 |
| 1 | start 962 (385.3); log_gate_batch 1140 (496.5); log_gate_run 9730 (914.4); page_write 369 (94.3); map_write 11 (0.0); claim_wait 0 (0.0); data_sync 63147 (766.3); end_wait 3272 (941.1); publish_writeback 18675 (2217.2); publish_sync 21312 (1854.8); one step max 2100.8, its page writes max 94.5 |
| 2 | start 7 (5.7); log_gate_batch 489 (480.5); log_gate_run 39 (12.6); page_write 13 (1.1); map_write 1 (0.0); claim_wait 0 (0.0); data_sync 3499 (571.3); end_wait 557 (550.6); publish_writeback 901 (813.6); publish_sync 1038 (599.3); one step max 571.6, its page writes max 1.2 |
| 3 | start 3002 (514.4); log_gate_batch 15137 (940.8); log_gate_run 76835 (948.0); page_write 880 (6.3); map_write 35 (0.1); claim_wait 0 (0.0); data_sync 170497 (765.9); end_wait 8259 (957.1); publish_writeback 101615 (5351.5); publish_sync 61881 (2425.6); one step max 4214.2, its page writes max 6.4 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 777/29223 | 16007.14/117548.72 | 23190.69/264969.88 | 23226.9/267550.0 | 23226.9/268111.4 | 0 |
| 0 | ping | 777/29223 | 11232.35/19794.74 | 13450.01/57606.97 | 13546.4/57976.9 | 13546.4/58133.1 | 0 |
| 1 | stmt | 5955/24045 | 68488.91/365.60 | 99314.53/47684.03 | 100248.1/97823.9 | 100319.1/98428.2 | 0 |
| 1 | ping | 5955/24045 | 57407.38/257.45 | 98222.41/32942.32 | 99187.9/97361.0 | 99259.1/97809.8 | 0 |
| 2 | stmt | 352/29648 | 2166.38/199.01 | 4113.79/1458.63 | 4173.5/3283.3 | 4173.5/4403.8 | 0 |
| 2 | ping | 352/29648 | 2088.24/177.43 | 4106.47/1449.13 | 4166.1/3223.1 | 4166.1/4383.7 | 0 |
| 3 | stmt | 21922/8078 | 213103.87/75.48 | 429436.94/352781.54 | 433377.6/434372.9 | 433795.7/434532.0 | 0 |
| 3 | ping | 21922/8078 | 213040.12/64.15 | 429431.38/352453.29 | 433374.5/433992.5 | 433793.2/434145.9 | 0 |

### c4-relaxed-r2.json: cores=4 durability=relaxed 600.0 s, 57761792 rows inserted, instance anchor gap max 11.11 s
host before: load 1.78 1.93 2.15 1/528 4031281; io pressure some avg10=41.39 avg60=42.66 avg300=41.54 total=43610410537

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 10 | 0 | 0 | 51 | 3206/15288 | 2139.3/9121.7 | 9121.9/1452.4 | 10/5680 | 10/824 | 10/31 | 40.11 | 64309.7 |
| 1 | 71 | 0 | 0 | 87 | 5524/9930 | 6149.1/11106.1 | 11106.3/1166.3 | 72/201 | 71/184 | 71/3 | 82.65 | 443650.5 |
| 2 | 23 | 0 | 0 | 56 | 3580/5472 | 4348.3/6803.0 | 6803.0/1432.4 | 23/11834 | 23/750 | 23/30 | 39.74 | 110783.3 |
| 3 | 3 | 0 | 0 | 58 | 3650/4734 | 3240.6/3536.3 | 3616.9/1432.2 | 4/12109 | 4/855 | 3/32 | 14.81 | 25283.6 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 41 (7.8); log_gate_batch 3362 (578.5); log_gate_run 1221 (369.6); page_write 110 (2.5); map_write 6 (0.0); claim_wait 0 (0.0); data_sync 13610 (601.2); end_wait 1404 (333.3); publish_writeback 3670 (770.4); publish_sync 4776 (1576.1); one step max 716.0, its page writes max 2.6 |
| 1 | start 4457 (438.8); log_gate_batch 15686 (650.5); log_gate_run 82957 (656.9); page_write 1431 (40.7); map_write 49 (0.1); claim_wait 0 (0.0); data_sync 150492 (599.7); end_wait 10100 (638.1); publish_writeback 106639 (3140.1); publish_sync 67992 (1549.7); one step max 2537.9, its page writes max 41.1 |
| 2 | start 374 (76.7); log_gate_batch 2957 (586.6); log_gate_run 13751 (654.1); page_write 255 (5.0); map_write 10 (0.0); claim_wait 0 (0.0); data_sync 41699 (586.6); end_wait 1918 (350.6); publish_writeback 17588 (1716.8); publish_sync 18421 (1174.8); one step max 1556.9, its page writes max 5.1 |
| 3 | start 7 (3.3); log_gate_batch 603 (592.9); log_gate_run 62 (24.6); page_write 46 (2.8); map_write 2 (0.0); claim_wait 0 (0.0); data_sync 6193 (592.0); end_wait 19 (9.7); publish_writeback 1422 (683.6); publish_sync 1439 (591.8); one step max 610.6, its page writes max 3.0 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 1415/28585 | 2748.90/118.76 | 13084.46/1306.24 | 13228.8/3237.7 | 13248.7/9128.8 | 0 |
| 0 | ping | 1415/28585 | 2617.49/98.72 | 12946.18/1141.01 | 13188.8/3196.4 | 13208.8/3483.5 | 0 |
| 1 | stmt | 22038/7962 | 169330.14/41.83 | 386149.88/229178.28 | 390071.8/358261.0 | 390509.9/379716.8 | 0 |
| 1 | ping | 22038/7962 | 168997.82/31.13 | 385582.96/229090.93 | 389511.5/358234.6 | 389929.9/379699.6 | 0 |
| 2 | stmt | 4861/25139 | 190677.79/156865.53 | 225828.33/291937.40 | 226291.5/294147.2 | 226343.0/294347.2 | 0 |
| 2 | ping | 4861/25139 | 74191.50/43125.46 | 78045.24/78209.34 | 78376.4/79001.1 | 78426.0/79246.5 | 0 |
| 3 | stmt | 492/29508 | 2418.44/118.89 | 4072.44/1035.83 | 4143.8/3458.7 | 4143.8/4027.2 | 0 |
| 3 | ping | 492/29508 | 2287.49/100.13 | 4052.29/997.59 | 4123.6/3294.1 | 4123.6/4007.3 | 0 |

### c4-relaxed-r3.json: cores=4 durability=relaxed 600.0 s, 28474368 rows inserted, instance anchor gap max 79.51 s
host before: load 2.06 2.13 2.33 1/505 4043324; io pressure some avg10=51.47 avg60=50.68 avg300=48.20 total=44643553942

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 13 | 0 | 0 | 92 | 5833/14849 | 11194.6/46991.1 | 46991.2/18467.1 | 14/666 | 14/364 | 14/65 | 288.66 | 446142.7 |
| 1 | 18 | 0 | 0 | 67 | 4282/11247 | 7567.7/78945.9 | 78968.8/12104.3 | 18/2853 | 18/324 | 18/58 | 206.05 | 234936.7 |
| 2 | 0 | 0 | 0 | - | -/- | -/- | 0.0/18480.3 | 0/6309 | 0/503 | 0/104 | - | 10749.6 |
| 3 | 21 | 0 | 0 | 70 | 4465/6811 | 6446.0/13300.9 | 13301.1/18481.8 | 22/4708 | 22/316 | 21/84 | 187.85 | 169154.6 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 293 (147.0); log_gate_batch 15840 (5300.6); log_gate_run 24894 (2739.6); page_write 627 (147.5); map_write 12 (0.3); claim_wait 0 (0.0); data_sync 68605 (1252.1); end_wait 2079 (538.8); publish_writeback 66111 (23153.5); publish_sync 28080 (6440.2); one step max 13182.4, its page writes max 147.6 |
| 1 | start 833 (511.1); log_gate_batch 16654 (10401.7); log_gate_run 25086 (2173.1); page_write 1661 (407.0); map_write 14 (0.0); claim_wait 0 (0.0); data_sync 73005 (1008.9); end_wait 4011 (888.1); publish_writeback 79618 (42627.2); publish_sync 23871 (2617.8); one step max 11766.2, its page writes max 407.1 |
| 2 | start 0 (0.0); log_gate_batch 0 (0.0); log_gate_run 0 (0.0); page_write 0 (0.0); map_write 0 (0.0); claim_wait 0 (0.0); data_sync 0 (0.0); end_wait 0 (0.0); publish_writeback 0 (0.0); publish_sync 0 (0.0); one step max 0.0, its page writes max 0.0 |
| 3 | start 2017 (715.2); log_gate_batch 1442 (452.8); log_gate_run 18141 (957.6); page_write 480 (97.5); map_write 12 (0.0); claim_wait 0 (0.0); data_sync 74450 (804.7); end_wait 3593 (1046.8); publish_writeback 23145 (2873.2); publish_sync 30483 (3262.7); one step max 2146.6, its page writes max 97.6 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 10154/19846 | 28336.12/255.18 | 76498.85/42483.90 | 78304.9/80310.1 | 78503.5/80686.2 | 0 |
| 0 | ping | 10154/19846 | 28271.20/246.26 | 76487.34/42392.05 | 78299.0/80268.5 | 78497.8/80645.8 | 0 |
| 1 | stmt | 11395/18605 | 139019.34/18975.14 | 236637.08/240014.89 | 238540.2/240624.0 | 238742.5/240765.8 | 0 |
| 1 | ping | 11395/18605 | 107846.64/16652.25 | 150805.47/152628.58 | 152067.2/155743.7 | 152230.0/156064.7 | 0 |
| 2 | stmt | 0/30000 | -/770.41 | -/34361.99 | -/35800.6 | -/36246.5 | 0 |
| 2 | ping | 0/30000 | -/521.64 | -/16976.64 | -/18923.7 | -/19150.1 | 0 |
| 3 | stmt | 7713/22287 | 134875.46/142482.60 | 245409.02/317157.55 | 246799.3/320261.8 | 246938.2/320684.5 | 0 |
| 3 | ping | 7713/22287 | 93640.98/50874.75 | 128691.57/153897.88 | 129635.6/155016.7 | 129729.1/155327.5 | 0 |

### c1-group-r1.json: cores=1 durability=group 600.0 s, 43945984 rows inserted, instance anchor gap max 5.66 s
host before: load 0.26 0.71 1.93 1/543 4022300; io pressure some avg10=0.00 avg60=0.00 avg300=4.73 total=42903620074

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 120 | 0 | 0 | 129 | 8233/9435 | 555.4/1125.9 | 1126.2/459.5 | 120/12218 | 120/530 | 2/0 | 5.66 | 456521.4 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1777 (25.7); log_gate_batch 1 (0.0); log_gate_run 0 (0.0); page_write 1938 (0.8); map_write 7 (0.0); claim_wait 0 (0.0); data_sync 60336 (462.4); end_wait 581 (20.3); publish_writeback 1820 (24.8); publish_sync 279 (14.0); one step max 462.7, its page writes max 0.9 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 3405/26595 | 485.87/90.64 | 938.48/890.97 | 1088.1/1014.5 | 1122.4/1127.6 | 0 |
| 0 | ping | 3405/26595 | 423.97/25.98 | 911.30/860.77 | 1078.9/988.2 | 1107.6/1065.7 | 0 |

### c1-group-r2.json: cores=1 durability=group 600.0 s, 43451392 rows inserted, instance anchor gap max 5.56 s
host before: load 4.18 3.98 3.19 1/525 4034265; io pressure some avg10=26.41 avg60=32.59 avg300=35.05 total=43826462953

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 120 | 0 | 0 | 128 | 8165/10026 | 685.4/1130.0 | 1140.7/543.5 | 120/12348 | 120/579 | 2/0 | 5.56 | 458992.4 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1805 (45.2); log_gate_batch 315 (311.2); log_gate_run 0 (0.0); page_write 1836 (0.9); map_write 8 (0.0); claim_wait 0 (0.0); data_sync 75888 (144.7); end_wait 360 (12.2); publish_writeback 1831 (24.6); publish_sync 485 (18.2); one step max 455.9, its page writes max 1.1 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 4180/25820 | 578.47/153.22 | 983.13/1079.63 | 1086.0/1199.6 | 1128.7/1282.0 | 0 |
| 0 | ping | 4180/25820 | 514.23/94.17 | 942.14/1053.54 | 1071.2/1169.2 | 1125.5/1262.0 | 0 |

### c1-group-r3.json: cores=1 durability=group 600.0 s, 26693632 rows inserted, instance anchor gap max 17.07 s
host before: load 12.94 10.43 6.46 7/565 4053534; io pressure some avg10=46.43 avg60=59.26 avg300=54.10 total=44949086204

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 119 | 0 | 0 | 100 | 6356/9127 | 689.9/12752.8 | 12753.0/1188.3 | 119/4324 | 116/736 | 40/2 | 17.07 | 503185.9 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1088 (21.6); log_gate_batch 23 (3.9); log_gate_run 0 (0.0); page_write 1817 (157.6); map_write 5 (0.0); claim_wait 0 (0.0); data_sync 123321 (1308.2); end_wait 3383 (601.3); publish_writeback 1248 (128.7); publish_sync 6957 (837.4); one step max 1308.3, its page writes max 157.8 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 6931/23069 | 44750.62/36936.36 | 73902.14/74499.68 | 75899.7/76573.8 | 75990.6/76935.5 | 0 |
| 0 | ping | 6931/23069 | 6298.26/511.29 | 70792.62/64594.37 | 71218.2/73293.6 | 71293.4/73649.6 | 0 |

### c4-group-r1.json: cores=4 durability=group 600.0 s, 48915456 rows inserted, instance anchor gap max 2.13 s
host before: load 1.80 1.77 1.96 1/551 4025353; io pressure some avg10=42.08 avg60=44.62 avg300=40.14 total=43180948174

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 120 | 0 | 0 | 31 | 1954/13902 | 374.3/1920.7 | 1920.9/455.8 | 120/2837 | 120/38 | 1/0 | 5.45 | 84905.9 |
| 1 | 118 | 0 | 0 | 41 | 2561/3640 | 805.9/1158.2 | 1158.2/140.1 | 118/360 | 118/2 | 29/0 | 5.55 | 93762.8 |
| 2 | 119 | 0 | 0 | 24 | 1498/2588 | 649.4/1057.1 | 1057.2/139.3 | 119/416 | 119/2 | 1/0 | 5.51 | 73678.7 |
| 3 | 119 | 0 | 0 | 33 | 2106/3518 | 564.1/1141.5 | 1141.5/250.0 | 119/1724 | 119/30 | 1/0 | 5.62 | 94385.6 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1846 (23.5); log_gate_batch 14162 (398.8); log_gate_run 253 (12.5); page_write 546 (1.1); map_write 12 (0.0); claim_wait 0 (0.0); data_sync 30478 (378.4); end_wait 834 (39.5); publish_writeback 3769 (416.9); publish_sync 1065 (118.8); one step max 436.2, its page writes max 1.2 |
| 1 | start 1825 (24.3); log_gate_batch 16156 (445.0); log_gate_run 2215 (48.9); page_write 762 (4.5); map_write 27 (0.1); claim_wait 0 (0.0); data_sync 53681 (455.5); end_wait 1598 (438.7); publish_writeback 7911 (486.0); publish_sync 7103 (451.2); one step max 465.3, its page writes max 4.6 |
| 2 | start 1795 (23.7); log_gate_batch 16480 (494.9); log_gate_run 4415 (462.8); page_write 679 (188.6); map_write 17 (0.0); claim_wait 0 (0.0); data_sync 30972 (435.8); end_wait 1548 (48.5); publish_writeback 9947 (541.5); publish_sync 5929 (428.0); one step max 500.7, its page writes max 188.7 |
| 3 | start 1829 (23.1); log_gate_batch 18044 (455.5); log_gate_run 2999 (457.3); page_write 528 (1.4); map_write 18 (0.0); claim_wait 0 (0.0); data_sync 37671 (428.6); end_wait 1058 (43.3); publish_writeback 5798 (437.0); publish_sync 3282 (137.8); one step max 497.6, its page writes max 1.5 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 2670/27330 | 238.39/0.62 | 1411.41/60.83 | 1874.9/401.7 | 1914.7/1934.6 | 0 |
| 0 | ping | 2670/27330 | 235.65/0.56 | 1411.14/53.83 | 1874.8/365.5 | 1914.6/738.8 | 0 |
| 1 | stmt | 4639/25361 | 396.44/0.48 | 1033.77/17.04 | 1104.0/526.7 | 1158.0/1060.3 | 0 |
| 1 | ping | 4639/25361 | 396.07/0.43 | 1033.65/11.61 | 1103.9/101.8 | 1157.9/1060.5 | 0 |
| 2 | stmt | 3617/26383 | 309.51/0.48 | 845.31/17.99 | 987.5/56.5 | 1047.2/139.6 | 0 |
| 2 | ping | 3617/26383 | 309.20/0.43 | 845.17/11.81 | 987.3/49.1 | 1047.1/139.7 | 0 |
| 3 | stmt | 3563/26437 | 307.27/0.54 | 855.20/56.25 | 1081.8/767.9 | 1141.4/942.9 | 0 |
| 3 | ping | 3563/26437 | 306.30/0.49 | 855.10/45.62 | 1081.6/583.0 | 1141.4/910.1 | 0 |

### c4-group-r2.json: cores=4 durability=group 600.0 s, 41497600 rows inserted, instance anchor gap max 3.36 s
host before: load 1.68 2.01 2.48 1/523 4037223; io pressure some avg10=48.52 avg60=49.74 avg300=48.21 total=44131029472

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 76 | 0 | 0 | 12 | 756/12856 | 417.0/3668.3 | 3668.4/391.8 | 76/3144 | 76/126 | 2/0 | 20.53 | 104377.4 |
| 1 | 118 | 0 | 0 | 34 | 2126/3706 | 1188.3/2125.7 | 2125.7/317.7 | 119/344 | 119/6 | 69/0 | 6.04 | 139044.8 |
| 2 | 71 | 0 | 0 | 13 | 773/1360 | 421.1/861.3 | 861.3/425.5 | 71/3544 | 70/132 | 0/0 | 25.55 | 96308.6 |
| 3 | 119 | 0 | 0 | 34 | 2141/3565 | 1170.1/2173.9 | 2173.9/155.4 | 119/354 | 119/7 | 74/0 | 5.94 | 138000.1 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1019 (20.6); log_gate_batch 13962 (520.8); log_gate_run 806 (109.5); page_write 199 (43.0); map_write 8 (0.0); claim_wait 0 (0.0); data_sync 13729 (380.4); end_wait 717 (63.6); publish_writeback 2363 (74.0); publish_sync 2192 (373.3); one step max 559.3, its page writes max 43.3 |
| 1 | start 1561 (20.3); log_gate_batch 18331 (551.2); log_gate_run 6308 (551.2); page_write 984 (48.1); map_write 34 (0.0); claim_wait 0 (0.0); data_sync 78430 (555.2); end_wait 1202 (91.4); publish_writeback 11609 (668.8); publish_sync 17822 (685.6); one step max 629.0, its page writes max 48.4 |
| 2 | start 929 (20.5); log_gate_batch 12679 (510.0); log_gate_run 2040 (108.9); page_write 218 (44.4); map_write 6 (0.0); claim_wait 0 (0.0); data_sync 9878 (407.8); end_wait 623 (144.6); publish_writeback 2726 (591.3); publish_sync 1208 (50.3); one step max 576.4, its page writes max 44.7 |
| 3 | start 1570 (21.1); log_gate_batch 20365 (549.3); log_gate_run 5732 (532.5); page_write 810 (37.2); map_write 35 (0.1); claim_wait 0 (0.0); data_sync 73945 (556.4); end_wait 1479 (398.9); publish_writeback 14287 (680.4); publish_sync 17292 (688.4); one step max 613.3, its page writes max 37.5 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 1752/28248 | 262.54/0.62 | 3330.36/232.45 | 3637.5/339.2 | 3657.4/503.8 | 0 |
| 0 | ping | 1752/28248 | 261.40/0.58 | 3329.94/231.77 | 3637.4/338.6 | 3657.3/463.2 | 0 |
| 1 | stmt | 6906/23094 | 590.92/0.48 | 1835.95/18.45 | 2024.9/122.2 | 2124.4/318.2 | 0 |
| 1 | ping | 6906/23094 | 589.71/0.42 | 1835.86/13.05 | 2024.7/110.8 | 2124.3/298.3 | 0 |
| 2 | stmt | 1531/28469 | 241.18/0.62 | 727.33/248.39 | 834.6/373.1 | 854.5/842.0 | 0 |
| 2 | ping | 1531/28469 | 240.09/0.58 | 727.21/245.21 | 834.5/348.8 | 854.4/643.8 | 0 |
| 3 | stmt | 6869/23131 | 582.85/0.48 | 1837.88/18.62 | 2093.4/71.7 | 2172.7/156.0 | 0 |
| 3 | ping | 6869/23131 | 582.11/0.42 | 1837.63/13.64 | 2093.0/65.8 | 2172.6/135.7 | 0 |

### c4-group-r3.json: cores=4 durability=group 600.0 s, 34633728 rows inserted, instance anchor gap max 12.15 s
host before: load 3.00 4.22 5.15 2/569 4061562; io pressure some avg10=37.16 avg60=34.01 avg300=39.78 total=45205144295

| core | runs | failed | refused | steps p50 | pages p50/max | run ms p50/max | longest hold ms in/out | holds >10ms in/out | >100ms in/out | >1s in/out | anchor gap max s | no-group ms |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 100 | 0 | 0 | 22 | 1379/12401 | 751.9/5084.6 | 5084.7/1193.3 | 100/7033 | 100/222 | 21/1 | 50.40 | 160089.1 |
| 1 | 97 | 0 | 0 | 22 | 1355/3205 | 745.5/1877.2 | 1877.2/1193.1 | 97/7181 | 97/234 | 17/1 | 48.91 | 149019.2 |
| 2 | 102 | 0 | 0 | 23 | 1423/3075 | 781.9/3683.9 | 3683.9/1190.3 | 102/6987 | 102/209 | 21/1 | 55.61 | 154238.7 |
| 3 | 106 | 0 | 0 | 21 | 1321/3095 | 742.6/12149.6 | 12149.9/648.4 | 106/6639 | 105/144 | 25/0 | 14.63 | 178756.5 |

| core | phase sums ms (max one occurrence ms) |
|---|---|
| 0 | start 1143 (24.9); log_gate_batch 16935 (534.7); log_gate_run 5683 (622.8); page_write 685 (60.4); map_write 22 (0.0); claim_wait 0 (0.0); data_sync 43223 (697.9); end_wait 1417 (459.7); publish_writeback 7306 (655.8); publish_sync 7581 (461.9); one step max 986.3, its page writes max 60.7 |
| 1 | start 1135 (34.3); log_gate_batch 16425 (671.6); log_gate_run 5061 (403.4); page_write 656 (60.0); map_write 18 (0.0); claim_wait 0 (0.0); data_sync 36969 (552.9); end_wait 1336 (444.7); publish_writeback 5363 (545.0); publish_sync 4886 (215.5); one step max 1018.2, its page writes max 60.9 |
| 2 | start 1135 (20.6); log_gate_batch 15658 (518.1); log_gate_run 6538 (421.8); page_write 955 (142.0); map_write 20 (0.0); claim_wait 0 (0.0); data_sync 41782 (905.9); end_wait 2277 (445.7); publish_writeback 6365 (599.8); publish_sync 6540 (614.6); one step max 1174.5, its page writes max 144.1 |
| 3 | start 1183 (27.2); log_gate_batch 22491 (938.7); log_gate_run 9736 (1106.1); page_write 1922 (203.5); map_write 19 (0.0); claim_wait 0 (0.0); data_sync 52419 (1014.6); end_wait 3385 (791.5); publish_writeback 14665 (2793.0); publish_sync 11379 (1119.4); one step max 4894.9, its page writes max 505.9 |

| core | probe | n in/out | p50 in/out ms | p99 in/out ms | p99.9 in/out ms | max in/out ms | errors |
|---|---|---|---|---|---|---|---|
| 0 | stmt | 4241/25759 | 470.40/0.62 | 5048.89/397.41 | 5804.8/1133.7 | 5872.7/2541.5 | 0 |
| 0 | ping | 4241/25759 | 467.77/0.59 | 5048.61/364.74 | 5784.7/1073.8 | 5852.7/1615.9 | 0 |
| 1 | stmt | 3610/26390 | 412.99/0.62 | 1610.59/402.01 | 1829.2/1080.3 | 1872.0/1835.2 | 0 |
| 1 | ping | 3610/26390 | 410.17/0.59 | 1609.87/375.98 | 1829.0/1044.8 | 1863.9/1222.1 | 0 |
| 2 | stmt | 4067/25933 | 428.78/0.62 | 2888.31/345.93 | 3586.5/1050.1 | 3665.8/1383.0 | 0 |
| 2 | ping | 4067/25933 | 427.06/0.58 | 2886.91/328.77 | 3586.2/981.3 | 3665.7/1199.9 | 0 |
| 3 | stmt | 5933/24067 | 748.47/0.61 | 37341.66/290.37 | 38395.0/988.0 | 38486.5/1985.7 | 0 |
| 3 | ping | 5933/24067 | 745.10/0.58 | 37300.76/286.90 | 38354.6/659.0 | 38446.4/1345.2 | 0 |
