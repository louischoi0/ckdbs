
#### group column `int`

| rows | statement | A p50 | B p50 | B−A p50 (median of 7 runs) | IQR of B−A | runs B>A | A QPS | B QPS | engine B−A per stmt |
|---|---|---|---|---|---|---|---|---|---|
| 200 | INSERT, asserted, existing group | 51.5 µs | 52.0 µs | +0.00 µs | -0.35 to +0.85 µs | 3/7 | 19,417 qps | 19,231 qps | +0.72 µs |
| 200 | INSERT, asserted, new group | 52.3 µs | 52.6 µs | +0.10 µs | -0.30 to +0.70 µs | 4/7 | 19,120 qps | 19,011 qps | +0.09 µs |
| 200 | UPDATE g, asserted | 52.2 µs | 53.1 µs | +0.50 µs | -0.00 to +1.00 µs | 5/7 | 19,157 qps | 18,832 qps | +0.44 µs |
| 200 | DELETE, asserted | 50.8 µs | 50.9 µs | -0.40 µs | -0.55 to +0.35 µs | 3/7 | 19,685 qps | 19,646 qps | -0.01 µs |
| 200 | ctl: INSERT, no assertion | 48.9 µs | 49.0 µs | +0.20 µs | -0.10 to +0.20 µs | 4/7 | 20,450 qps | 20,408 qps | -0.14 µs |
| 200 | ctl: INSERT repeated (noise floor) | 48.9 µs | 49.0 µs | +0.20 µs | +0.05 to +0.40 µs | 5/7 | 20,450 qps | 20,408 qps | -0.16 µs |
| 200 | ctl: UPDATE g, no assertion | 51.5 µs | 51.7 µs | +0.20 µs | -0.25 to +0.55 µs | 4/7 | 19,417 qps | 19,342 qps | +0.18 µs |
| 200 | ctl: UPDATE repeated (noise floor) | 51.4 µs | 51.7 µs | -0.20 µs | -0.45 to +0.45 µs | 3/7 | 19,455 qps | 19,342 qps | +0.16 µs |
| 200 | ctl: DELETE, no assertion | 48.6 µs | 48.6 µs | +0.00 µs | -0.25 to +0.05 µs | 2/7 | 20,576 qps | 20,576 qps | -0.05 µs |
| 200 | ctl: pk SELECT | 63.1 µs | 63.0 µs | -0.30 µs | -0.60 to +0.05 µs | 2/7 | 15,848 qps | 15,873 qps | -0.06 µs |
| 200 | ctl: SHOW META | 50.0 µs | 50.4 µs | +0.40 µs | +0.00 to +0.50 µs | 4/7 | 20,000 qps | 19,841 qps | +0.14 µs |
| 1,000 | INSERT, asserted, existing group | 51.8 µs | 52.1 µs | +0.10 µs | -0.25 to +0.40 µs | 4/7 | 19,305 qps | 19,194 qps | +0.06 µs |
| 1,000 | INSERT, asserted, new group | 52.4 µs | 52.6 µs | +0.30 µs | -0.30 to +0.35 µs | 4/7 | 19,084 qps | 19,011 qps | +0.10 µs |
| 1,000 | UPDATE g, asserted | 52.1 µs | 53.1 µs | +0.60 µs | +0.50 to +0.80 µs | 6/7 | 19,194 qps | 18,832 qps | +0.44 µs |
| 1,000 | DELETE, asserted | 51.0 µs | 51.1 µs | +0.50 µs | +0.00 to +0.70 µs | 5/7 | 19,608 qps | 19,569 qps | +0.42 µs |
| 1,000 | ctl: INSERT, no assertion | 49.0 µs | 49.0 µs | +0.00 µs | -0.15 to +0.15 µs | 3/7 | 20,408 qps | 20,408 qps | +0.10 µs |
| 1,000 | ctl: INSERT repeated (noise floor) | 49.0 µs | 49.0 µs | -0.10 µs | -0.15 to +0.05 µs | 2/7 | 20,408 qps | 20,408 qps | -0.05 µs |
| 1,000 | ctl: UPDATE g, no assertion | 51.2 µs | 51.5 µs | +0.20 µs | -0.05 to +0.25 µs | 4/7 | 19,531 qps | 19,417 qps | +0.13 µs |
| 1,000 | ctl: UPDATE repeated (noise floor) | 51.2 µs | 51.3 µs | +0.20 µs | -0.10 to +0.35 µs | 4/7 | 19,531 qps | 19,493 qps | +0.27 µs |
| 1,000 | ctl: DELETE, no assertion | 48.7 µs | 48.7 µs | +0.00 µs | -0.10 to +0.10 µs | 3/7 | 20,534 qps | 20,534 qps | -0.01 µs |
| 1,000 | ctl: pk SELECT | 63.6 µs | 63.9 µs | +0.40 µs | +0.25 to +1.20 µs | 6/7 | 15,723 qps | 15,649 qps | +0.37 µs |
| 1,000 | ctl: SHOW META | 50.2 µs | 50.2 µs | +0.00 µs | -0.15 to +0.15 µs | 3/7 | 19,920 qps | 19,920 qps | -0.01 µs |
| 10,000 | INSERT, asserted, existing group | 52.9 µs | 52.9 µs | +0.30 µs | -0.20 to +0.35 µs | 4/7 | 18,904 qps | 18,904 qps | +0.16 µs |
| 10,000 | INSERT, asserted, new group | 52.6 µs | 52.8 µs | +0.50 µs | +0.05 to +1.30 µs | 5/7 | 19,011 qps | 18,939 qps | +0.11 µs |
| 10,000 | UPDATE g, asserted | 56.0 µs | 56.1 µs | -0.40 µs | -0.65 to +0.15 µs | 2/7 | 17,857 qps | 17,825 qps | +0.18 µs |
| 10,000 | DELETE, asserted | 51.5 µs | 51.3 µs | -0.30 µs | -0.45 to +0.70 µs | 2/7 | 19,417 qps | 19,493 qps | -0.29 µs |
| 10,000 | ctl: INSERT, no assertion | 49.1 µs | 49.0 µs | +0.10 µs | -0.20 to +0.25 µs | 4/7 | 20,367 qps | 20,408 qps | +0.10 µs |
| 10,000 | ctl: INSERT repeated (noise floor) | 48.9 µs | 49.0 µs | +0.10 µs | -0.10 to +0.20 µs | 4/7 | 20,450 qps | 20,408 qps | +0.17 µs |
| 10,000 | ctl: UPDATE g, no assertion | 51.7 µs | 51.6 µs | -0.10 µs | -0.40 to +0.75 µs | 2/7 | 19,342 qps | 19,380 qps | -0.02 µs |
| 10,000 | ctl: UPDATE repeated (noise floor) | 51.7 µs | 51.4 µs | -0.20 µs | -0.45 to +0.70 µs | 2/7 | 19,342 qps | 19,455 qps | -0.03 µs |
| 10,000 | ctl: DELETE, no assertion | 48.7 µs | 48.9 µs | +0.20 µs | -0.05 to +0.20 µs | 4/7 | 20,534 qps | 20,450 qps | +0.14 µs |
| 10,000 | ctl: pk SELECT | 63.1 µs | 63.0 µs | -0.10 µs | -0.75 to +0.15 µs | 2/7 | 15,848 qps | 15,873 qps | -0.35 µs |
| 10,000 | ctl: SHOW META | 50.1 µs | 50.2 µs | +0.10 µs | -0.10 to +0.20 µs | 4/7 | 19,960 qps | 19,920 qps | +0.04 µs |

