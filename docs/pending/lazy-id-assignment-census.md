# Lazy id assignment for an undeclared transaction: the census a later order needs

Status: **PENDING.** Raised 2026-11-15 by work order CS
(`instructions/v3.2.0/workorder-cs-read-only-transaction.md` §0.2 and
CS-Q9 (a), adopted on the operator's standing go-ahead of 2026-11-15 -
go-ahead-achieving-milestone - not the operator's own mark). Every
`[source-read]` line number is at `bbaff33a` (`v3.4.11-3-gbbaff33a`),
before CS-S1's code. Related: `docs/spec/txn.md` §1 (the access mode CS
built), §4.1; `include/kds/txn/read_view.hpp`; `src/server/command_dispatcher.cpp`.

## What prompted it

`[design]` CS gives a client that declares `BEGIN READ ONLY` a transaction
with no id, no record and no in-flight slot. An undeclared `BEGIN` still
draws its id at `BEGIN` (`TransactionManager::Begin`, `src/txn/manager.cpp:125-166`),
so a client that reads in a `READ WRITE` transaction pays for it
(`bench/v3.4.11/results-cs-s0-premise-v3.4.11-3-gbbaff33a.md`: 64 WAL bytes
and one id per transaction, and the `strict` sync at `COMMIT`).
PostgreSQL and InnoDB draw the id at the first write instead. That would give
every read-only explicit transaction CS's savings without the keyword, but
every path that reads the id before it is drawn reads `kNoTrxId` (`0`), and
that is `[quiet-wrong]`: a tuple stamped `0` is invisible to every view until
the first floor reclaim and visible to every view, older snapshots
included, after it (`read_view.hpp:106-115`, `instance_visibility.cpp:322-334`,
the floor starting at 0 at `instance_visibility.hpp:653`). Recovery skips
records with `txn_id == kNoTxnId` (`src/wal/analysis.cpp:53`, `:70`).

## The tree as read `[source-read]`

**Readings of `Transaction::id()`** (`manager.hpp:138`; only `src/server`
reads it, the manager uses `id_`), dispatcher line numbers:

- **Lock family**: waiter ids `:401`, `:404`, `:664`; the FK parent locks
  `:4753`, `:4875`, `:4878`; the insert's `ReleaseIf` `:6549`, `:7128`;
  `BorrowRelationForDdl` `:11437`, `BorrowChain` `:11476`,
  `UniqueKeyHolds::Release` `:11779`; progress registration `:3060`, `:3998`.
- **Row and WAL stamps**: spill and index logs `:5410`, `:5430`; UPDATE's
  `new_trx_id` and `HEAP_OVERWRITE` `:10716`, `:10790`; DELETE's and
  `HEAP_DELETE_MARK` `:13030`, `:13058`; `WriterId` `:12075` and its callers
  `:4222` (`CheckView`), `:7075`, `:7189`, `:7219`, `:7344`, `:7387` (insert
  admit, reserve, place), `:10636` (update reserve), `:11849` (unique-key
  check view), `:13008` (delete reserve).
- **Own-version and visibility tests**: `:6516`, `:6772`, `:10219`; the
  index build run `:2684`, `:2699`, `:2731`.
- **DDL bookkeeping**: `:3845`, `:7591` (`DdlScope` - `OpenDdl` and the undo
  hook), `:7668`, `:7789`, `:7800`; rename `:3399`.
- **Commit, abort and the reply**: `:11045`, `:11147`, `:12013`, `:12041`;
  `:11020`.
- **Outside the dispatcher**: the lifetime sweep's stamp,
  `tcp_server.cpp:935`, `:938`, `:972-974` (keyed on `open_txn_seq()` since
  CS-S1).

**Null-transaction tests on a write scope** (each means "no manager" today,
and a lazy id would give it a second meaning): `:2683 2708 2797 3059 3844
4752 4871 5409 5429 6530 6545 6669 7127 7166 7367 7377 7388 7667 7796 7885
7903 10069 10154 10219 10697 10775 11474 11929 12075 13017 13053`.

**`txn::kNoTrxId` comparisons and uses**: comparisons at `read_view.hpp:108`,
`src/exec/step_vm.cpp:938` (the Cabin bank's decline), `src/catalog/catalog.cpp:1614`,
`:4305`; default parameters `catalog.hpp:573`, `:1289`; `prior_trx_id`
stamps `manager.cpp:246`, `command_dispatcher.cpp:7369`, `:7620`; views
minted with no owner `:3084`, `:3251`, `:3400`, `:3975`, `:7727`,
`cabin_optimizer_exec.cpp:32`, `catalog.cpp:1594`, `:4282`, `manager.hpp:865`.
The parallel `wal::kNoTxnId == 0` convention is compared at
`wal/analysis.cpp:53`, `:70`, `wal/payload.cpp:558` and
`catalog.cpp:234, 255, 269, 312, 327, 356`, with about 40 uses across
catalog, exec, wal and backup.

## The options `[design]`

- **(a) Draw the id at the first `BeginWrite`** of a `READ WRITE`
  transaction, with every reading above moved behind a "drawn" test. The
  in-flight publication (AX-R2) and the `TXN_BEGIN` record move with it,
  and the view minted at `BEGIN` has `own_trx_id = 0` until then. That is
  the hard part: `Visible`'s own-write arm must start answering for the new
  id at the draw.
- **(b) Leave it.** `BEGIN READ ONLY` is the declared form, and the
  undeclared one keeps its cost.

Nothing is decided here.
