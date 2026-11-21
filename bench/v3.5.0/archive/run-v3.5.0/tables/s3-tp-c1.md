| loans | index | shape | now | spread | runs | v3.4.14 | spread | delta | read |
|---|---|---|---|---|---|---|---|---|---|
| 200 | none | pk-user | 16,000 qps | 5.0 % | 3 | 16,313 qps | 3.4 % | -1.9 % | inside the spread |
| 200 | none | loans-by-user | 12,422 qps | 8.2 % | 3 | 13,089 qps | 8.3 % | -5.1 % | inside the spread |
| 200 | none | loans-by-book | 10,799 qps | 49.1 % | 3 | 13,831 qps | 7.9 % | -21.9 % | spread > 20 %, no claim |
| 200 | none | resv-by-user | 9,443 qps | 67.1 % | 3 | 15,314 qps | 8.0 % | -38.3 % | spread > 20 %, no claim |
| 200 | none | books-by-author | 12,438 qps | 48.1 % | 3 | 14,925 qps | 6.6 % | -16.7 % | spread > 20 %, no claim |
| 200 | none | books-by-genre | 11,587 qps | 39.0 % | 3 | 15,083 qps | 6.2 % | -23.2 % | spread > 20 %, no claim |
| 200 | none | loans-by-daterange | 12,195 qps | 3.0 % | 3 | 12,180 qps | 1.2 % | +0.1 % | inside the spread |
| 200 | none | overdue | 13,245 qps | 0.7 % | 3 | 8,621 qps | 64.3 % | +53.6 % | spread > 20 %, no claim |
| 200 | none | join-loan-user | 8,019 qps | 55.7 % | 3 | 6,523 qps | 87.5 % | +22.9 % | spread > 20 %, no claim |
| 200 | none | join-no-literal | 4,621 qps | 32.7 % | 3 | 4,225 qps | 39.9 % | +9.4 % | spread > 20 %, no claim |
| 200 | none | exists-correlated | 8,104 qps | 40.3 % | 3 | 8,688 qps | 39.6 % | -6.7 % | spread > 20 %, no claim |
| 200 | none | count-by-user | 13,405 qps | 41.0 % | 3 | 17,301 qps | 30.1 % | -22.5 % | spread > 20 %, no claim |
| 200 | all | pk-user | 15,974 qps | 2.0 % | 3 | 16,155 qps | 2.6 % | -1.1 % | inside the spread |
| 200 | all | loans-by-user | 12,594 qps | 5.5 % | 3 | 12,937 qps | 4.2 % | -2.6 % | inside the spread |
| 200 | all | loans-by-book | 13,298 qps | 6.7 % | 3 | 13,423 qps | 6.6 % | -0.9 % | inside the spread |
| 200 | all | resv-by-user | 14,388 qps | 32.2 % | 3 | 15,221 qps | 4.7 % | -5.5 % | spread > 20 %, no claim |
| 200 | all | books-by-author | 13,928 qps | 7.3 % | 3 | 14,065 qps | 10.6 % | -1.0 % | inside the spread |
| 200 | all | books-by-genre | 14,749 qps | 3.2 % | 3 | 15,152 qps | 9.8 % | -2.7 % | inside the spread |
| 200 | all | loans-by-daterange | 12,019 qps | 2.2 % | 3 | 11,834 qps | 7.0 % | +1.6 % | inside the spread |
| 200 | all | overdue | 13,123 qps | 0.9 % | 3 | 13,316 qps | 10.1 % | -1.4 % | inside the spread |
| 200 | all | join-loan-user | 11,351 qps | 4.5 % | 3 | 11,933 qps | 3.4 % | -4.9 % | **finding** |
| 200 | all | join-no-literal | 4,367 qps | 1.7 % | 3 | 4,484 qps | 2.8 % | -2.6 % | inside the spread |
| 200 | all | exists-correlated | 7,342 qps | 8.3 % | 3 | 7,831 qps | 2.4 % | -6.2 % | inside the spread |
| 200 | all | count-by-user | 16,807 qps | 7.0 % | 3 | 17,544 qps | 3.1 % | -4.2 % | inside the spread |
| 1,000 | none | pk-user | 15,504 qps | 19.3 % | 3 | 16,207 qps | 5.6 % | -4.3 % | inside the spread |
| 1,000 | none | loans-by-user | 10,194 qps | 18.7 % | 3 | 10,593 qps | 3.3 % | -3.8 % | inside the spread |
| 1,000 | none | loans-by-book | 10,965 qps | 2.4 % | 3 | 11,111 qps | 1.1 % | -1.3 % | inside the spread |
| 1,000 | none | resv-by-user | 13,369 qps | 46.4 % | 3 | 13,495 qps | 1.9 % | -0.9 % | spread > 20 %, no claim |
| 1,000 | none | books-by-author | 12,658 qps | 49.1 % | 3 | 13,986 qps | 1.2 % | -9.5 % | spread > 20 %, no claim |
| 1,000 | none | books-by-genre | 11,274 qps | 43.6 % | 3 | 11,507 qps | 1.5 % | -2.0 % | spread > 20 %, no claim |
| 1,000 | none | loans-by-daterange | 5,851 qps | 31.7 % | 3 | 6,274 qps | 0.6 % | -6.7 % | spread > 20 %, no claim |
| 1,000 | none | overdue | 6,978 qps | 33.8 % | 3 | 6,954 qps | 8.6 % | +0.3 % | spread > 20 %, no claim |
| 1,000 | none | join-loan-user | 9,355 qps | 44.3 % | 3 | 9,542 qps | 27.7 % | -2.0 % | spread > 20 %, no claim |
| 1,000 | none | join-no-literal | 4,000 qps | 29.3 % | 3 | 4,417 qps | 2.3 % | -9.4 % | spread > 20 %, no claim |
| 1,000 | none | exists-correlated | 5,688 qps | 28.0 % | 3 | 6,297 qps | 8.1 % | -9.7 % | spread > 20 %, no claim |
| 1,000 | none | count-by-user | 10,111 qps | 41.4 % | 3 | 13,569 qps | 4.2 % | -25.5 % | spread > 20 %, no claim |
| 1,000 | all | pk-user | 16,026 qps | 2.1 % | 3 | 15,625 qps | 7.4 % | +2.6 % | inside the spread |
| 1,000 | all | loans-by-user | 12,739 qps | 8.6 % | 3 | 12,690 qps | 11.9 % | +0.4 % | inside the spread |
| 1,000 | all | loans-by-book | 13,441 qps | 2.1 % | 3 | 13,423 qps | 1.8 % | +0.1 % | inside the spread |
| 1,000 | all | resv-by-user | 14,286 qps | 2.6 % | 3 | 14,556 qps | 4.2 % | -1.9 % | inside the spread |
| 1,000 | all | books-by-author | 13,966 qps | 7.2 % | 3 | 14,184 qps | 11.3 % | -1.5 % | inside the spread |
| 1,000 | all | books-by-genre | 10,905 qps | 2.1 % | 3 | 10,204 qps | 25.5 % | +6.9 % | spread > 20 %, no claim |
| 1,000 | all | loans-by-daterange | 6,124 qps | 0.2 % | 3 | 6,050 qps | 2.4 % | +1.2 % | inside the spread |
| 1,000 | all | overdue | 7,519 qps | 23.4 % | 3 | 7,728 qps | 18.2 % | -2.7 % | spread > 20 %, no claim |
| 1,000 | all | join-loan-user | 11,574 qps | 15.0 % | 3 | 11,905 qps | 2.1 % | -2.8 % | inside the spread |
| 1,000 | all | join-no-literal | 4,284 qps | 2.0 % | 3 | 4,355 qps | 8.8 % | -1.6 % | inside the spread |
| 1,000 | all | exists-correlated | 7,474 qps | 5.5 % | 3 | 7,616 qps | 1.5 % | -1.9 % | inside the spread |
| 1,000 | all | count-by-user | 17,301 qps | 1.0 % | 3 | 17,361 qps | 1.4 % | -0.3 % | inside the spread |
| 10,000 | none | pk-user | 15,152 qps | 11.2 % | 3 | 16,129 qps | 3.8 % | -6.1 % | inside the spread |
| 10,000 | none | loans-by-user | 3,180 qps | 4.8 % | 3 | 3,186 qps | 6.3 % | -0.2 % | inside the spread |
| 10,000 | none | loans-by-book | 3,204 qps | 4.1 % | 3 | 3,278 qps | 2.4 % | -2.2 % | inside the spread |
| 10,000 | none | resv-by-user | 5,831 qps | 7.7 % | 3 | 5,872 qps | 4.1 % | -0.7 % | inside the spread |
| 10,000 | none | books-by-author | 8,628 qps | 2.9 % | 3 | 8,873 qps | 6.2 % | -2.8 % | inside the spread |
| 10,000 | none | books-by-genre | 2,821 qps | 5.6 % | 3 | 2,742 qps | 3.2 % | +2.9 % | inside the spread |
| 10,000 | none | loans-by-daterange | 907 qps | 3.0 % | 3 | 900 qps | 1.9 % | +0.8 % | inside the spread |
| 10,000 | none | overdue | 1,121 qps | 3.6 % | 3 | 1,126 qps | 2.6 % | -0.5 % | inside the spread |
| 10,000 | none | join-loan-user | 3,040 qps | 10.9 % | 3 | 3,145 qps | 3.7 % | -3.3 % | inside the spread |
| 10,000 | none | join-no-literal | 1,954 qps | 4.0 % | 3 | 1,934 qps | 3.3 % | +1.0 % | inside the spread |
| 10,000 | none | exists-correlated | 1,520 qps | 2.9 % | 3 | 1,588 qps | 6.2 % | -4.3 % | inside the spread |
| 10,000 | none | count-by-user | 3,362 qps | 4.5 % | 3 | 3,470 qps | 2.4 % | -3.1 % | inside the spread |
| 10,000 | all | pk-user | 14,065 qps | 4.3 % | 3 | 15,848 qps | 6.4 % | -11.3 % | **finding** |
| 10,000 | all | loans-by-user | 12,180 qps | 8.0 % | 3 | 12,346 qps | 1.4 % | -1.3 % | inside the spread |
| 10,000 | all | loans-by-book | 13,193 qps | 2.0 % | 3 | 13,175 qps | 4.8 % | +0.1 % | inside the spread |
| 10,000 | all | resv-by-user | 14,006 qps | 6.9 % | 3 | 14,327 qps | 38.1 % | -2.2 % | spread > 20 %, no claim |
| 10,000 | all | books-by-author | 13,459 qps | 6.7 % | 3 | 13,477 qps | 3.8 % | -0.1 % | inside the spread |
| 10,000 | all | books-by-genre | 2,616 qps | 2.1 % | 3 | 2,662 qps | 1.9 % | -1.7 % | inside the spread |
| 10,000 | all | loans-by-daterange | 881 qps | 0.6 % | 3 | 891 qps | 1.9 % | -1.1 % | inside the spread |
| 10,000 | all | overdue | 1,308 qps | 5.0 % | 3 | 1,329 qps | 3.0 % | -1.6 % | inside the spread |
| 10,000 | all | join-loan-user | 11,223 qps | 5.6 % | 3 | 11,390 qps | 2.3 % | -1.5 % | inside the spread |
| 10,000 | all | join-no-literal | 4,401 qps | 2.4 % | 3 | 4,301 qps | 8.5 % | +2.3 % | inside the spread |
| 10,000 | all | exists-correlated | 7,541 qps | 1.5 % | 3 | 7,541 qps | 1.4 % | +0.0 % | inside the spread |
| 10,000 | all | count-by-user | 16,207 qps | 9.8 % | 3 | 17,123 qps | 0.5 % | -5.3 % | inside the spread |
