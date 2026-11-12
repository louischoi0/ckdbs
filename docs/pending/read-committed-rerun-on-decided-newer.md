# Pending: re-run instead of refuse when a row's newer version is decided, under READ COMMITTED

Status: **PENDING** - put here by CQ-Q9 (a), which the operator adopted on
2026-11-11 (`instructions/v3.2.0/raft-marks-2026-11-11.md` §7): the re-run
is "priced in S0, not built in CQ; if S0 shows what it recovers, it goes
to `docs/pending/` as the operator's question on `txn.md` §5". Decides
nothing, opens no stage.
Raised: 2026-11-11, by CQ-S0's result.
Read at: `ca7124f6` (`v3.4.7-15-gca7124f6`).
Related: `docs/spec/txn.md` §5 (the rule);
`instructions/v3.2.0/workorder-cq-column-reservation.md` §3 (A1), §8 and
§9.1; `bench/v3.4.7/results-cq-s0-premise-v3.4.7-13-g92449e65.md`;
`bench/v3.4.7/results-cp-s0-premise-v3.4.7-1-gcc54a296.md` (M4);
`instructions/v3.2.0/workorder-cp-lock-model-exploration.md`.

## 1. What prompted it

`[measured]` Two measurements found that what a client sees on a
contended row is not a lock wait but this refusal:

| measurement | shape | refused |
|---|---|---|
| CP-S0 M4, `v3.4.7-1-gcc54a296`, `cores = 4`, `relaxed` | 8 sessions, autocommit, an absolute `SET` on one row | `2,519` of `32,000` statements (`7.9 %`), all `TXN_CONFLICT` |
| CQ-S0 M1, `v3.4.7-13-g92449e65`, `cores = 4`, `group` | 8 sessions, an explicit transaction debiting one row | `1.4-12.8 %` of attempts across the hot arms, every one decided-newer, each failing its transaction; more once the debit is the last statement (`1.4 %`, `2.5 %`, `3.3 %` at eight following statements) |
| CQ-S0 M2, the same commit, `strict` | the same | `0-0.4 %` |

No deadlock and no fault-net refusal stands behind any of them.

## 2. What the tree does today

- `[source-read]` A writer whose view cannot see the row's last writer is
  refused `TxnConflict`, whether that writer is in flight or committed
  after the view (`src/txn/manager.cpp:188-207`). `docs/spec/txn.md:772-776`
  states it as a choice: under `READ COMMITTED` "KDS aborts retryably
  rather than re-reading ... a deliberate simplification - there is no
  re-read loop".
- `[source-read]` An in-flight writer is waited for and the statement run
  again (`txn.md` §5); it is the **decided** newer writer that is refused.
- `[source-read]` Under `READ COMMITTED` every statement mints a fresh
  view (`src/txn/manager.cpp:173-179`), so the window is from that mint to
  the statement's write.
- `[source-read]` A view's ceiling is capped by every open commit's
  marker, and a marker can hide a commit that is already acknowledged
  (`include/kds/txn/instance_visibility.hpp:62-105`). `[design]` So a
  writer that waited for a holder and runs again at its release can still
  mint a view below that holder's commit while a third commit is open, and
  be refused; whether this is the larger part of the refusals was **not
  measured**.
- `[source-read]` A statement that wrote nothing is already run again for
  a stale descent (BA-S14) and after a lock wait.

## 3. The options

| | option | cost | gives |
|---|---|---|---|
| (a) | Leave it: the client retries | none | - |
| (b) | **Autocommit only**: a statement refused decided-newer that has written nothing is run again under a fresh view, inside its deadline | a contract change in `txn.md` §5 and the client manual; a bound on the rounds; if §2's marker case is the cause, the re-run must first wait for the ceiling to cover the commit it lost to, as a session's own commit is waited for | M4's `7.9 %` |
| (c) | (b), and inside an explicit `READ COMMITTED` transaction for a statement that has written nothing | (b)'s, and a statement-level re-run where today the transaction fails | CQ-S0's `1.4-12.8 %` |

`REPEATABLE READ` is outside every option: there the refusal is
first-updater-wins and is the level's contract.

`[design]` The re-run changes no result a client could not already get by
retrying; it moves the retry into the engine. What it costs in latency
under sustained contention on one row is not known and would be the first
thing to measure.
