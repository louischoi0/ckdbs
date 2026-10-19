| durability | rows | rep | dirty pass: rows/s | dirty wall | server CPU | roll hold / rolls | longest non-yielding run | clean re-run wall | log bytes of the re-run |
|---|---|---|---|---|---|---|---|---|---|
| relaxed | 10,000 | 0 | 1,615,102 | 6.2 ms | 6 ms | 0 ms / 0 | 3 ms | 0.41 ms | 64 |
| relaxed | 10,000 | 1 | 1,664,977 | 6.0 ms | 6 ms | 0 ms / 0 | 2 ms | 0.41 ms | 64 |
| relaxed | 100,000 | 0 | 1,561,489 | 64.0 ms | 63 ms | 0 ms / 0 | 0 ms | 2.88 ms | 64 |
| relaxed | 100,000 | 1 | 1,643,689 | 60.8 ms | 61 ms | 0 ms / 0 | 0 ms | 2.94 ms | 64 |
| relaxed | 1,000,000 | 0 | 768,241 | 1301.7 ms | 693 ms | 626 ms / 2 | 911 ms | 27.30 ms | 64 |
| relaxed | 1,000,000 | 1 | 768,858 | 1300.6 ms | 666 ms | 642 ms / 2 | 916 ms | 27.51 ms | 64 |
| strict | 10,000 | 0 | 888,047 | 11.3 ms | 6 ms | 0 ms / 0 | 3 ms | 2.73 ms | 64 |
| strict | 10,000 | 1 | 718,956 | 13.9 ms | 9 ms | 0 ms / 0 | 8 ms | 2.53 ms | 64 |
| strict | 100,000 | 0 | 1,330,535 | 75.2 ms | 63 ms | 0 ms / 0 | 0 ms | 6.00 ms | 64 |
| strict | 100,000 | 1 | 1,331,919 | 75.1 ms | 62 ms | 0 ms / 0 | 0 ms | 6.70 ms | 64 |
| strict | 1,000,000 | 0 | 492,178 | 2031.8 ms | 658 ms | 762 ms / 2 | 1584 ms | 31.04 ms | 64 |
| strict | 1,000,000 | 1 | 491,679 | 2033.8 ms | 661 ms | 763 ms / 2 | 1625 ms | 128.88 ms | 64 |
| group | 10,000 | 0 | 915,681 | 10.9 ms | 6 ms | 0 ms / 0 | 2 ms | 2.23 ms | 64 |
| group | 10,000 | 1 | 867,943 | 11.5 ms | 6 ms | 0 ms / 0 | 0 ms | 59.03 ms | 64 |
| group | 100,000 | 0 | 1,331,593 | 75.1 ms | 62 ms | 0 ms / 0 | 0 ms | 4.46 ms | 64 |
| group | 100,000 | 1 | 1,357,219 | 73.7 ms | 62 ms | 0 ms / 0 | 0 ms | 4.81 ms | 64 |
| group | 1,000,000 | 0 | 448,390 | 2230.2 ms | 656 ms | 922 ms / 2 | 1861 ms | 29.13 ms | 64 |
| group | 1,000,000 | 1 | 493,345 | 2027.0 ms | 654 ms | 760 ms / 2 | 1639 ms | 29.51 ms | 64 |
