# Aggregation — Workplan

Work instructions, companion to `docs/feat-aggregate.md` (AG1–AG15). Tasks
`AG01`–`AG10`. Written as instructions: each task states what to build and
when it counts as done.

Execution rules:
- Do tasks in numeric order unless "needs" says otherwise.
- Each task ships with its listed tests in the same change.
- If a task touches an `[OPEN]` or `[PROPOSED]` item in the spec — build
  the proposed default, do not decide differently, and flag it.
- The chain-identity test (`AG02`) and the fingerprint-invariance test
  (`AG01`) are regression-mandatory from the moment they exist.
- **The step VM is off-limits.** AG1 is falsifiable by grep: a change to
  `step_vm.cpp` or to `AccessKind` in service of this workplan means the
  placement decision was violated, not that the task needed it.

Environment note: `include/kds/server/config_file.hpp` used `std::uint64_t`
with no `<cstdint>`. **Fixed 2026-08-06 in its own commit**, as this note
asked. It reproduced on gcc 13 and not on gcc 11.5, which supplies the
include transitively — the header owed it either way.

**Status: AG01–AG10 are built (2026-08-06).** One task was added, `AG11`,
because this workplan had no doc-sync step and the work resolves
`parser-v2.md` I14 — which CLAUDE.md's own closing rule requires be moved
out of Open Decisions by whoever closes it.

---

## AG01 — Grammar & AST

Lexer untouched (no reserved words, no token types — §2's fingerprint
argument depends on this). Parser: aggregate select items
(`COUNT/SUM/MIN/MAX`, `DISTINCT`, `COUNT(*)`), the `GROUP BY` clause after
WHERE, `AVG` → `Unsupported`, `HAVING` → `Unsupported`, subquery-aggregate
→ `Unsupported` (J2). AST: `AggFunc`, `SelectItem{is_aggregate, func,
star_arg, distinct, column, byte_offset}`, `SelectStmt::{agg_items,
group_by, aggregated()}`.

Function heads are recognized as *unqualified name from the set followed by
`(`* — one token of lookahead suffices if the head is first parsed as a
column name and the paren checked after. A non-aggregated statement's items
collapse back into `projection` once GROUP BY has or has not been seen, so
nothing downstream of a plain SELECT changes shape. Mind the
multi-relation `SELECT *` ambiguity check: while items sit in `agg_items`,
`projection.empty()` alone misreads every named list as a star.

*Done when:* every refusal row of spec §2 fails with its position; a column
named `count` or `group` still parses everywhere it did; the golden corpus
hashes identically (`kFingerprintVersion` unmoved), pinned by test.

## AG02 — Compile: `AggregateSpec` (needs AG01)

`StepChain` gains `std::optional<AggregateSpec>` per spec §4;
`StepChain::star()` becomes "projection empty and no aggregate". The
compiler resolves group keys and item refs (duplicate keys refused), fills
`type_val` per item, enforces AG5 and AG3's type rules, and labels
`column_names` (`count(*)`, `sum(distinct x)`).

*Done when:* the chain compiled with and without the fold is byte-identical
in steps, kinds, residuals and class over a corpus spanning every access
kind and a join; every compile-time refusal carries a position; compile
determinism (same statement + catalog → identical spec) holds.

## AG03 — The fold: `exec/aggregate.{hpp,cpp}` (needs AG02)

`Aggregator` per spec §5: `Accumulate(ChainFrame)`, `Finish(emit)`,
first-seen group order, reused key-encoding scratch, heterogeneous
hash-map probe, contiguous group storage, no-GROUP-BY fast path with no
map. NULL semantics per §3.1. SUM through `__builtin_add_overflow`;
overflow is a statement error carrying the aggregate's label. MIN/MAX
compare through `CompareValues` with the item's `type_val`.

