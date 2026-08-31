# Work order PD — retiring user-declared patterns

Drafted 2026-08-31 against `main` at `6b5b368` (`v2.2.1-142-g6b5b368`).

Operator direction, 2026-08-31: the user-declared pattern DDL is
withdrawn and will be re-designed later. A pattern is, for now, **a
fingerprint-identified case tracked by statistics** — one concept, and
the declaration path duplicated it. Remove the code now rather than
carry two models.

`docs/spec/create-pattern-user-defined-patterns-v1.md` is not deleted.
It is marked withdrawn and kept as the design record the re-design will
start from.

## What survives, established from source before anything is removed

The auto path does not depend on the declared one. §4.2: an
auto-registered pattern has no `sys.pattern_defs` row and prints as a
bare hex id. §7: *"declared and auto rows are found by the same
lookup — there is no 'declared pattern matcher'"*, and §3.2 is what
made the two hashes converge. So matching, trail recording and the
waystone tree run off `sys.patterns` and the fingerprint alone.

**This is the whole basis for the removal being safe.** Verify it holds
in code before PD3 deletes anything — the spec says it, and a spec
sentence is not a source read.

## What goes

- `sys.pattern_defs` (`kSysPatternDefsTable = 115`), its bootstrap
  creation and its four `sys.columns` rows
  (`kSysPatternDefsColumnOidBase = 120`)
- `CREATE PATTERN` / `DROP PATTERN` parse, dispatch and execution
- `src/exec/pattern_ddl.cpp` (527 lines), `include/kds/exec/pattern_ddl.hpp`
- `src/stats/pattern_defs.cpp` (265), `include/kds/stats/pattern_defs.hpp`
- `SHOW PATTERNS`' name column — every pattern prints as hex, which is
  today's auto behaviour

## What stays, and why — [PROPOSAL]

**`SysPatternRow::origin` and `flags` stay.** Two reasons. `origin` is a
`u8` at offset 37 *because* `dir_depth` ends the row there and a `u16`
would pad (`rows.hpp:550`); removing it changes the on-disk layout.
And the concept is not local to this feature — `SysCabinRow` mirrors it
with `kCabinOriginAuto`/`kCabinOriginUser` (`rows.hpp:728`). Every row
becomes `kOriginAuto` and `kPatternPinned` loses its only writer, but
retention is unbuilt so it had no reader either.

If the operator would rather the fields go, that is a format change and
a separate row; it is not free and PD does not assume it.

## PD0 — verify the auto path is independent

Confirm in code, not from §7: the fingerprint lookup, trail recording
and waystone attachment read `sys.patterns` and never `sys.pattern_defs`.
**If any of them joins to the defs relation, stop and report** — the
removal's whole safety argument fails and the scope has to change.

## PD1 — establish the blast radius of `kParam`

`ast.hpp:61` defines `kParam` as *a declared pattern's `$name`*, so it
is this feature's concept. But ten files reference it: `row_codec`,
`step_vm`, `aggregate`, `bound_cabin`, `step_compiler`, `chain_frame`,
`lexer`, `parser`, `fingerprint`, `pattern_ddl`.

Classify each before touching any. The comment at `ast.hpp:62-64` says
*"nothing ever binds one — a chain compiled from a pattern body exists
to be..."*, which reads as most of those sites being **defensive**
handling of a value kind that cannot occur rather than working code.
Confirm that per site.

**[PROPOSAL]** Remove `kParam` only from the sites that construct or
consume it meaningfully; leave the defensive arms, converted to
"unreachable" handling of the same shape. Reasoning: a value-kind enum
with a hole is worse than one with an unused member, the enum is
serialized in step descriptors (`kStepDescriptorVersion`), and
renumbering it for a cleanup is a wire change nobody asked for.

## PD2 — the two dependants that are not about patterns

**`varheap_sweep.cpp:113-117`** lists `kSysPatternDefsTable` in the
mount-time sweep H7 built for the `kNoTxnId` spill leak. Its comment
names *"`exec::LogChainInsert`'s two callers"*. After PD there is one.
Update the list, the comment and the argument — do not leave a sweep
whose stated justification names a relation that no longer exists.

**`relayout_planner.cpp:195`** branches on *"catalog relations
(`sys.pattern_defs`, `sys.assertions` — the two...)"*. That branch's
premise becomes a single relation. Re-read the branch and say whether
it still needs to exist.

## PD3 — remove

Bootstrap stops creating the relation. `kCatalogPagePatternDefs = 10`
becomes reserved-and-unused; `kSysPatternDefsTable = 115` and
`kSysPatternDefsColumnOidBase = 120` are **retired, not reused** —
`well_known.hpp:262`'s static_assert requires an oid to name exactly one
object for the life of a database, so a new relation goes past the
highest and never into this gap. Say so in a comment where the constants
were.

No migration is owed: §4 states no backward compatibility is owed
pre-release, the bootstrap format changes in place and existing data
directories are re-initialized.

## PD4 — mark the spec withdrawn

`create-pattern-user-defined-patterns-v1.md` gets a header stating it is
withdrawn 2026-08-31, that a pattern is currently a
fingerprint-identified case tracked by statistics, and that the file is
kept as the record the re-design starts from. Do not delete it.

`pattern-tracking-levels.md` — check whether it leans on declared
patterns and amend if so; not read for this draft.

## PD5 — what this changes elsewhere

**CB0-CB3 are withdrawn.** `instructions/v2.7.0/catalog-placement-buildout.md`
made `sys.pattern_defs`' var-heap the first exercise of CR1, chosen as
the smallest case with no peer reader. There is no such case after PD.

CR1 stays a ratified rule and becomes **unexercised**: the only
remaining catalog var-heap is `sys.assertions`, which already has its
page-at-a-time workaround (`exec::AssertionSpillPages`). CR1 applies to
the next catalog relation that gains a var-heap. Record that in the
ratification so a later reader does not look for a build that was never
performed.

CB4-CB9 are unaffected and become the whole of that order.

## What this order does not do

It does not re-design declared patterns, decide whether they return, or
touch `sys.patterns`' schema. It removes one of two overlapping models
so the remaining one is the only one.
