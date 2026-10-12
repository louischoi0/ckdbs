| shape | path | bin | p0 | p25 | p50 | p90 | p99 | mean | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|
| noop | literal | A | 18.5 | 25.8 | 26.2 | 29.0 | 40.9 | 27.3 | 12.2 | 15.2 |
| noop | literal | B | 18.5 | 25.8 | 26.2 | 29.3 | 41.0 | 27.3 | 12.3 | 15.3 |
| ping | literal | A | 44.4 | 49.8 | 50.5 | 59.0 | 72.3 | 52.8 | 33.5 | 29.8 |
| ping | literal | B | 42.5 | 45.7 | 46.2 | 56.3 | 69.7 | 48.9 | 24.6 | 24.9 |
| ping | prepared | A | 43.3 | 47.8 | 48.4 | 56.0 | 68.7 | 50.5 | 32.6 | 28.0 |
| ping | prepared | B | 40.9 | 43.3 | 43.9 | 50.1 | 65.3 | 46.0 | 23.5 | 22.8 |
| select-pk | literal | A | 59.5 | 63.7 | 65.5 | 78.2 | 97.9 | 70.2 | 35.7 | 42.3 |
| select-pk | literal | B | 52.6 | 54.9 | 56.5 | 68.0 | 86.1 | 60.5 | 28.1 | 32.7 |
| select-pk | prepared | A | 58.0 | 60.9 | 63.4 | 75.4 | 93.7 | 66.9 | 35.0 | 39.4 |
| select-pk | prepared | B | 49.8 | 51.9 | 53.8 | 65.5 | 83.4 | 57.7 | 28.4 | 29.9 |
| update-lit | literal | A | 47.2 | 53.0 | 53.8 | 64.4 | 82.8 | 81.4 | 31.9 | 34.3 |
| update-lit | literal | B | 37.5 | 45.2 | 45.7 | 53.9 | 71.9 | 50.0 | 24.5 | 24.9 |
| update-lit | prepared | A | 43.8 | 50.4 | 51.3 | 60.4 | 77.0 | 54.6 | 30.0 | 31.9 |
| update-lit | prepared | B | 36.2 | 41.9 | 42.4 | 51.3 | 69.8 | 45.6 | 23.5 | 22.6 |
| update-expr | literal | A | 48.9 | 55.9 | 56.6 | 67.4 | 88.7 | 60.2 | 33.6 | 34.3 |
| update-expr | literal | B | 40.0 | 47.4 | 47.9 | 57.2 | 76.5 | 66.5 | 27.1 | 24.8 |
| update-expr | prepared | A | 44.7 | 52.8 | 53.7 | 64.2 | 85.7 | 57.6 | 32.7 | 31.5 |
| update-expr | prepared | B | 37.1 | 43.7 | 44.2 | 53.6 | 75.0 | 48.5 | 25.8 | 21.9 |
| update-spill | literal | A | 54.8 | 60.8 | 62.3 | 80.6 | 114.3 | 70.0 | 41.6 | 35.1 |
| update-spill | literal | B | 37.6 | 45.0 | 45.7 | 57.4 | 84.5 | 50.8 | 24.7 | 25.2 |
| update-spill | prepared | A | 53.9 | 58.8 | 60.0 | 73.1 | 98.5 | 65.5 | 39.4 | 32.2 |
| update-spill | prepared | B | 35.7 | 41.9 | 42.6 | 52.3 | 73.6 | 46.3 | 23.5 | 22.5 |
| update-spill-assign | literal | A | 58.0 | 63.3 | 65.0 | 80.8 | 144.5 | 77.7 | 41.9 | 35.4 |
| update-spill-assign | literal | B | 44.6 | 49.7 | 50.8 | 66.0 | 115.7 | 63.8 | 29.7 | 26.2 |
| update-spill-assign | prepared | A | 54.4 | 60.9 | 62.4 | 76.0 | 99.8 | 68.0 | 41.5 | 32.8 |
| update-spill-assign | prepared | B | 41.4 | 45.5 | 46.2 | 58.1 | 78.1 | 52.3 | 27.8 | 23.5 |
| insert | literal | A | 44.1 | 50.5 | 52.4 | 78.2 | 118.2 | 60.5 | 29.7 | 33.5 |
| insert | literal | B | 37.2 | 43.3 | 44.5 | 63.1 | 102.7 | 51.5 | 23.3 | 24.8 |
| insert | prepared | A | 42.1 | 47.8 | 48.5 | 59.1 | 107.4 | 57.4 | 28.3 | 31.8 |
| insert | prepared | B | 35.3 | 40.6 | 41.1 | 50.0 | 91.7 | 51.1 | 22.1 | 22.8 |
| update-range | literal | A | 933.8 | 957.8 | 986.6 | 1195.4 | 2718.6 | 1121.8 | 1003.5 | 68.8 |
| update-range | literal | B | 699.1 | 722.2 | 763.6 | 1005.9 | 1988.9 | 887.5 | 743.7 | 59.1 |
| update-range | prepared | A | 941.5 | 1017.1 | 1074.0 | 1652.5 | 3228.7 | 1218.0 | 989.1 | 66.6 |
| update-range | prepared | B | 691.0 | 746.2 | 818.1 | 1324.4 | 2218.7 | 947.8 | 767.9 | 52.9 |
| scan | literal | A | 722.5 | 757.9 | 783.0 | 850.9 | 1138.6 | 794.0 | 731.1 | 58.4 |
| scan | literal | B | 691.0 | 719.3 | 728.9 | 785.8 | 1219.1 | 749.2 | 699.8 | 47.9 |
| scan | prepared | A | 720.4 | 754.0 | 766.3 | 812.0 | 1013.0 | 791.4 | 732.3 | 53.8 |
| scan | prepared | B | 687.9 | 713.2 | 724.5 | 763.2 | 1372.0 | 744.1 | 697.8 | 44.8 |