*Done when:* an allocation-counting test shows zero allocations folding a
row into an existing group; §3.1's table is pinned including both empty-
input forms; a SUM crossing `INT64_MAX` fails and emits nothing; MIN/MAX
over `uint64` above `INT64_MAX` are exact; two executions emit identical
bytes.

## AG04 — DISTINCT (needs AG03)

Per-`(group, item)` observed-value sets over the key encoding, existing
only for items that declared the word. `Merge` (AG05) unions them.

*Done when:* `COUNT(DISTINCT)`/`SUM(DISTINCT)` are exact against
duplicated input; `MIN(DISTINCT x)` equals `MIN(x)`; a statement without
DISTINCT allocates no set.

## AG05 — `Merge` (AG-M) (needs AG03; AG04 for distinct union)

`Aggregator::Merge(Aggregator&&)` per spec §1: addition, comparison, set
union; left operand's group order preserved, unseen right groups appended.
Unused by any v1 statement path — the test is the consumer.

*Done when:* for a corpus of statements and a randomized two-way partition
of their input rows, partition-fold-merge equals one-pass fold, byte for
byte.

## AG06 — Dispatcher wiring (needs AG03)

The SELECT path: when the chain carries a spec, the sink body becomes
`Accumulate` and a completed execution emits through `Finish` into the
same one-line response format. Waystone recording/replay, access-shape
recording and the affinity check run **unchanged and unconditionally** —
the fold is downstream of all of them. Catalog-view aggregation refused
(AG12). ANALYZE still runs the identical parse/compile/execute.

*Done when:* dispatcher-level tests cover: global count on empty and
non-empty relations; GROUP BY over heap and btree relations; WHERE + GROUP
BY; a join + GROUP BY; an aggregated probe chain that records a trail on
its second execution and replays on its third with identical output
(five-way contract-test pattern); `sys.*` aggregation refused.

## AG07 — Bounds & config (needs AG03, AG04)

`aggregate_max_groups` and `aggregate_max_distinct` in `config_file` +
`kds.conf.sample`, proposed defaults from spec §6, plumbed to the
Aggregator. Exceeding either fails the statement naming the key.

*Done when:* both caps trip in tests with the key's name in the error and
zero rows emitted; defaults land as `[PROPOSED]` values in one place each.

## AG08 — Plan printer & ANALYZE line (needs AG02)

One `Aggregate` line: keys, items, DISTINCT flags, and — under ANALYZE —
the group count.

*Done when:* an ANALYZE of an aggregated statement prints the line; a
non-aggregated plan's output is byte-identical to before this workplan.

## AG09 — Contract suite (needs AG01–AG08)

`tests/aggregate_contract_test.cpp` collecting spec §9 items 1–9 in one
regression-mandatory file, in the `waystone_contract_test.cpp` style:
configurations compared byte for byte.

*Done when:* all nine pinned; the suite runs in the default test target.

## AG10 — Bench (needs AG06)

Extend `bench/` with a grouped-scan and a grouped-probe scenario; record
results as `bench/results-aggregate.md`, on a block device (the Cabin
bench's tmpfs lesson is a rule now). Ratify or amend the AG11 defaults
from the numbers.

*Done when:* results file exists with device noted; AG11 defaults either
ratified `[CONFIRMED]` in the spec or amended with the measurement cited.

## AG11 — Doc sync (added, not in the original workplan)

`parser-v2.md` I14 marked resolved and J2's two references corrected from
"blocked on I14" to AG8 (permanent for v1, not blocked); CLAUDE.md's Open
Decisions entry for I14 replaced with what was decided, a Core
Architecture entry for aggregation added, an **Aggregation** open-decisions
section added for what is genuinely still open, and the Documents list
extended; `docs/client-manual.md` given the grammar and its refusals, since
that is the client-visible surface; the two aggregate documents' filename
cross-references fixed — the spec cited `docs/aggregate-workplan.md` and
this file cited `docs/aggregate.md`, and neither exists.

*Done when:* no document refers to I14 as open, and nothing cites a
filename that is not on disk.
