| cell | runs | A | A' | B | B - A cost, paired median | B slower than A | A' - A typical / largest abs gap | verdict |
|---|---|---|---|---|---|---|---|---|
| INSERT, no dropped column (positional, relation `i`), literal | 18 | 40,790 | 40,742 | 41,124 | -0.24 us (-1.0 %, B faster) | 5 of 18 | 0.49 / 1.02 us | inside |
| INSERT, no dropped column (positional, relation `i`), prepared | 18 | 41,793 | 41,707 | 42,097 | -0.21 us (-0.9 %, B faster) | 6 of 18 | 0.48 / 1.06 us | inside |
| INSERT, two dropped slot columns, not cleaned (column list), literal | 18 | 37,180 | 37,195 | 37,546 | -0.17 us (-0.6 %, B faster) | 6 of 18 | 0.44 / 1.49 us | inside |
| INSERT, two dropped slot columns, not cleaned (column list), prepared | 18 | 38,663 | 38,444 | 38,710 | -0.32 us (-1.3 %, B faster) | 5 of 18 | 0.68 / 1.40 us | inside |
| INSERT, SPARE 2, no ALTER (column list), literal | 6 | 37,272 | 37,751 | 37,735 | -0.03 us (-0.1 %, B faster) | 3 of 6 | 0.68 / 1.25 us | inside |
| INSERT, SPARE 2, no ALTER (column list), prepared | 6 | 38,753 | 38,894 | 38,778 | -0.08 us (-0.3 %, B faster) | 3 of 6 | 0.50 / 1.64 us | inside |
| INSERT, no SPARE (column list), literal | 6 | 37,508 | 37,593 | 37,824 | +0.01 us (+0.0 %, B slower) | 3 of 6 | 0.74 / 1.11 us | inside |
| INSERT, no SPARE (column list), prepared | 6 | 38,819 | 39,238 | 38,786 | +0.07 us (+0.3 %, B slower) | 3 of 6 | 0.71 / 1.68 us | inside |
| point SELECT by key, 1,000-row table, literal | 6 | 29,836 | 30,212 | 30,216 | -0.37 us (-1.1 %, B faster) | 2 of 6 | 0.40 / 1.90 us | inside |
| point SELECT by key, 1,000-row table, prepared | 6 | 33,346 | 34,002 | 34,571 | -0.38 us (-1.3 %, B faster) | 1 of 6 | 0.68 / 1.68 us | inside |
| every-row UPDATE of 1,000 rows, literal | 18 | 1,218 | 1,266 | 1,226 | -11.62 us (-1.4 %, B faster) | 6 of 18 | 27.55 / 124.61 us | inside |
| every-row UPDATE of 1,000 rows, prepared | 18 | 1,232 | 1,205 | 1,248 | -15.01 us (-1.8 %, B faster) | 6 of 18 | 28.31 / 273.74 us | inside |
| scan, net per row, 10,000 rows (`bn`), literal | 7 | 38,675,499 | 38,890,173 | 39,078,316 | +0.10 ns (+0.4 %, B slower) | 4 of 7 | 0.12 / 1.32 ns | inside |
| scan, net per row, 10,000 rows (`bn`), prepared | 7 | 38,440,016 | 38,803,543 | 38,962,730 | -0.22 ns (-0.8 %, B faster) | 3 of 7 | 0.23 / 1.80 ns | inside |
| scan, net per row, 200 rows, literal | 2 | 33,736,808 | 32,953,954 | 34,602,741 | -0.74 ns (-2.5 %, B faster) | 1 of 2 | 0.70 / 0.73 ns | **OUTSIDE** |
| scan, net per row, 200 rows, prepared | 2 | 34,356,323 | 34,424,398 | 33,618,769 | +0.64 ns (+2.2 %, B slower) | 1 of 2 | 3.21 / 3.27 ns | inside |
| scan, net per row, 1,000 rows, literal | 2 | 37,948,481 | 40,856,109 | 39,583,841 | -1.09 ns (-4.1 %, B faster) | 1 of 2 | 1.88 / 3.68 ns | inside |
| scan, net per row, 1,000 rows, prepared | 2 | 41,214,081 | 41,377,925 | 41,715,129 | -0.29 ns (-1.2 %, B faster) | 1 of 2 | 1.04 / 1.14 ns | inside |
