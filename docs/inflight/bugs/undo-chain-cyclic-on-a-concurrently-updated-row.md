# A read of a row other sessions keep updating is refused `Corruption`, "undo chain revisits the record"

**What is wrong.** A read whose version-chain walk reaches a row that
other sessions are updating concurrently is refused `Corruption` with
`undo chain revisits the record at <ptr>; the links are cyclic`. The
message is the cycle guard's on the version-chain walk
(`src/txn/visibility.cpp:50`; the same text is at
`src/txn/undo_log.cpp:214`). It has been seen in two shapes:

- **a walk**: scenario 0's reporter, `SELECT ... FROM s0_accounts WHERE
  user_id = ?`, over a BTREE relation whose rows 1 to 4 traders update by
  key (`UPDATE s0_accounts SET ... WHERE id = ?`), both in autocommit
  (`txn=0`, the default) and in explicit transactions with a foreign key
  share (`txn=1 fk=1`);
- **a point read by key of a row the reading transaction has just
  updated**: scenario 5's new order, `UPDATE s5_districts SET d_next_o_id =
  d_next_o_id + 1 WHERE d_key = ?` then `SELECT d_tax, d_next_o_id FROM
  s5_districts WHERE d_key = ?` in one explicit transaction, four
  terminals on one warehouse's ten districts, `strict`.

Only the read is refused; every consistency check either scenario runs
afterwards passed (balances, quantities, row counts; TPC-C's conditions 1
to 4), so no wrong answer has been seen.

**Verified at** `e571ae574` (`v3.5.1-57-ge571ae574`): a Release
`kds_server` built from that engine source (sha256 prefix
`e961b65c1852fe50`), `cores = 1`. Found by EN-S2's scenario 0 port and
counted in EN's close measurement (`bench/v3.5.1/archive/en-close-v3.5.1-63-g9543cf720/timed/*.serverlog`):

| cell | refusals over 3 rounds |
|---|---|
| s0 four traders, `group`, `txn=0` | 8 refusals (2, 5, 1) of 1,000 to 1,050 reporter scans a round |
| s0 one trader, `group`, `txn=0` | 1 refusal (round 3) |
| s5 four terminals, `strict` | 1 refusal (round 1), a new order |
| every other cell | 0 refusals |

**Reproduction.** A fresh server, then repeated (3 runs of 6 by CLA):

    tools/eqbench/target/release/eqbench --engine kds --kds 127.0.0.1:<port> \
      --scenario s0 --set users=300 --set assets=100 --set seconds=6 \
      --set traders=3 --set profit_users=20 --set txn=1 --set fk=1 --no-profile

**What it costs.** A refusal of a read, `Corruption`-classed, of data that
every later read finds consistent: a refusal, not a quiet wrong answer, as
far as observed. **Unverified**: whether the chain the reader walks is
truly cyclic (undo storage reused while a live view or the writer's own
transaction can still reach it, a read-horizon defect that could equally
return a wrong version), or whether the cycle guard misreads a legal chain
that concurrent updates of one row build. A debug build with the chain
dumped at the guard would tell the two apart.

**Fix**: unknown, and no work order carries it. EN's results count these
refusals in each cell's errors and take no figure from a refused
operation.