| shape | path | A p50 | B - A p50, median [min, max] over runs | A server CPU | B - A server CPU, median [min, max] |
|---|---|---|---|---|---|
| noop | literal | 26.2 | +0.01 [-2.81, +0.67] | 12.2 | +0.02 [-0.44, +0.22] |
| ping | literal | 50.5 | -4.44 [-11.32, -3.82] | 33.5 | -8.76 [-9.52, -7.68] |
| ping | prepared | 48.4 | -4.72 [-10.25, -3.85] | 32.6 | -9.13 [-9.35, -8.12] |
| select-pk | literal | 65.5 | -9.23 [-14.71, -8.08] | 35.7 | -7.43 [-7.97, -5.81] |
| select-pk | prepared | 63.4 | -9.44 [-12.87, -9.01] | 35.0 | -6.50 [-6.90, -5.51] |
| update-lit | literal | 53.8 | -7.88 [-9.30, -0.34] | 31.9 | -7.45 [-7.86, -5.88] |
| update-lit | prepared | 51.3 | -8.67 [-10.03, -7.90] | 30.0 | -6.61 [-8.57, -6.11] |
| update-expr | literal | 56.6 | -8.53 [-9.89, -7.47] | 33.6 | -6.38 [-7.66, -5.63] |
| update-expr | prepared | 53.7 | -9.39 [-10.01, -8.36] | 32.7 | -7.09 [-8.09, -4.36] |
| update-spill | literal | 62.3 | -16.56 [-24.81, -14.25] | 41.6 | -15.95 [-19.21, -12.61] |
| update-spill | prepared | 60.0 | -17.03 [-18.38, -14.89] | 39.4 | -15.52 [-16.57, -12.21] |
| update-spill-assign | literal | 65.0 | -14.10 [-17.07, -11.77] | 41.9 | -12.19 [-16.01, -10.08] |
| update-spill-assign | prepared | 62.4 | -15.17 [-16.60, -11.73] | 41.5 | -13.31 [-14.37, -11.50] |
| insert | literal | 52.4 | -7.24 [-8.52, -3.80] | 29.7 | -6.35 [-6.77, -5.64] |
| insert | prepared | 48.5 | -7.19 [-7.90, -5.43] | 28.3 | -6.28 [-7.18, -5.90] |
| update-range | literal | 986.6 | -227.75 [-244.19, -191.39] | 1003.5 | -220.91 [-319.55, -210.83] |
| update-range | prepared | 1074.0 | -259.96 [-401.37, -230.80] | 989.1 | -236.38 [-312.82, -169.80] |
| scan | literal | 783.0 | -37.74 [-126.87, -30.52] | 731.1 | -36.68 [-48.42, -21.65] |
| scan | prepared | 766.3 | -37.01 [-75.53, -32.44] | 732.3 | -35.01 [-52.99, -16.14] |

pages-unspilled: [{"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}]

pages-spilled: [{"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}]

end_state: [{"A": {"range_v_ok": true, "spill_txt_ok": true}, "B": {"range_v_ok": true, "spill_txt_ok": true}}, {"A": {"range_v_ok": true, "spill_txt_ok": true}, "B": {"range_v_ok": true, "spill_txt_ok": true}}, {"A": {"range_v_ok": true, "spill_txt_ok": true}, "B": {"range_v_ok": true, "spill_txt_ok": true}}, {"A": {"range_v_ok": true, "spill_txt_ok": true}, "B": {"range_v_ok": true, "spill_txt_ok": true}}, {"A": {"range_v_ok": true, "spill_txt_ok": true}, "B": {"range_v_ok": true, "spill_txt_ok": true}}, {"A": {"range_v_ok": true, "spill_txt_ok": true}, "B": {"range_v_ok": true, "spill_txt_ok": true}}, {"A": {"range_v_ok": true, "spill_txt_ok": true}, "B": {"range_v_ok": true, "spill_txt_ok": true}}, {"A": {"range_v_ok": true, "spill_txt_ok": true}, "B": {"range_v_ok": true, "spill_txt_ok": true}}]

idle_cpu_us_per_s: [{"A": 5180.46, "B": 5304.655}, {"A": 4745.651, "B": 4564.694}, {"A": 5376.089, "B": 5034.086}, {"A": 4617.113, "B": 4625.338}, {"A": 5537.519, "B": 4906.281}, {"A": 4734.515, "B": 4811.221}, {"A": 5299.515, "B": 4736.058}, {"A": 5090.89, "B": 4456.449}]

bad replies: 0
