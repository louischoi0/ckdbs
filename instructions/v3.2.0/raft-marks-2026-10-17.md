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
| **Mark** | **BZ opened.** BZ-Q1 **(b)**, a `sys.columns` hidden flag - CLA's proposal revised from (a) against the tree before adoption (the order's §4 says why); BZ-Q2 **(a)**, a heap FK parent refused; BZ-Q3 **(a)**, a heap's last declared column not dropped (raised by BZ-S1's review). All **adopted on the operator's standing go-ahead of 2026-10-17 (go-ahead-achieving-milestone)**, not marked by the operator item by item |
| **Does not settle** | push, merge to `main`, tag, version |
| **Recorded at** | this file; the order's header, §4 and §7; `index.md`'s BZ row |

## 3. CA written and opened

| | |
|---|---|
| **Word** | *"Things are now reasonably in order; this is the stage of optimizing performance from statistics. First, for frequently used query patterns, create index pages bound to the values queried most often; and where a query reads several tables, as a join does, add a concept that stores every lookup key ref."* (W0), then *"The aux structures the optimizer manages must be advisory, so their role is to help from the background while putting as little strain on transactions as possible. OLTP performance comes first; optimizing comes after it. So choose the structures and the design that help OLTP processing from the background."* (W1), then *"And now we open a real milestone, not a CN."* (W2) |
| **Mark** | **Every structure the optimizer manages is advisory** (invariants 8 and 9), **puts as little strain on transactions as possible** (CA-R2 reads it as one latch-free note and one latch-free probe), and **is built in the background**, with OLTP performance ranked ahead of optimization (W1). The design is CLA's choice, as W1 asks: `workorder-ca-background-advisory-optimizer.md` §0, §2. **CA is opened as a milestone, not a concept note** (W2). CA-S0, the premise measurement, needs no mark |
| **Does not settle** | CA-Q1..Q6 (`workorder-ca-background-advisory-optimizer.md` §4); no tag, no version |
| **Recorded at** | this file; `workorder-ca-background-advisory-optimizer.md` header; `index.md`'s row for it and this file's row |

## 4. CA made exploratory; CLA's proposals adopted on the standing go-ahead

| | |
|---|---|
| **Word** | *"For now treat this milestone as an exploratory one: apply several possibilities, find in the end a way that improves performance in all of scenarios 1, 2, 3 and 4, and report it."* (W3), with `/go-ahead-achieving-milestone` |
| **Mark** | **CA is exploratory** (`workorder-ca-background-advisory-optimizer.md` §5a): an option lands only if it improves `tools/scenario1..4` together. **Adopted on the operator's standing go-ahead of 2026-10-17 (go-ahead-achieving-milestone), not as the operator's own marks**: CA-Q1 **revised to (c)** before adoption - scenario 4's `on` arm is the controller (§4); CA-Q2 (a), CA-Q3 (b), CA-Q4 (b) (follows Q1 (c)), CA-Q5 (a), CA-Q6 (a), each as proposed and each subject to the §5a test |
| **Does not settle** | The cabin controller's deletion (W1's reading of it stands for a later order); push, merge, tag, version |
| **Recorded at** | this file; the work order's header, §4 and §5a; `index.md`'s CA row |
