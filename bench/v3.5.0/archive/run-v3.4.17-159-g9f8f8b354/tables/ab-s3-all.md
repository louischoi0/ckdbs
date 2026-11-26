| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s3 ddl (10,000 loans, indexed) | 1,518.1 qps | 14.3 % | 2,045.4 qps | 64.7 % | 1.35x | spread > 20 %, no claim |
| s3 load (10,000 loans, indexed) | 5,005.0 qps | 39.2 % | 2,856.3 qps | 121.0 % | 0.57x | spread > 20 %, no claim |
| s3 create-index (10,000 loans, indexed) | 181.4 qps | 82.5 % | 166.1 qps | 49.3 % | 0.92x | spread > 20 %, no claim |
| s3 pk-user (10,000 loans, indexed) | 36,764.7 qps | 38.4 % | 22,779.0 qps | 5.3 % | 0.62x | spread > 20 %, no claim |
| s3 loans-by-user (10,000 loans, indexed) | 27,472.5 qps | 34.0 % | 17,985.6 qps | 16.0 % | 0.65x | spread > 20 %, no claim |
| s3 loans-by-book (10,000 loans, indexed) | 29,585.8 qps | 35.9 % | 19,379.8 qps | 16.9 % | 0.66x | spread > 20 %, no claim |
| s3 resv-by-user (10,000 loans, indexed) | 33,112.6 qps | 36.9 % | 21,052.6 qps | 17.3 % | 0.64x | spread > 20 %, no claim |
| s3 books-by-author (10,000 loans, indexed) | 30,303.0 qps | 35.3 % | 19,762.8 qps | 17.0 % | 0.65x | spread > 20 %, no claim |
| s3 books-by-genre (10,000 loans, indexed) | 6,410.3 qps | 21.2 % | 5,076.1 qps | 4.0 % | 0.79x | spread > 20 %, no claim |
| s3 loans-by-daterange (10,000 loans, indexed) | 2,014.1 qps | 19.2 % | 1,608.8 qps | 5.1 % | 0.80x | **finding** |
| s3 overdue (10,000 loans, indexed) | 3,151.6 qps | 20.2 % | 2,509.4 qps | 3.5 % | 0.80x | spread > 20 %, no claim |
| s3 join-loan-user (10,000 loans, indexed) | 23,753.0 qps | 31.3 % | 16,420.4 qps | 17.6 % | 0.69x | spread > 20 %, no claim |
| s3 join-no-literal (10,000 loans, indexed) | 9,363.3 qps | 25.6 % | 6,896.6 qps | 8.2 % | 0.74x | spread > 20 %, no claim |
| s3 exists-correlated (10,000 loans, indexed) | 17,241.4 qps | 29.9 % | 11,655.0 qps | 12.2 % | 0.68x | spread > 20 %, no claim |
| s3 count-by-user (10,000 loans, indexed) | 39,682.5 qps | 42.5 % | 23,201.9 qps | 19.2 % | 0.58x | spread > 20 %, no claim |