#### group column `date`

| rows | statement | A p50 | B p50 | B−A p50 (median of 7 runs) | IQR of B−A | runs B>A | A QPS | B QPS | engine B−A per stmt |
|---|---|---|---|---|---|---|---|---|---|
| 200 | INSERT, asserted, existing group | 52.7 µs | 52.7 µs | -0.30 µs | -0.55 to +0.55 µs | 3/7 | 18,975 qps | 18,975 qps | +0.03 µs |
| 200 | INSERT, asserted, new group | 53.4 µs | 53.6 µs | +0.00 µs | -0.40 to +0.50 µs | 3/7 | 18,727 qps | 18,657 qps | -0.02 µs |
| 200 | UPDATE g, asserted | 57.2 µs | 53.6 µs | -3.80 µs | -4.35 to -3.60 µs | 0/7 | 17,483 qps | 18,657 qps | -3.30 µs |
| 200 | DELETE, asserted | 51.5 µs | 50.9 µs | +0.00 µs | -0.70 to +0.25 µs | 3/7 | 19,417 qps | 19,646 qps | +0.11 µs |
| 200 | ctl: INSERT, no assertion | 49.7 µs | 49.8 µs | -0.20 µs | -0.40 to +0.20 µs | 2/7 | 20,121 qps | 20,080 qps | +0.04 µs |
| 200 | ctl: INSERT repeated (noise floor) | 49.8 µs | 49.8 µs | +0.00 µs | -0.20 to +0.35 µs | 3/7 | 20,080 qps | 20,080 qps | +0.07 µs |
| 200 | ctl: UPDATE g, no assertion | 52.6 µs | 52.2 µs | -0.50 µs | -0.80 to -0.15 µs | 2/7 | 19,011 qps | 19,157 qps | -0.10 µs |
| 200 | ctl: UPDATE repeated (noise floor) | 52.7 µs | 52.4 µs | -0.40 µs | -0.75 to -0.15 µs | 2/7 | 18,975 qps | 19,084 qps | -0.00 µs |
| 200 | ctl: DELETE, no assertion | 48.9 µs | 48.8 µs | -0.20 µs | -0.30 to +0.15 µs | 3/7 | 20,450 qps | 20,492 qps | -0.09 µs |
| 200 | ctl: pk SELECT | 64.8 µs | 64.7 µs | -0.20 µs | -0.80 to +0.10 µs | 2/7 | 15,432 qps | 15,456 qps | -0.42 µs |
| 200 | ctl: SHOW META | 50.3 µs | 50.6 µs | +0.40 µs | +0.05 to +0.40 µs | 5/7 | 19,881 qps | 19,763 qps | +0.20 µs |
| 1,000 | INSERT, asserted, existing group | 53.1 µs | 52.4 µs | -0.40 µs | -1.10 to +0.00 µs | 2/7 | 18,832 qps | 19,084 qps | -0.54 µs |
| 1,000 | INSERT, asserted, new group | 53.6 µs | 53.1 µs | -0.50 µs | -0.90 to +0.20 µs | 2/7 | 18,657 qps | 18,832 qps | -0.33 µs |
| 1,000 | UPDATE g, asserted | 57.3 µs | 53.5 µs | -3.90 µs | -4.15 to -3.35 µs | 0/7 | 17,452 qps | 18,692 qps | -3.03 µs |
| 1,000 | DELETE, asserted | 51.3 µs | 51.3 µs | +0.20 µs | -0.20 to +0.45 µs | 5/7 | 19,493 qps | 19,493 qps | +0.11 µs |
| 1,000 | ctl: INSERT, no assertion | 49.5 µs | 49.5 µs | +0.10 µs | -0.10 to +0.10 µs | 4/7 | 20,202 qps | 20,202 qps | -0.04 µs |
| 1,000 | ctl: INSERT repeated (noise floor) | 49.6 µs | 49.7 µs | +0.20 µs | -0.15 to +0.25 µs | 4/7 | 20,161 qps | 20,121 qps | -0.19 µs |
| 1,000 | ctl: UPDATE g, no assertion | 52.2 µs | 51.9 µs | -0.20 µs | -0.65 to +0.15 µs | 3/7 | 19,157 qps | 19,268 qps | -0.14 µs |
| 1,000 | ctl: UPDATE repeated (noise floor) | 52.1 µs | 52.0 µs | -0.20 µs | -0.65 to +0.25 µs | 3/7 | 19,194 qps | 19,231 qps | -0.12 µs |
| 1,000 | ctl: DELETE, no assertion | 48.7 µs | 48.8 µs | +0.10 µs | +0.00 to +0.15 µs | 4/7 | 20,534 qps | 20,492 qps | -0.06 µs |
| 1,000 | ctl: pk SELECT | 65.4 µs | 65.2 µs | -0.70 µs | -0.70 to +0.00 µs | 2/7 | 15,291 qps | 15,337 qps | -0.39 µs |
| 1,000 | ctl: SHOW META | 50.1 µs | 50.2 µs | +0.30 µs | +0.05 to +0.50 µs | 5/7 | 19,960 qps | 19,920 qps | +0.00 µs |
| 10,000 | INSERT, asserted, existing group | 53.7 µs | 53.0 µs | -0.30 µs | -0.70 to -0.20 µs | 0/7 | 18,622 qps | 18,868 qps | -0.13 µs |
| 10,000 | INSERT, asserted, new group | 53.8 µs | 53.1 µs | -0.40 µs | -0.70 to -0.20 µs | 1/7 | 18,587 qps | 18,832 qps | -0.45 µs |
| 10,000 | UPDATE g, asserted | 57.7 µs | 56.9 µs | -0.80 µs | -1.25 to -0.55 µs | 0/7 | 17,331 qps | 17,575 qps | -0.75 µs |
| 10,000 | DELETE, asserted | 51.6 µs | 51.4 µs | -0.50 µs | -0.70 to +0.00 µs | 0/7 | 19,380 qps | 19,455 qps | -0.29 µs |
| 10,000 | ctl: INSERT, no assertion | 49.7 µs | 49.7 µs | +0.00 µs | -0.10 to +0.05 µs | 2/7 | 20,121 qps | 20,121 qps | -0.11 µs |
| 10,000 | ctl: INSERT repeated (noise floor) | 49.7 µs | 49.6 µs | -0.20 µs | -0.30 to +0.10 µs | 2/7 | 20,121 qps | 20,161 qps | -0.06 µs |
| 10,000 | ctl: UPDATE g, no assertion | 52.5 µs | 51.9 µs | -0.70 µs | -1.15 to -0.25 µs | 0/7 | 19,048 qps | 19,268 qps | -0.51 µs |
| 10,000 | ctl: UPDATE repeated (noise floor) | 52.6 µs | 51.8 µs | -0.70 µs | -1.05 to -0.30 µs | 0/7 | 19,011 qps | 19,305 qps | -0.49 µs |
| 10,000 | ctl: DELETE, no assertion | 48.8 µs | 48.7 µs | -0.10 µs | -0.20 to +0.05 µs | 2/7 | 20,492 qps | 20,534 qps | -0.08 µs |
| 10,000 | ctl: pk SELECT | 64.2 µs | 64.2 µs | +0.10 µs | -0.10 to +0.55 µs | 4/7 | 15,576 qps | 15,576 qps | +0.22 µs |
| 10,000 | ctl: SHOW META | 49.9 µs | 50.0 µs | -0.10 µs | -0.25 to +0.20 µs | 3/7 | 20,040 qps | 20,000 qps | +0.01 µs |
