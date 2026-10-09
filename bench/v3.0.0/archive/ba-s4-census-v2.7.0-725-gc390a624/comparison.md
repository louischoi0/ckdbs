
### KDS `relaxed` against PostgreSQL `synchronous_commit = off`, statements/s at 16 sessions (and 1)

| shape | KDS k1 | KDS k2 | KDS k4 | PG CPU 0 | PG CPUs 0,2 | PG CPUs 0,2,4,6 |
|---|---|---|---|---|---|---|
| point | 43,409 stmt/s (14,521) | 65,347 stmt/s (14,262) | 55,404 stmt/s (14,016) | 26,960 stmt/s (16,974) | 54,433 stmt/s (16,929) | 80,327 stmt/s (16,998) |
| trade | 37,118 stmt/s (16,436) | 60,113 stmt/s (16,110) | 51,024 stmt/s (15,905) | 18,540 stmt/s (13,812) | 37,593 stmt/s (13,823) | 55,594 stmt/s (13,688) |
| insert1 | 38,166 stmt/s (17,799) | 68,341 stmt/s (17,693) | 56,560 stmt/s (17,318) | 27,186 stmt/s (17,332) | 54,628 stmt/s (17,296) | 77,684 stmt/s (17,368) |
| insertN | 37,721 stmt/s (17,955) | 64,647 stmt/s (17,582) | 53,013 stmt/s (17,344) | 26,933 stmt/s (17,299) | 54,282 stmt/s (17,431) | 79,473 stmt/s (17,267) |
| recent | 34,206 stmt/s (14,344) | 53,233 stmt/s (14,044) | 46,590 stmt/s (13,785) | 21,652 stmt/s (14,173) | 43,880 stmt/s (14,222) | 63,365 stmt/s (14,166) |

### KDS `group` against PostgreSQL `synchronous_commit = on`, statements/s at 16 sessions (and 1)

| shape | KDS k1 | KDS k2 | KDS k4 | PG CPU 0 | PG CPUs 0,2 | PG CPUs 0,2,4,6 |
|---|---|---|---|---|---|---|
| point | 43,412 stmt/s (14,527) | 65,331 stmt/s (14,144) | 56,397 stmt/s (14,159) | 27,051 stmt/s (16,965) | 54,758 stmt/s (16,963) | 80,366 stmt/s (17,056) |
| trade | 4,977 stmt/s (676) | 1,656 stmt/s (657) | 3,905 stmt/s (654) | 4,902 stmt/s (673) | 5,150 stmt/s (678) | 5,286 stmt/s (568) |
| insert1 | 4,927 stmt/s (668) | 2,922 stmt/s (671) | 3,991 stmt/s (682) | 5,120 stmt/s (708) | 5,325 stmt/s (691) | 5,220 stmt/s (674) |
| insertN | 4,918 stmt/s (701) | 3,558 stmt/s (689) | 4,226 stmt/s (687) | 4,857 stmt/s (647) | 5,035 stmt/s (686) | 5,173 stmt/s (672) |
| recent | 6,033 stmt/s (1,274) | 6,325 stmt/s (1,253) | 7,376 stmt/s (1,250) | 9,733 stmt/s (1,232) | 9,942 stmt/s (1,238) | 10,171 stmt/s (1,257) |

### KDS `strict` against PostgreSQL `synchronous_commit = on`, statements/s at 16 sessions (and 1)

| shape | KDS k1 | KDS k2 | KDS k4 | PG CPU 0 | PG CPUs 0,2 | PG CPUs 0,2,4,6 |
|---|---|---|---|---|---|---|
| point | 43,725 stmt/s (14,390) | 65,444 stmt/s (14,381) | 51,301 stmt/s (14,269) | 27,051 stmt/s (16,965) | 54,758 stmt/s (16,963) | 80,366 stmt/s (17,056) |
| trade | 684 stmt/s (680) | 721 stmt/s (653) | 857 stmt/s (655) | 4,902 stmt/s (673) | 5,150 stmt/s (678) | 5,286 stmt/s (568) |
| insert1 | 728 stmt/s (708) | 703 stmt/s (702) | 1,010 stmt/s (665) | 5,120 stmt/s (708) | 5,325 stmt/s (691) | 5,220 stmt/s (674) |
| insertN | 726 stmt/s (688) | 752 stmt/s (706) | 985 stmt/s (692) | 4,857 stmt/s (647) | 5,035 stmt/s (686) | 5,173 stmt/s (672) |
| recent | 1,336 stmt/s (1,248) | 1,472 stmt/s (1,248) | 1,867 stmt/s (1,252) | 9,733 stmt/s (1,232) | 9,942 stmt/s (1,238) | 10,171 stmt/s (1,257) |
