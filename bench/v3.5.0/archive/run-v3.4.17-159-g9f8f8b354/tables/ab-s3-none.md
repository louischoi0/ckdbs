| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s3 ddl (10,000 loans, unindexed) | 2,605.5 qps | 74.6 % | 3,342.2 qps | 78.2 % | 1.28x | spread > 20 %, no claim |
| s3 load (10,000 loans, unindexed) | 5,168.0 qps | 71.0 % | 6,101.3 qps | 3.6 % | 1.18x | spread > 20 %, no claim |
| s3 pk-user (10,000 loans, unindexed) | 22,675.7 qps | 19.3 % | 22,624.4 qps | 61.4 % | 1.00x | spread > 20 %, no claim |
| s3 loans-by-user (10,000 loans, unindexed) | 5,750.4 qps | 4.9 % | 5,405.4 qps | 28.7 % | 0.94x | spread > 20 %, no claim |
| s3 loans-by-book (10,000 loans, unindexed) | 5,717.6 qps | 3.9 % | 5,515.7 qps | 28.3 % | 0.96x | spread > 20 %, no claim |
| s3 resv-by-user (10,000 loans, unindexed) | 9,765.6 qps | 7.3 % | 9,049.8 qps | 34.0 % | 0.93x | spread > 20 %, no claim |
| s3 books-by-author (10,000 loans, unindexed) | 14,471.8 qps | 10.7 % | 13,262.6 qps | 38.0 % | 0.92x | spread > 20 %, no claim |
| s3 books-by-genre (10,000 loans, unindexed) | 5,512.7 qps | 4.0 % | 5,285.4 qps | 22.5 % | 0.96x | spread > 20 %, no claim |
| s3 loans-by-daterange (10,000 loans, unindexed) | 1,629.7 qps | 0.3 % | 1,609.8 qps | 20.4 % | 0.99x | spread > 20 %, no claim |
| s3 overdue (10,000 loans, unindexed) | 2,067.4 qps | 1.3 % | 2,018.6 qps | 20.4 % | 0.98x | spread > 20 %, no claim |
| s3 join-loan-user (10,000 loans, unindexed) | 5,694.8 qps | 5.8 % | 5,316.3 qps | 27.1 % | 0.93x | spread > 20 %, no claim |
| s3 join-no-literal (10,000 loans, unindexed) | 3,521.1 qps | 3.7 % | 3,388.7 qps | 22.4 % | 0.96x | spread > 20 %, no claim |
| s3 exists-correlated (10,000 loans, unindexed) | 3,027.6 qps | 3.3 % | 2,934.3 qps | 39.9 % | 0.97x | spread > 20 %, no claim |
| s3 count-by-user (10,000 loans, unindexed) | 6,257.8 qps | 4.6 % | 5,773.7 qps | 31.3 % | 0.92x | spread > 20 %, no claim |
