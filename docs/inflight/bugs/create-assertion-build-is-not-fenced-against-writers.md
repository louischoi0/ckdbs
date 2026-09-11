# A `CREATE ASSERTION` build does not fence off a writer on another core

**Found** 2026-09-10 by the source read of AT-S5d (`workorder-at-m3-uniformity.md`)
on `m3-at`. **Not fixed.** Verified by that read at `e827360`, where the
build ran on the relation's owner and a write could arrive from any core,
and unchanged by AT-S5d, which moved the build to the session's core and
left the window where it was.

**Cost: a quiet wrong answer.** The assertion's aggregate understates its
group by the missed row for the life of the assertion, so later admissions
fit rows the constraint forbids, and `SHOW ASSERTIONS` reports
`enforcing=1` throughout.

## The wrong behaviour

`docs/spec/assertion.md` §8.1 rests the build on one core's event loop:
*"no write can interleave with the build"*. Since AT-S5 a write runs where
its session is, so a write on another core can. The build
(`exec::BuildAssertionCabin`, `src/exec/assertion_build.cpp`) scans the
relation under a check view and refuses `TxnConflict` for a row an in-flight
transaction wrote **on a page it has not passed yet**; it takes no relation
lock (`HandleAssertion` never calls `BorrowRelationForDdl`), and §8.1a's
membership protocol, which would reconcile a concurrent write, is not built.

So a writer on core B that

1. is admitted while the registry holds no directory for the relation
   (`AnyOn` false: nothing to check),
2. places its row on a page the build on core A has already scanned, and
3. reaches `ReserveInsert` before core A's `Adopt` (nothing to reserve)

commits a row that is in neither the build's scan nor the cabin. A writer
that reaches `ReserveInsert` *after* the adoption is admitted there instead
- AT-S5d's uncovered-assertion arm - so the window is the three steps above
all falling before `Adopt`.

**The same window on the other write paths**, which ask the registry once
per row and have no reservation-time re-check: an `UPDATE` that read
`AnyOn` false (`CommandDispatcher`'s update walk) and a `DELETE`
(`ReserveDelete`'s caller) move a row the cabin never hears of, and
`SortedFillEligible`'s `!AnyOn` admits a heap bulk fill that bypasses the
check entirely. Each is the same missing fence.

## Reproduction

Not reproduced; the read is the evidence. Two cores: core A runs
`CREATE ASSERTION one ON t GROUP BY (v) CHECK COUNT(*) <= 1` over a relation
large enough that the scan takes measurable time; core B inserts `(…, 7)`
into the scan's already-passed range during it. Afterwards a second
`INSERT` of `v = 7` is admitted.

## The fix, and whose it is

`CREATE ASSERTION` takes the relation `X` for its build, which a writer's
relation `IX` waits on - the shape AT-R15 rules for `CREATE INDEX` at
AT-S5e (D6). What it needs beyond D6's: the build is not transactional DDL
(`ddl-transactional.md` §5), so its `X` has no transaction to hold it, and an
`INSERT` takes its relation `IX` **after** its admission, where the fence
would have to stand before it. Both are decisions rather than patches, so
the shape is `workorder-at-m3-uniformity.md` AT-0 item 13.
