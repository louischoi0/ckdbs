# Plan statistics (spec)

Status: **built since CC-S2** (`instructions/v3.2.0/workorder-cc-plan-statistics.md`),
**read by the join planner since CC-S3** (`exec::EstimateChain`). Related: `docs/spec/parser-v2.md` I12
(the planner and its contract), `include/kds/exec/join_order.hpp` (the cost
model that reads these facts), `include/kds/stats/statement_stats.hpp` (the
per-core part protocol adopted here).

---

## 1. What it is

The facts the join planner prices an order with, beyond the statement and
the catalog: **how many rows a relation holds**, and **how many rows a join
of a set of a statement's relations produced** the last times it ran. One
store for the instance, `stats::PlanStats`, **memory-resident** (CC-Q2):
nothing is logged, no statement writes a page of it, and the planner prices
data-free until its first publish. **A relation's prior, and nothing else
of it, outlives a clean shutdown since CY-S1** (§8): a crash or a restore
still empties the whole store.

**Advisory, and only to the order.** A fact here chooses which of a block's
legal orders runs; it never chooses a step's kind, never skips a step,
shortens a walk, declares a result empty or replaces a legality test. Every
order of a block returns the same rows (I12), so a wrong fact costs speed,
never an answer. Nothing outside the cost model reads it (PS9).

## 2. The facts

**PS1 - Relation prior**, keyed by `rel_oid`: an estimate of the relation's
rows, read by a steward step from the clustered tree's own pages:

- height 1 (the root is the leaf): the root's live slots, exact up to
  undecided versions;
- height 2 or more, **stratified by the root's children** (CC-Q6 (c), at
  CC's close): one descent under each child the root names - every child
  when it has at most `kPriorDescents`, else the rightmost and the rest
  spread evenly - multiplying the fan-outs below it, then reading the leaf
  it reaches for its live slots. The leaves are the children's products
  summed (the rightmost as measured and the rest as their sample's mean
  when the root names a sample); the rows are the leaves times the mean
  sampled leaf's live slots. Exact in leaves up to height 3 with few root
  children. The rightmost subtree stands apart because an append-built
  tree's is the one not full: four random descents, CC-S2's estimator, read
  a measured height-3 tree at 0.38x by averaging it in.

A budget spent before any leaf is read refuses the seed - the steward
retries the relation next step with a whole budget - rather than publish a
leaf priced at its row capacity (`LeafRowCapacity`) times
`kAssumedLeafFill`; that constant prices only a tree whose leaves no read
could reach. A budget spent after a leaf keeps what finished. It reads no key
domain. A page whose type or
`owner_oid` does not match discards the seed when it is the anchor or the
root, and the one descent it lies on below them.

**PS2 - Join-subset posterior** `card(T)`, keyed by `(fetch_id, n, the
block's relation oids in written order, T)`, `T` a set of written positions:
the rows the join of `T` produced under every conjunct over `T`. It is read
off a complete run of a planned block: after executed slot `k`, `T` is the
written positions of slots `0..k`, and `card(T)` is that slot's
`rows_matched`. A planned block is top-level, so its conjuncts sit on the
latest step they read, and `card(T)` depends on neither the order within
`T` nor the access kinds - one order's runs price another. A build's own
`build_rows` is not folded: it leaves out rows whose join key is NULL, so it
is not the same fact as `card({w})`. Which masks a run reports depends on
the order it ran in - a relation alone is reported only when it drives - so
the *presence* of a fact follows the plan and the configuration that chose
it; its *value* never does (PS10).

**PS3 - Decline fact**, per key and written position: an annotated build
that did not publish (`inner_builds == 0`) on a complete run with
`join_build_max_rows > 0`. Set, never averaged, and published at once. The
planner then prices no build there, so no run re-tests it: a fact no decline
has repeated for `kDeclineTtlNs` (60 s) lapses, and the build is priced -
and tried - again.

A run feeds no posterior unless it completed: a run a LIMIT or a stop cut
(`ExecStats::stopped`) and a refused run fold nothing. Every searched block -
a stopped run's, and one of more than `kMaxPosteriorRelations` (4)
relations, which keys no posterior - asks the steward for the priors the
snapshot lacks. A block the planner does not search (one relation, a chain
of tuple key descents, a subquery block, `join_order = written`) feeds
nothing.

## 3. Collection (CC-Q3)

**One part per core**, a fixed table under its own `LatchKind::kStatsRing`
latch, written only by its core at a planned statement's end and read only
by the steward - `statement_stats.hpp`'s protocol. A part holds, per key and
mask, the sum and count since the last merge, the decline bits, and the
oids it saw without a prior (the wanted set). A part half full of keys drops a new key's observation and counts it, so
its probes stay short; the steward empties every part each step. No allocation on the statement's path, no
cross-core write.

