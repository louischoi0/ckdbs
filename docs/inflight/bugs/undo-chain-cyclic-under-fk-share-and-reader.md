# A reader is refused `Corruption`, "undo chain revisits the record", under explicit transactions with a foreign key's share

**What is wrong.** A plain `SELECT ... FROM accounts WHERE user_id = ?`
(a walk over a BTREE relation) running beside three writers is refused
`Corruption` with `undo chain revisits the record at <ptr>; the links are
cyclic`. The writers run explicit transactions of two `INSERT`s into a
child relation whose `account_id` references `accounts`, plus two
`UPDATE accounts SET ... WHERE id = ?`. The message is the cycle guard's
on the version-chain walk (`src/txn/visibility.cpp:50`; the same text is at
`src/txn/undo_log.cpp:214`). Only the read is refused. Every check the
scenario runs afterwards passes (balances, quantities and row counts),
so no wrong answer has been seen.

**Verified at** `e571ae574` (`v3.5.1-57-ge571ae574`): a Release
`kds_server` built from that engine source (sha256 prefix
`e961b65c1852fe50`), `cores = 1`, `durability = group`. Found by EN-S2's
scenario 0 port (`tools/eqbench/src/scenarios/s0_stockmarket.rs`), on
worktree `three-engine-equal-client-bench`.

**Reproduction.** A fresh server, then the following, repeated: the
refusal appeared in 3 of 6 runs by CLA (and 3 of 8 runs, 0 of 5 with
`fk=0`, 0 of 6 with `txn=0`, by the porting pass).

    tools/eqbench/target/release/eqbench --engine kds --kds 127.0.0.1:<port> \
      --scenario s0 --set users=300 --set assets=100 --set seconds=6 \
      --set traders=3 --set profit_users=20 --set txn=1 --set fk=1 --no-profile

**What it costs.** A refusal of a read, `Corruption`-classed, on data
that every later read finds consistent. That makes it a refusal, not a
quiet wrong answer, as far as observed. **Unverified**: whether the chain
the reader walks is truly cyclic (undo storage reused while a live view
can still reach it, which is a read-horizon defect and could equally
produce a wrong version), or whether the cycle guard misreads a legal chain
the foreign key's `S` share produces. A debug build with the chain dumped
at the guard would tell the two apart.

**Fix**: unknown, and no work order carries it. EN measures scenario 0 at
`txn=0 fk=0`, its default, where the refusal was not seen.
