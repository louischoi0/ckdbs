# H9 — an errored page grow leaves a leaf chain out of ascending order

**Reproduced at `449804e`, not reproducible at `eeb88f47`.** Re-checked
2026-10-13 on `worktree-close-unreproduced-bugs`. The relation is a
**btree**, not a heap: the minimized case creates `t2` as
`CREATE TABLE t2 (...) BTREE`. The file name is kept so that existing
citations still resolve.

## Symptom

The sim's integrity sweep reports
`[chain-order] page 191: relation 't2': min_key 78 does not exceed a
predecessor page's max id 183` (`sim/integrity.cpp`). Before it come
`page-fail-grow` at op 1418 and three more armed page faults.

## Reproduction, at `449804e`

`ckdbs-sim --seed 20260826003 --ops 1500 --iterations 2 --mode clean
--profile uniform --faults io --fault-rate 40` fails as above. The
`zipfian` profile of the same seed fails the same way.

`--minimize` shrinks iteration 1 from 1500 ops to 638. Replayed at
`449804e`, the minimized case fails with predecessor max id 79. In that
case a `log-fail-write` fires at op 609, and the poisoned transaction is
rolled back at op 625, after the grow fault.

The case file `--minimize` writes cannot be replayed as written. Line 3 is
the second line of a two-line failure signature, and `--replay` refuses it
as an unknown tag. Deleting that line makes it replay.

## At `eeb88f47`

- **The seed's run stops early.** It ends at 48 ops with `fail_stops=2`.
  Since BC-S4 a failed log write fail-stops the log, so the run never
  reaches op 1418.
- **The minimized case does not replay.** It contains `create-pattern` ops,
  which are withdrawn. Stripping them makes the case pass at `449804e` too,
  so the stripped case proves nothing either way.
- **A sweep is clean, but short.** 160 seeds
  (`20261013000`-`20261013159`), each run under both profiles with the same
  flags, made 320 runs. All were `SIM ok`, with no `chain-order` finding.
  But every run ended within 432 ops, because log faults fail-stop it.

## What is known

**The mechanism is still unread.** The original case needs a failed log
write to be followed by more service: a transaction poisoned and then rolled
back after a page grow failed. Since BC-S4, a failed log write ends service
until a restart, so that sequence cannot happen in a live run.

Since the find, the btree write path has also changed. A leaf is now sorted
by placement (BD-S2), and a split is one `BTREE_SPLIT` record (BF).

None of this shows that the defect is gone, only that its known path is
closed.

## Why it matters

If a page opens below an id already on a predecessor page, the leaf chain
is out of order. Invariant 4's leaf order and every walk that relies on the
chain ascending would then be wrong.

## Closing

Closing needs one of two things:

- a reading of the grow-failure path at `eeb88f47` that shows the ascent
  holds, or
- a sim run that reaches a page grow fault without a log fault.

Today `--faults io` cannot do the second, because it arms both kinds of
fault. The seed is deliberately absent from `tests/testdata/sim_seeds.txt`.
