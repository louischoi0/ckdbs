# A rollback whose pk descent is refused leaves its keys bound

**Found by measurement, not reproduced as a cell.** Seen on 2026-10-11 by
BI-S1's measurement (`instructions/v3.0.0/workorder-bi-checkpoint-off-the-reactor.md`
§5.1), against the build-release of `55ee8370` (`v2.7.0-779-g55ee8370`,
BI-S1's instrumentation over `main` at `0e6e7d8d`; nothing BI changed is on
the insert or rollback path). Evidence: the server's log and config,
archived at
`/home/cdkbs/bench-runs/bi-s1-v2.7.0-779-g55ee8370/evidence/c4-relaxed-r1.{log,conf}`.

## What was seen

`[measured]` `cores = 4`, `durability = relaxed`, four sessions of
1024-row named-key `INSERT`s into one btree, each session its own
ascending key range (`tools/bi_checkpoint_stall.py`).

1. One statement, keys 3005127168..3005128191, was refused
   `ERR TXN_CONFLICT retryable=1 btree descent for key 3005127896 from page
   999 gave up after 5 attempts ... (row 729)`.
2. The same second, the log carries 645 lines
   `WARN [btree] descent for pk 30051271xx in table oid 4000 failed, falling
   back to a scan: btree descent for key ... gave up after 5 attempts`, one
   per key the statement had placed, in descending order - the rollback
   relocating each row it inserted.
3. The client retried the same statement, as `retryable=1` invites, and it
   was refused `ERR duplicate primary key 3005127168 already present at page
   276967 slot 3 (byte 25) (row 1)`.

A second refusal of the same kind (`row 539`, keys from 4000003072)
earlier in the same run was retried without a duplicate. `[measured]` Across
the measurement's `cores = 4` cells the driver then recorded six retried
refusals, three of which met the duplicate; none occurred at `cores = 1`
(`bench/v3.0.0/results-bi-s1-checkpoint-stall-v2.7.0-779-g55ee8370.md`).

## What the source says

- `[source-read]` The rollback relocates a row through
  `CommandDispatcher::LocateByPk` (`src/server/command_dispatcher.cpp`, the
  relocator above it). A btree descent that fails with anything but
  `NotFound` is answered `kScan`, and the relocator refuses `kScan` rather
  than guess a slot: *"could not be relocated for rollback"*.
- `[design]` So a statement refused because a descent cannot cross a
  splitting chain hands its rollback the same descents, under the same
  splits; each can be refused the same way, and each refused one leaves its
  row in its slot. The keys stay bound, contrary to `heap-and-tuple.md`
  §4.1's *"a rolled-back key is free"* (BD-R4), and the retry `retryable=1`
  invites is refused `AlreadyExists`.

## What it costs, and what is not known

- A client that retries as told is refused, and those keys stay refused.
- `[design]` Not established: whether the rows left behind are invisible
  (their transaction aborted) or read as live, i.e. whether this is a
  refusal or a quiet wrong answer; and what the rollback does with the
  transaction once a row could not be relocated. The database of the run
  was deleted with the cell, so neither could be read off it.

## The fix

Not designed. A cell first: four cores inserting ascending named keys into
one btree until a statement is refused at a descent, then the retry. The
relocator's refusal of `kScan` is deliberate (it will not compensate a slot
it cannot vouch for); the question is whether the rollback's descent should
re-run, as BA-S14 re-runs a statement, before it gives up.