## 4. The steward

**PS4.** Core 0's `system` group, every `kPlanStatsTickNs` (1 s), one bounded step:

1. copy every part's entries out under its latch, one latch at a time, and
   sum them across the cores - so a posterior moves one step however many
   cores ran its statement;
2. fold each posterior: the first observation sets it, later ones move it
   by `alpha = 1/4`; past `kMaxMasterKeys` keys the least recently seen go,
   once a step;
3. seed wanted oids, and re-seed known ones round-robin, at most every
   `kReseedNs` (10 s) each, within `kSeedPageBudget` (16) page reads a step;
   a relation that no longer resolves loses its prior;
4. band every value (§5);
5. publish a new snapshot only when a band moved or a fact was added.

A seed reads pages through `PageStore::GetForRead` on core 0, like a
statement, and **holds one page at a time** - a page is let go before the
next is asked for - for `btree.hpp`'s descent rule: a split holds pages
bottom-up, and a reader holding a parent while asking for its child could
close a latch cycle with it. A descent re-reads the root for its own pick. A
cold page is a synchronous read on core 0's reactor, which is why the budget
is small. Internal pages are what every probe keeps resident; the leaves a
seed samples - one a descent - may be cold. A seed costs 2 pages plus one a
level below the root a descent, so a 16-page step seeds one or two
relations of height 2 or more, and a block's priors can take a few steps to
arrive. The catalog lookup that names a relation's pages is outside the page
budget (a memo hit for a relation a statement just used).

## 5. Bands and publication

**PS5.** A value `v` is published as its half-octave band `b = round(2 log2 v)`,
with Schmitt hysteresis: a published band moves only when `|2 log2 v - b| >=
0.75` - `v` about 30 % from the band's centre, so a value that sits near a
band's edge holds its band through churn of +-9 % or more. A decline fact
publishes at once.

The snapshot is immutable, sorted, held by `std::shared_ptr<const ...>`. The
steward swaps it under a mutex and bumps a 64-bit generation (release). A
compile pins it through its core's `PlanStatsSource`: one acquire load of
the generation, and the mutex and a reference copy only when the generation
moved - at most once a second. A pinned snapshot outlives a sliced walk's
yields by its reference.

## 6. Exposure

`ANALYZE` prints a searched block's choice on one line after the class
line: `planned order=written|planned|memo gen=<g> switch=<r> est=<id>:<rows>,...`
- whether an order other than the written one was taken, the snapshot
generation its prices read (0: none published), the chosen cost over the
written order's, and each executed step's estimated rows labelled by its
step id (its written position), in executed order.

`SHOW META`'s `plan_stats` block: `plan_stats_generation`, `_relations`,
`_posteriors`, `_part_drops`, `_seeds`, `_seed_pages`, `_publishes` and
`_steward_us_max`, the steward's longest step. `sys.relation_stats` gains `est_rows` (the published prior,
its band's centre) and `generation`; the planner never reads the view.

## 6a. The order memo (CC-S4)

