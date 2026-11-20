| measure | v3.4.14 engine | spread | v3.5.0 engine | spread | new over old | read |
|---|---|---|---|---|---|---|
| s2 8 bookers, cores 1, group | 1,187.4 tps | 1.8 % | 1,170.5 tps | 0.7 % | 0.99x | inside the spread |
| s2 1 booker, cores 1, strict | 605.8 tps | 3.9 % | 619.7 tps | 3.7 % | 1.02x | inside the spread |
| s2 1 booker strict, commit (1e6 / mean) | 1,800.2 qps | 4.2 % | 1,829.2 qps | 1.4 % | 1.02x | inside the spread |
| s2 1 booker strict, recipe-read (1e6 / mean) | 6,353.2 qps | 9.7 % | 6,570.3 qps | 6.6 % | 1.03x | inside the spread |
| s2 1 booker strict, charge-insert (1e6 / mean) | 20,661.2 qps | 6.7 % | 21,186.4 qps | 3.3 % | 1.03x | inside the spread |
