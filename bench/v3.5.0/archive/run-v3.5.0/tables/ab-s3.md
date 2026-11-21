| measure | v3.4.14 engine | spread | v3.5.0 engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s3 pk-user (10,000 loans, unindexed) | 15,105.7 qps | 27.0 % | 14,836.8 qps | 35.7 % | 0.98x | spread > 20 %, no claim |
| s3 loans-by-user (10,000 loans, unindexed) | 2,884.3 qps | 8.4 % | 2,908.7 qps | 13.0 % | 1.01x | inside the spread |
| s3 loans-by-book (10,000 loans, unindexed) | 2,963.0 qps | 12.4 % | 2,973.5 qps | 9.8 % | 1.00x | inside the spread |
| s3 resv-by-user (10,000 loans, unindexed) | 4,299.2 qps | 39.0 % | 5,274.3 qps | 21.4 % | 1.23x | spread > 20 %, no claim |
| s3 books-by-author (10,000 loans, unindexed) | 7,429.4 qps | 27.8 % | 8,084.1 qps | 28.8 % | 1.09x | spread > 20 %, no claim |
| s3 books-by-genre (10,000 loans, unindexed) | 2,767.0 qps | 9.7 % | 2,657.5 qps | 3.5 % | 0.96x | inside the spread |
| s3 loans-by-daterange (10,000 loans, unindexed) | 892.4 qps | 3.6 % | 893.6 qps | 0.6 % | 1.00x | inside the spread |
| s3 overdue (10,000 loans, unindexed) | 1,089.8 qps | 5.6 % | 1,111.2 qps | 0.4 % | 1.02x | inside the spread |
| s3 join-loan-user (10,000 loans, unindexed) | 2,853.1 qps | 11.4 % | 2,773.9 qps | 6.2 % | 0.97x | inside the spread |
| s3 join-no-literal (10,000 loans, unindexed) | 1,968.5 qps | 0.6 % | 1,904.8 qps | 10.6 % | 0.97x | inside the spread |
| s3 exists-correlated (10,000 loans, unindexed) | 1,511.3 qps | 1.9 % | 1,563.7 qps | 5.5 % | 1.03x | inside the spread |
| s3 count-by-user (10,000 loans, unindexed) | 3,207.2 qps | 3.4 % | 3,416.5 qps | 4.7 % | 1.07x | **finding** |
