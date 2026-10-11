| shape | path | bin | p0 | p25 | p50 | p90 | p99 | mean | server CPU | driver CPU |
|---|---|---|---|---|---|---|---|---|---|---|
| noop | literal | A | 17.8 | 25.9 | 26.2 | 28.8 | 48.4 | 31.6 | 12.1 | 15.0 |
| noop | literal | B | 18.0 | 25.8 | 26.1 | 29.1 | 49.8 | 30.0 | 12.0 | 15.0 |
| select-pk | literal | A | 59.2 | 63.2 | 65.2 | 76.2 | 100.9 | 70.2 | 35.3 | 41.4 |
| select-pk | literal | B | 52.2 | 54.7 | 56.8 | 70.0 | 119.5 | 64.7 | 28.6 | 32.2 |
| select-pk | prepared | A | 54.4 | 60.5 | 62.2 | 75.6 | 148.0 | 78.5 | 34.9 | 38.9 |
| select-pk | prepared | B | 46.4 | 51.5 | 52.7 | 61.5 | 81.1 | 57.6 | 26.8 | 29.4 |
| delete-pk | literal | A | 42.4 | 49.2 | 50.1 | 57.3 | 77.2 | 59.2 | 28.5 | 32.9 |
| delete-pk | literal | B | 34.9 | 41.8 | 42.3 | 50.0 | 66.1 | 44.7 | 21.4 | 23.8 |
| delete-pk | prepared | A | 40.3 | 46.2 | 46.8 | 54.8 | 75.1 | 49.6 | 27.1 | 30.6 |
| delete-pk | prepared | B | 32.4 | 39.2 | 39.6 | 45.5 | 66.0 | 43.2 | 20.8 | 21.6 |

| shape | path | A p50 | B - A p50, median [min, max] over runs | A server CPU | B - A server CPU, median [min, max] |
|---|---|---|---|---|---|
| noop | literal | 26.2 | -0.12 [-0.71, -0.03] | 12.1 | -0.02 [-0.08, +0.08] |
| select-pk | literal | 65.2 | -8.05 [-9.97, -6.91] | 35.3 | -6.52 [-8.11, -5.49] |
| select-pk | prepared | 62.2 | -9.53 [-9.91, -7.91] | 34.9 | -7.22 [-8.50, -6.71] |
| delete-pk | literal | 50.1 | -7.61 [-8.03, -4.35] | 28.5 | -6.74 [-7.54, -5.71] |
| delete-pk | prepared | 46.8 | -7.21 [-7.53, -4.43] | 27.1 | -6.28 [-8.12, -5.29] |

pages-unspilled: [{"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 0}, "B": {"updates": 1000, "pages_allocated_delta": 0}}]

pages-spilled: [{"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}, {"A": {"updates": 1000, "pages_allocated_delta": 38}, "B": {"updates": 1000, "pages_allocated_delta": 0}}]

end_state: [{"A": {"range_v_ok": false, "spill_txt_ok": true}, "B": {"range_v_ok": false, "spill_txt_ok": true}}, {"A": {"range_v_ok": false, "spill_txt_ok": true}, "B": {"range_v_ok": false, "spill_txt_ok": true}}, {"A": {"range_v_ok": false, "spill_txt_ok": true}, "B": {"range_v_ok": false, "spill_txt_ok": true}}, {"A": {"range_v_ok": false, "spill_txt_ok": true}, "B": {"range_v_ok": false, "spill_txt_ok": true}}, {"A": {"range_v_ok": false, "spill_txt_ok": true}, "B": {"range_v_ok": false, "spill_txt_ok": true}}, {"A": {"range_v_ok": false, "spill_txt_ok": true}, "B": {"range_v_ok": false, "spill_txt_ok": true}}]

idle_cpu_us_per_s: [{"A": 4771.904, "B": 4415.753}, {"A": 4448.118, "B": 5188.625}, {"A": 4756.536, "B": 4971.885}, {"A": 5039.967, "B": 4584.482}, {"A": 5610.063, "B": 5600.677}, {"A": 5218.171, "B": 5352.661}]

bad replies: 0
