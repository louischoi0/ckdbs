| headline (qps, median of reps) | n | base | spread | adv-off | delta | spread |
|---|---|---|---|---|---|---|
| s1 load-bars (insert) | 3 | 20123 | 1.1% | 19323 | -4.0% ~ | 28.5% (n=3) |
| s1 result-insert | 3 | 769 | 2.2% | 763 | -0.8% ~ | 3.9% (n=3) |
| s1 backtest-read (3-rel join, cold) | 3 | 1613 | 0.5% | 1542 | -4.4% ~ | 7.6% (n=3) |
| s1 backtest-replay (warm) | 3 | 1712 | 1.3% | 1654 | -3.4% | 2.4% (n=3) |
| s1 read-day-slice | 3 | 1859 | 4.4% | 1758 | -5.4% ~ | 10.0% (n=3) |
| s1 read-bar-lookup (pk) | 3 | 12150 | 3.5% | 10791 | -11.2% ~ | 30.4% (n=3) |
| s1 read-join-point (pk) | 3 | 12294 | 8.8% | 12736 | +3.6% ~ | 28.7% (n=3) |
| s1 agg-day-slice | 3 | 2109 | 2.1% | 1998 | -5.2% ~ | 8.3% (n=3) |
| s2 booking (txn, 8 bookers) | 3 | 2349 | 5.9% | 2426 | +3.3% ~ | 2.2% (n=3) |
| s2 commit (committed TPS) | 3 | 1725 | 6.2% | 1772 | +2.7% ~ | 2.3% (n=3) |
| s2 cargo-lookup | 3 | 2349 | 5.9% | 2426 | +3.2% ~ | 2.1% (n=3) |
| s2 charge-insert | 3 | 9706 | 6.1% | 9966 | +2.7% ~ | 2.4% (n=3) |
| s3 pk-user (control) | 3 | 14306 | 5.3% | 14728 | +2.9% ~ | 1.9% (n=3) |
| s3 loans-by-user | 3 | 10616 | 1.7% | 10650 | +0.3% ~ | 1.5% (n=3) |
| s3 loans-by-book | 3 | 11403 | 0.5% | 11325 | -0.7% ~ | 3.5% (n=3) |
| s3 books-by-genre | 3 | 1993 | 1.9% | 2013 | +1.0% ~ | 1.1% (n=3) |
| s3 loans-by-daterange | 3 | 609 | 2.5% | 621 | +2.0% ~ | 1.2% (n=3) |
| s3 overdue | 3 | 991 | 2.3% | 995 | +0.3% ~ | 1.3% (n=3) |
| s3 join-loan-user | 3 | 9606 | 21.4% | 10070 | +4.8% ~ | 1.1% (n=3) |
| s3 exists-correlated | 3 | 6345 | 9.3% | 6545 | +3.1% ~ | 1.5% (n=3) |
| s3 count-by-user | 3 | 13532 | 26.6% | 14749 | +9.0% ~ | 1.3% (n=3) |
| s4 board [off arm] | 2 | 1968 | 0.9% | 1947 | -1.1% | 0.5% (n=2) |
| s4 tape10k [off arm] | 2 | 2234 | 2.0% | 2210 | -1.1% ~ | 0.2% (n=2) |
| s4 pk [off arm] | 2 | 13614 | 16.0% | 13865 | +1.8% ~ | 0.8% (n=2) |
| s4 open (insert) [off arm] | 2 | 847 | 2.0% | 826 | -2.5% ~ | 7.4% (n=2) |
| s4 board [on arm] | 2 | 3232 | 0.1% | 3278 | +1.4% | 1.2% (n=2) |
| s4 tape10k [on arm] | 2 | 4699 | 0.3% | 4842 | +3.0% | 0.3% (n=2) |
| s4 pk [on arm] | 2 | 13407 | 0.0% | 15130 | +12.9% | 3.9% (n=2) |
| s4 open (insert) [on arm] | 2 | 796 | 1.0% | 791 | -0.6% ~ | 3.1% (n=2) |
| s4 board [declared arm] | 2 | 3388 | 0.4% | 3459 | +2.1% | 0.7% (n=2) |
| s4 tape10k [declared arm] | 2 | 4566 | 0.1% | 4710 | +3.2% | 0.0% (n=2) |
| s4 pk [declared arm] | 2 | 13648 | 6.3% | 14960 | +9.6% | 0.6% (n=2) |
| s4 open (insert) [declared arm] | 2 | 822 | 0.3% | 847 | +3.0% ~ | 3.9% (n=2) |

`~` = |delta| <= the larger spread of the two arms (inside the noise floor).
