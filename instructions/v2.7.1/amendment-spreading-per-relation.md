# Amendment — insert spreading is a per-relation option, default off

Operator, 2026-08-31, in full:

> spreading insert is a builtin option for a relation (user can decide),
> default = OFF, ID is identity and sequence if turned off, otherwise just
> identity but not monotonic and sequential.

Recorded on the `xf` worktree at `1beda80`. **This reverses DA1**
(`instructions/v2.7.0/ratification-da.md`), which armed `range_size_ids`
at 65,536 so that "a range opens on *workload* rather than on someone
configuring it".

## What it says, in three parts

**1. The switch moves from the instance to the relation.** Whether a
relation's inserts spread across cores is declared per relation by the
user, not by an instance-wide config key.

**2. The default is off.** A relation that says nothing does not spread.

**3. The id promise is the switch's other half, and it is the reason the
switch exists.** Spreading **off**, the pk is an identity **and a
sequence** — monotonic and gapless in issue order. Spreading **on**, the
pk is an identity and nothing more: not monotonic, not sequential.

## What is now true in the tree, and what is not

**Built** (`1beda80`+): `Expeditor::Config::range_size_ids` ships
`kRangeSizeOff`. The default is off, today, and an operator who wants the
DA1 engine sets `range_size_ids = 65536` explicitly.
`ExpeditorConfigTest` pins the new value — it is DA1's own guard against a
silent default move, and it moved because somebody said so.

**Not built, and neither is invented here:**

- **The per-relation flag.** `SysTableRow` has no spare byte: every offset
  is a fixed on-disk constant and `Decode` refuses any size but the exact
  one, so the flag is a **format-version event** — superblock 15 → 16, the
  `anchor_page_id` precedent (`catalog/rows.hpp`).
- **The syntax to declare it.** `WITH (...)` table options are **V11**,
  listed open and unbuilt in `docs/spec/parser-v2.md`. The engine's own
  rule is that nothing is reserved lightly, so no keyword is chosen here.

Until both land, "no relation has asked" is every relation, which is why
the instance default carries the amendment exactly.

## The distinction the amendment forces, which the engine had conflated

`range_size_ids` spelled two facts as one — *whether* a relation spreads
and *how big* a range is. D6 took range = lease grant deliberately
(`workplan-range-directory.md` §11), and that part stands. What the
amendment separates is the first from the second:

| fact | before | after |
|---|---|---|
| does this relation spread | `range_size_ids != 0`, instance-wide | the relation's declared option, default off |
| how big is a range / grant | `range_size_ids`, default 65,536 (DA1's sweep) | `range_size_ids`, **a size**, no longer a default |

`kRangeSizeIdsDefault` keeps its name and its value: DA1's sweep is still
the derivation of 65,536, and a second name for the same quantity is what
the engine's no-parallel-knobs rule forbids. It is simply no longer
answering "whether".

## What it changes about claims already in the tree

- **`sys.tables.key_order` does not become the declaration.** It is an
  *observation* — whether an id has ever landed out of order — and stays
  one. The amendment adds a second, different fact: what the relation
  **promises**. Conflating them would make a promise derivable from data,
  which is exactly what it must not be.
- **Every DA1-era measurement is now a number for a configuration**, not
  for a default: `bench/v2.7.0/results-ratification-da-*` and the
  scenario-2-at-1/2/4/8-cores cells, including the 0.72× figure, were
  taken with spreading armed. They are not retracted and they are not the
  shipped engine's behaviour.
- **The `RefuseAuxiliaryOnSplitRelation` trap stops being reachable by
  default** and is not fixed by that. `known-gaps.md` carries the
  narrowing; work order SA narrows the gate itself.
- **The second DA1 trap is untouched**: no test exercises a spreading
  `Expeditor` end to end, and an operator who arms it runs precisely the
  configuration nothing covers.

## What SA inherits

Work order SA (`auxiliaries under split and 2PC`) is written against a
world where relations split. That premise is now opt-in rather than
ambient, which changes SA's **urgency** and none of its **content**: every
gate SA narrows is still reachable the moment a relation asks to spread,
and SA-R2's observation that "today the narrowed gate admits every indexed
relation" holds unchanged. No SA task is invalidated; SA-T2's gate rewrite
should cite this amendment where it says why a relation is or is not
splittable.
