| measure | `07f4552a` engine | spread | `9f8f8b354` engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s3 ddl (10,000 loans, indexed) | 1,711.7 qps | 37.9 % | 1,736.4 qps | 89.0 % | 1.01x | spread > 20 %, no claim |
| s3 load (10,000 loans, indexed) | 6,729.5 qps | 66.7 % | 6,626.9 qps | 64.2 % | 0.98x | spread > 20 %, no claim |
| s3 create-index (10,000 loans, indexed) | 215.1 qps | 18.6 % | 218.8 qps | 6.1 % | 1.02x | inside the spread |
| s3 pk-user (10,000 loans, indexed) | 18,975.3 qps | 10.1 % | 18,181.8 qps | 13.8 % | 0.96x | inside the spread |
| s3 loans-by-user (10,000 loans, indexed) | 25,062.7 qps | 6.4 % | 24,813.9 qps | 2.2 % | 0.99x | inside the spread |
| s3 loans-by-book (10,000 loans, indexed) | 27,027.0 qps | 19.1 % | 27,173.9 qps | 10.0 % | 1.01x | inside the spread |
| s3 resv-by-user (10,000 loans, indexed) | 29,761.9 qps | 1.5 % | 29,761.9 qps | 22.8 % | 1.00x | spread > 20 %, no claim |
| s3 books-by-author (10,000 loans, indexed) | 27,624.3 qps | 12.0 % | 27,777.8 qps | 6.6 % | 1.01x | inside the spread |
| s3 books-by-genre (10,000 loans, indexed) | 5,361.9 qps | 4.7 % | 5,382.1 qps | 13.2 % | 1.00x | inside the spread |
| s3 loans-by-daterange (10,000 loans, indexed) | 1,507.4 qps | 1.2 % | 1,524.9 qps | 2.7 % | 1.01x | inside the spread |
| s3 overdue (10,000 loans, indexed) | 2,371.9 qps | 2.8 % | 2,388.3 qps | 1.6 % | 1.01x | inside the spread |
| s3 join-loan-user (10,000 loans, indexed) | 24,038.5 qps | 6.2 % | 23,696.7 qps | 4.2 % | 0.99x | inside the spread |
| s3 join-no-literal (10,000 loans, indexed) | 8,888.9 qps | 3.1 % | 8,635.6 qps | 7.5 % | 0.97x | inside the spread |
| s3 exists-correlated (10,000 loans, indexed) | 17,064.8 qps | 5.4 % | 16,420.4 qps | 2.9 % | 0.96x | inside the spread |
| s3 count-by-user (10,000 loans, indexed) | 34,722.2 qps | 2.1 % | 34,482.8 qps | 3.1 % | 0.99x | inside the spread |