**PS6.** Each core remembers the order a searched block of at most four
relations chose (`exec::PlanMemo`, one per dispatcher, no latch), keyed by
everything the search read besides the catalog facts it compiles from: the
block's key (fetch id, written relation oids), **the statement's literals**
(`arg_hash` - a literal can change a step's kind, a range's span or a
LIMIT's bound in an order the written chain does not show), the statistics
generation, the catalog's two stamps (`cache_built_at`, `catalog_version`),
and the statement's `join_build_max_rows` and `sort_max_rows`. A statement
with a `?` bound later is not memoized. A hit compiles the written order
and then the remembered one through the full ladder - two compiles, where
the search makes up to 24 - so it reuses no chain, literal or page id; a
remembered order that no longer compiles is searched again, and the
search's answer replaces it. Under a minted view (a DDL in flight) the memo
is not read. It is an exact cache of the search's answer for its key,
up to a collision of its two hashes (fetch id, literals), which serves
another statement's order - the same rows, as every order gives. A
statement whose literals vary between runs misses every time and pays the
search. `ANALYZE` prints `order=memo` for a hit, with `reordered=1` where
the remembered order moved; `SHOW META`'s `plan_memo` block counts this
core's `plan_memo_hits` and `plan_memo_misses`.

## 7. Rules

- **PS9 - one reader.** Only `exec::EstimateChain` reads a published fact
  for a plan, and it clamps every rows value at 1; a fact chooses among a
  block's orders and nothing else. The compile pins the snapshot only for
  a block it searches, after the tuple key chain's skip. The fold's wanted
  check, `SHOW META` and `sys.relation_stats` read the snapshot for
  themselves, never for a plan.
- **PS10 - no advisory input.** Waystone trails, `sys.patterns`,
  `sys.access_stats`, the optimizer signals, the Cabin store and index trees
  feed nothing here: `rows_matched` is the same whether a trail replayed, a
  Cabin served or an index probed, so no configuration of those changes a
  fact's value (invariant 8 in its byte-for-byte reading). Which facts exist
  follows the order the planner ran (§2).
- **PS11 - written positions.** Every mask and decline bit names written
  positions, the step ids CB-Q3 fixes; a planned compile reaches a chain
  only through the step id rewrite.
- **PS12 - off.** `join_order = written` compiles no planned block, so
  nothing is pinned or folded.

## 8. Across a clean shutdown (CY-S1)

`instructions/v3.2.0/workorder-cy-managed-statistics.md`, on the operator's
words of 2026-11-18 (`raft-marks-2026-11-18.md` §12): the statistics are
unlogged, and kept across a clean shutdown only.

**PS13 - what is kept.** The relation priors (PS1), each as its band. Not
the posteriors and not the decline facts: a shape's first complete run
measures the first again, and the second lapses in a minute. A prior alone
is enough for the two scenario shapes CY-S0 found a blind compile to cost
(`StatisticsPlannerTest.RelationPriorsAloneFixTheFilteredJoinsBlindOrder`
and `...PutTheSmallRelationFirstAndBuildTheLarge`).

**PS14 - where.** `sys.statistics` (oid 160), a catalog relation with a
fixed root at page 17 (superblock 36), **unlogged by CR6** (`crosscore.md`
CC13) as `sys.access_stats` is: its content is only a statistic. One
fixed-offset row a relation (`catalog::SysStatisticsRow`, 19 bytes: the
relation's oid, the band, the kind) and one closing row counting the
relation rows. A mount empties the chain, so the row's layout can change
with no migration: a row of another size reads as a chain that is not
whole.

**PS15 - who writes it, and when.** The shutdown tail alone
(`Expeditor::WriteRelationPriors`): after every core is joined and every
listener detached, before the final sync that carries the pages out. **No
statement, no steward step, no tick and no checkpoint of a running instance
touches the relation's pages**, so statistics cost a running workload no
page write and no log record (CY-R10). The one walk that visits every
catalog root, the delete-mark sweep (`Catalog::RetireDeleteMarks`, at the
mount and on core 0's tick), passes an unlogged relation by
(`catalog::IsUnloggedCatalogRelation`): such a relation holds no delete
mark, and its chain is no walk's to trust but its own guarded one.

**PS16 - when a mount believes it** `[the rule the storage class rests on]`.
Every other unlogged structure is discarded at every mount, because a mount
cannot tell a crash from a clean stop and an unlogged page written during a
run may name a page the map never kept (`restore.hpp`, B6a). This one is
kept on three facts together:

1. the rows were written by a shutdown, onto a chain nothing else writes;
2. the shutdown said it finished: one closing mark, and exactly as many
   relation rows as it counts, on a chain that read to its end over pages
   that are each its own by type and owner
   (`Catalog::TakeStatisticsAtMount`, one walk with the discard);
3. **the mount empties the chain whatever it found**, in the window of the
   advisory discard - after recovery, before the completion checkpoint that
   persists the emptying, before the listener binds. So the chain on the
   device is non-empty only from a shutdown's write to the next mount's
   completion checkpoint, an interval in which no statement runs.

A mount after a crash therefore finds an empty chain, or - if it crashed
before its own completion checkpoint - the previous clean stop's rows over
data no statement has touched since. A restore's copy is taken from a
running instance and holds an empty chain.

**Damage costs the statistic and never the mount.** A shutdown killed
between its page writes and its map write can leave the head linking to a
page the map does not hold; a write torn on the head has no logged image to
heal from. A link that does not reach one of the chain's own pages is cut
and what was read before it is not taken; a head that is unreadable or no
heap page is made again empty in place (`CreateOverTorn`, redo's heal); each
is logged. The mount is refused on this relation's account only where the
store itself fails. **The count is a count, not a checksum**: pages of two
different shutdowns combining into a chain that reads whole would need a
cut-off page's id handed back to the chain and a power loss between two of
its page writes, and would cost stale priors - an order, never a row.

**PS17 - what the mount does with them.** The priors are published as the
first generation before any statement compiles (`PlanStats::SeedPriors`),
each at the band it left at, so the first planned compile after a clean
restart is priced as the last one before it was. A carried prior is due a
seed whatever the clock reads, so the tree's own pages replace it as the
steward comes round, within its page budget (PS4); a relation that no
longer resolves loses it.

**What it does not do.** After a crash or a restore the planner is blind
until it observes, as before CY-S1. Nothing here gathers, locks or exports
a statistic, and no SQL statement reads the relation yet.
