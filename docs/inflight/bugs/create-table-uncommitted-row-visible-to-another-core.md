# An uncommitted `CREATE TABLE` may be visible to another core

**Found by reading, not reproduced.** Read at `3ee37d3a` on
`worktree-compress-readme`, 2026-10-13, during the docs-and-comments
compression pass (a subagent's report on `ddl-transactional.md`).

## What is wrong

`CommandDispatcher::ViewFor` (`src/server/command_dispatcher.cpp:6545`)
returns `std::nullopt` - resolve names with no read view, so every catalog
row reads as present - whenever `ddl_txns_` is empty:

```
if (txn_ == nullptr || ddl_txns_.empty()) return std::nullopt;
```

`ddl_txns_` (`include/kds/server/command_dispatcher.hpp:1143`) is a member
of the dispatcher, and every core has its own dispatcher. A `CREATE TABLE`
inside an explicit transaction on core A registers its id in core A's set
only. On core B the set is empty, so B resolves names with no view and
looks as if it would bind the relation A has not committed.

`docs/spec/ddl-transactional.md` §4/§5 and `CLAUDE.md`'s Transactional DDL
row state that `CREATE TABLE` is isolated. If the reading is right, that
holds on one core only.

## Smallest reproduction (not yet run)

Two sessions on two cores (`cores = 2`, force placement with the load
placer off): session 1 runs `BEGIN; CREATE TABLE t (id int, v int);` and
does not commit. Session 2, on the other core, runs `SELECT * FROM t`. The
spec's answer is "no such relation"; the reading predicts that the name
binds.

## What it costs

An isolation breach on DDL. A reader on another core can bind a relation
that is later rolled back. Whether it can also write rows into it, or fail
quietly afterwards, has not been checked. Not yet classified as a quiet
wrong answer. The first step is to run the reproduction.
