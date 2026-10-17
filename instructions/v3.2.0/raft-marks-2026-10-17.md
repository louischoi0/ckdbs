# Ratification marks — 2026-10-17

The operator's words of 2026-10-17, recorded by CLA. Each is recorded
**in English**, translated from the Korean it was typed in.

## 1. The heap redefined, BZ written

| | |
|---|---|
| **Word** | *"Now the heap table SUS will be lifted. But the heap table concept is redefined: every optimization behaviour for heap tables is discarded, and it simply means a table with no identity column defined - pages go in insertion order or arbitrary order - but instead of a linear search through the tail, it is managed by a btree keyed on the time a row was most recently added."* Then four answers to CLA's questions: a hidden Keystone; the btree key is the issued id, which is insertion order; the heap chain and tail append are discarded; superblock bump, older volumes refused. Then: *"And the behaviour must not carry the constraint that a key that does not exist but has a low id cannot be inserted because of the high key mark, even with a hidden Keystone."* Then *"$!a"* twice: the hidden key is not nameable from SQL, and the sorted fill and relayout Part I are deleted with the chain. Then: *"Write the work order first."* |
| **Mark** | **BZ written** (`workorder-bz-heap-hidden-key.md`, W0-W5), SUS-1 to be lifted by BZ-S1. the order's review cut its questions to two |
| **Does not settle** | BZ-Q1, BZ-Q2 (§2 below); no tag, no version |
| **Recorded at** | this file; `workorder-bz-heap-hidden-key.md`; `index.md`'s BZ row and this file's row |

## 2. BZ opened on the standing go-ahead

| | |
|---|---|
| **Word** | `/go-ahead-achieving-milestone` - *"go ahead until achieving milestone, I will follow CLA proposal if decision needed, keep going in this session."* |
| **Mark** | **BZ opened.** BZ-Q1 **(b)**, a `sys.columns` hidden flag - CLA's proposal revised from (a) against the tree before adoption (the order's §4 says why); BZ-Q2 **(a)**, a heap FK parent refused. Both **adopted on the operator's standing go-ahead of 2026-10-17 (go-ahead-achieving-milestone)**, not marked by the operator item by item |
| **Does not settle** | push, merge to `main`, tag, version |
| **Recorded at** | this file; the order's header, §4 and §7; `index.md`'s BZ row |
