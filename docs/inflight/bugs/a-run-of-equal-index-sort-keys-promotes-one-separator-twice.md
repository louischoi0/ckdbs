# A run of equal index sort keys promotes one separator twice

**Reproduced** at `4860e96` on `at-s15-index-leaf-coverage` (a scratch
cell, not committed), found by AT-S15's `critics-developer` pass. One core;
nothing concurrent about it.

## What is wrong

An index's sort key is `(key, pk)`, and it is **not unique**: an `UPDATE`
that moves a covered column appends an entry with the same key and pk and
different covered bytes (`index_tree.hpp`, "Duplicates"). One hot row
therefore builds a run of equal sort keys, and `InsertEntry` puts each new
one at the front of the run.

`IndexLeafView::SplitInto` divides at the entry midpoint, so a run longer
than half a leaf is cut, and the right half's first sort key `K` is copied
up as the separator. Later entries with sort key `K` route to that right
half and pile up at its front; when it divides, its right half starts with
`K` again, and `IndexInternalView::InsertEntry` refuses the second `K`
(`AlreadyExists`, *"two subtrees cannot share a low key"*). By then the leaf
is divided and its new sibling linked, with no separator in the parent.

## Smallest reproduction

A tree-level cell on a 4-byte key with 8 covered bytes: insert
`(key 42, pk 1, covered i)` for `i = 0, 1, ...`. The insert at `i = 610`
fails `AlreadyExists`. **Since AT-S15** every later insert of sort key
`(42, 1)` is refused `TxnConflict` after five attempts: it routes to the
leaf whose new sibling now starts at that key, the coverage check sends it
back, and nothing ever repairs the parent. Before AT-S15 (by reading) it
was admitted into that leaf, out of chain order with its sibling.

SQL shape: an index on `owner` covering `balance`, and one account whose
balance is updated about 600 times. `CREATE INDEX`'s backfill, which
appends one entry per version, reaches the same state.

## What it costs

A **refusal that does not clear**: every write that would append an entry
for that `(key, pk)` fails - the row's `UPDATE`s that move a covered column
- first `AlreadyExists`, then `TxnConflict`, which calls itself retryable.
Reads are unaffected: separators remain lower bounds and a probe walks
forward. A second effect of the same runs: `FindExactDuplicate` looks only
in the leaf the descent lands on, so a byte-identical entry in the left
half of a straddling run is stored twice (reads absorb it through the pk
dedup).

## The fix

Needs a decision, not recorded anywhere: a split point that never cuts
inside a run of equal sort keys; the covered bytes (or a uniquifier) made
part of the sort order; or duplicate separators admitted. `index.md` IX4a's
midpoint is `[PROPOSED]`, and all three change what IX4a says. No work
order carries it.
