| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s3 ddl (10,000 loans, unindexed) | 1,388.9 qps | 75.9 % | 1,477.3 qps | 51.3 % | 1.06x | spread > 20 %, no claim |
| s3 load (10,000 loans, unindexed) | 2,890.2 qps | 167.9 % | 6,775.1 qps | 38.5 % | 2.34x | spread > 20 %, no claim |
| s3 pk-user (10,000 loans, unindexed) | 30,303.0 qps | 15.4 % | 30,864.2 qps | 4.2 % | 1.02x | inside the spread |
| s3 loans-by-user (10,000 loans, unindexed) | 6,887.1 qps | 5.5 % | 6,839.9 qps | 1.0 % | 0.99x | inside the spread |
| s3 loans-by-book (10,000 loans, unindexed) | 7,032.3 qps | 9.8 % | 6,839.9 qps | 6.9 % | 0.97x | inside the spread |
| s3 resv-by-user (10,000 loans, unindexed) | 11,723.3 qps | 9.8 % | 11,560.7 qps | 3.1 % | 0.99x | inside the spread |
| s3 books-by-author (10,000 loans, unindexed) | 17,064.8 qps | 9.5 % | 17,241.4 qps | 11.9 % | 1.01x | inside the spread |
| s3 books-by-genre (10,000 loans, unindexed) | 5,948.8 qps | 6.4 % | 5,945.3 qps | 11.0 % | 1.00x | inside the spread |
| s3 loans-by-daterange (10,000 loans, unindexed) | 1,476.2 qps | 5.5 % | 1,521.4 qps | 3.4 % | 1.03x | inside the spread |
| s3 overdue (10,000 loans, unindexed) | 1,944.0 qps | 1.8 % | 1,963.9 qps | 3.1 % | 1.01x | inside the spread |
| s3 join-loan-user (10,000 loans, unindexed) | 6,775.1 qps | 1.8 % | 6,702.4 qps | 4.0 % | 0.99x | inside the spread |
| s3 join-no-literal (10,000 loans, unindexed) | 4,147.7 qps | 2.9 % | 4,116.9 qps | 5.4 % | 0.99x | inside the spread |
| s3 exists-correlated (10,000 loans, unindexed) | 4,095.0 qps | 2.1 % | 4,122.0 qps | 4.6 % | 1.01x | inside the spread |
| s3 count-by-user (10,000 loans, unindexed) | 7,501.9 qps | 0.6 % | 7,418.4 qps | 1.0 % | 0.99x | **finding** |
