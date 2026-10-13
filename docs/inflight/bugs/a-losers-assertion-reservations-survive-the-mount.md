# A loser's assertion reservations survive the mount

**Found by reading, not reproduced**, while fixing
`a-fetch-refused-after-a-page-write-leaves-the-mutation-half-done.md`.
Verified at `afb21fbb` on `worktree-fix-fetch-refused-half-done`.

## What is wrong

`assertion.md` §7 says in-flight reservations at a crash "are rolled back by
normal transaction recovery via `ASSERT_ROLLBACK` compensation". Nothing does
it. Assertion replay applies `ASSERT_RESERVE` (`exec/assertion_replay.cpp:205`)
and puts an entry back only for an `ASSERT_ROLLBACK` record in the log
(`ReplayRollback`); mount undo walks the loser's undo chain, which holds no
assertion record, and writes none. A transaction that reserved and then lost
to a crash leaves its reserved entries and their group delta in place.

## What it costs

A wrong group total after the restart: the assertion counts rows that were
rolled back, so it refuses a write it should admit, or - with a negative
delta - admits one it should refuse. The second is a quiet wrong answer.

## The fix

Not scheduled; the assertion subsystem owns it, beside
`assertion-reservations-stranded-by-a-failed-settle.md`.
