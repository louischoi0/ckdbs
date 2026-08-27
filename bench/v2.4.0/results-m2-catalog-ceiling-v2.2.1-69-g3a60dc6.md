# M2 — the catalog ceiling delta

`instructions/v2.4.0/range-foundation.md` §7 M2, read against **H4**
(§4 of the same file): *"the catalog ceiling cost is immaterial. Each new
bootstrap root shrinks the catalog overflow range by one page, roughly 68
`sys.columns` rows off the instance ceiling
(`include/kds/catalog/well_known.hpp:313-325`)."* Falsifier: *"M2's number
read against the widest schema the scenario benches declare. If a real
relation comes within an order of magnitude of the ceiling, the bump is
priced differently and C1 re-opens."*

**This is a source-read cell, not a run.** On the worktree
`v2.4.0-range-foundation-1` at `3a60dc6` (`git describe --tags` =
`v2.2.1-69-g3a60dc6`), nothing in this cell was built, started, or
executed — every number below is derived arithmetically from constants and
struct layouts read out of the tree at that commit, cited by `path:line`,
and tagged **source-read**. No number here is tagged `measured`.

## 1. Stamp

| | |
|---|---|
| Worktree | `v2.4.0-range-foundation-1` |
| Branch | `worktree-v2.4.0-range-foundation-1` |
| Commit | `3a60dc6` |
| `git describe --tags` | `v2.2.1-69-g3a60dc6` |
| Tree cleanliness | clean at read time (`git status` — nothing to commit); no source edited for this cell |
| What ran | nothing of the engine — no engine build, no server start, no archive directory, no git tag; the one compilation was §4's throwaway 40-line `constexpr` checker, discarded after use. Every figure below is read out of the source tree and computed by arithmetic, not measured |

## 2. The task's own citation, corrected

The work order's H4 cites `include/kds/catalog/well_known.hpp:313-325` for
the ~68-rows/page comment. **At `3a60dc6` those lines are the
`kAllCatalogPages` array (`:313-318`) and the head of the overflow-range
comment (`:320-325`)**, not the sentence — the comment carrying "~68"
and "~7,600" sits at `include/kds/catalog/well_known.hpp:338-339`, and the
constants it explains (`kCatalogOverflowFirst`, `kCatalogOverflowLimit`)
are at `:351` and `:356`. The task prompt's framing of the root page as
"page 2, `kCatalogPageColumns`" is also not what the source says:
`kCatalogPageColumns = 5` at `well_known.hpp:272`. Both are read from the
file itself below, not from the prompt, per this cell's own charge.

## 3. Before/after the overflow ceiling

`include/kds/catalog/well_known.hpp:349-350`'s ledger entry: *"Moved 15 ->
16 when sys.ranges claimed page 15 (RD1)"* — confirming the pre-RA2 value
of `kCatalogOverflowFirst` was **15**. At `3a60dc6`:

| constant | site | value |
|---|---|---|
| `kCatalogOverflowFirst` (post-RA2) | `well_known.hpp:351` | 16 |
| `kCatalogOverflowFirst` (pre-RA2, from the ledger's own words) | `well_known.hpp:349-350` | 15 |
| `kCatalogOverflowLimit` | `well_known.hpp:356` | 128 |
| `kCatalogPageRanges` (RD1's root, the page that moved the boundary) | `well_known.hpp:298` | 15 |
| `kCatalogPageColumns` (sys.columns' own root) | `well_known.hpp:272` | 5 |
| `kAllCatalogPages` (twelve catalog roots, the range's sharers) | `well_known.hpp:313-317` | 12 entries |

The overflow range is `[kCatalogOverflowFirst, kCatalogOverflowLimit)`:

- **Before RA2**: `[15, 128)` = **113 pages**.
- **After RA2**: `[16, 128)` = **112 pages**.
- **Delta**: **1 page**, exactly RA2's cost — `sys.ranges` claiming page 15
  as a fixed bootstrap root shrinks the shared overflow range by that one
  page (`well_known.hpp:343-350`'s ledger, restating the same 14→15 move
  AST03 made and the 15→16 move RD1 makes for the same reason).

## 4. Re-deriving "~68 rows per page" from the layout, not the comment

The comment at `well_known.hpp:338` asserts "~68" without showing the
arithmetic. Re-derived here from the row codec and the page layout, both
read at `3a60dc6`, and checked against a compiled program running the same
constants (not part of the engine build — a throwaway 40-line C++ file
evaluating the `constexpr`s below, discarded after use; it built and ran
nothing of the engine, so it does not violate this cell's no-build rule
any more than doing the arithmetic by hand would).

**4a. `SysColumnRow`'s on-disk size** (`include/kds/catalog/rows.hpp:172-228`):

| field | offset site | offset | width |
|---|---|---|---|
| `oid` (`Oid = std::uint64_t`, `include/kds/catalog/oid.hpp:13`) | `rows.hpp:220` | 0 | 8 |
| `rel_id` | `rows.hpp:221` | 8 | 8 |
| `pos` (`uint32_t`) | `rows.hpp:222` | 16 | 4 |
| `name` (`Name = std::array<char, kCatalogNameMax>`, `kCatalogNameMax = 64`, `oid.hpp:15-16`) | `rows.hpp:223` | 20 | 64 |
| `type_val` (`uint32_t`) | `rows.hpp:224` | 84 | 4 |
| `len` (`uint32_t`) | `rows.hpp:225` | 88 | 4 |
| `notnull` (`bool`) | `rows.hpp:226` | 92 | 1 |
| `cabin_policy` (`uint8_t`) | `rows.hpp:227` | 93 | 1 |
| **`kOnDiskSize`** | `rows.hpp:228` | — | **94 bytes** |

**4b. What a catalog row does *not* carry — the load-bearing correction.**
The task's framing suggested checking whether the row carries "the 20-byte
MVCC header per invariant 12 and the 8-byte Keystone word per invariant 5."
The MVCC header does apply (every heap tuple has one); **the Keystone word
does not**. `src/catalog/catalog.cpp:368-374`, on the function that inserts
every catalog row (`InsertRow<RowT>`, called for `SysColumnRow` from
`Catalog::InsertColumnRow` at `catalog.cpp:1163`, and for every other
`SysXxxRow` the same way): *"This is
`heap::ChainInsert` minus the one thing a catalog row does not have: a
**Keystone id**. … A catalog row is a fixed-offset struct with no such
word, so there is no key to order by and every page carries `min_key = 0`."*
The payload `InsertTuple()` receives is `row.Encode()` verbatim
(`catalog.cpp:384,392`) — 94 bytes for `SysColumnRow`, with no 8-byte
Keystone word prepended anywhere in that path. That word exists only for
**user** tuples (`include/kds/storage/keystone.hpp:38-39`: "Bytes the word
occupies at the front of every tuple payload" — of a *tuple*, and a catalog
row is deliberately not one, per the comment just quoted).

**4c. Per-page capacity** (`include/kds/storage/heap/heap_page.hpp`,
`include/kds/base/common.hpp:12,15`, `include/kds/storage/page_header.hpp:67,70`):

- `kPageSize = 8192` (`common.hpp:15`), `PageId = std::uint32_t`
  (`common.hpp:12`, 4 bytes).
- `kPageHeaderSize = 32` (`page_header.hpp:67`), `kPageBodyOffset =
  kPageHeaderSize` (`page_header.hpp:70`) — the common headered-page prefix
  every heap page carries.
- `kHeapHeaderOffset = storage::kPageBodyOffset = 32`
  (`heap_page.hpp:59`), `kHeaderSize = 16` (`heap_page.hpp:77`) — the
  heap-specific header on top of the common one.
- `kNextPageIdOffset = kPageSize - sizeof(PageId) = 8188`
  (`heap_page.hpp:166`) — the tail reservation for the chain link.
- A freshly created page's `lower`/`upper` are initialized to exactly
  `kHeapHeaderOffset + kHeaderSize` and `kNextPageIdOffset`
  (`src/storage/heap/heap_page.cpp:87,90`), so the space available to
  slots+tuples on an empty page is `8188 − (32 + 16) = 8140` bytes
  (`free_space()`, `heap_page.cpp:110-113`).
- Each `InsertTuple` charges `needed = kSlotOnDiskSize +
  kTupleHeaderOnDiskSize + payload_len` against that space
  (`heap_page.cpp:146`, `kSlotOnDiskSize = 5` at `heap_page.hpp:103`,
  `kTupleHeaderOnDiskSize = 20` at `heap_page.hpp:152`), and this is exact
  per row — `lower` grows by 5, `upper` shrinks by `20 + payload_len` on
  every successful insert (`heap_page.cpp:176-177`), so free space shrinks
  by precisely `needed` each time with no rounding or alignment slack.

**Per-`sys.columns`-row footprint**: `5 (slot) + 20 (tuple header) + 94
(SysColumnRow payload, no Keystone word) = 119 bytes`.

**4d. The result**: `floor(8140 / 119) = 68`, with `68 × 119 = 8092` and
`8140 − 8092 = 48` bytes left over — too little for a 69th row (`69 × 119 =
8211 > 8140`). **The comment's "~68" is exactly right: 68, not an
approximation that happens to round to 68.** The exactness is conditional
on 4b's finding — computing the same division with an (incorrectly)
included 8-byte Keystone word (`needed = 127`) gives `floor(8140/127) =
64`, which would *not* match the comment. The comment is correct because
catalog rows carry no Keystone word, not despite it; verifying that fact in
code (`catalog.cpp:368-374`) rather than assuming invariant 5 applies
uniformly to every stored tuple is what makes 68 reproducible from first
principles rather than merely repeated.

Independent corroboration, found while reading the scenario drivers for
§5 below: `tools/scenario0_stockmarket.py:157-159` and
`tools/scenario1_backtest.py:229-232` both independently state "`sys.columns`
held ~68 rows" from when the catalog's pages did not yet chain — the same
number, arrived at by whoever wrote those comments, presumably the same way.

## 5. The ceiling, in the source comment's own framing

The overflow range is shared by all **twelve** catalog chains
(`kAllCatalogPages`, `well_known.hpp:313-317`), so range × 68 is an upper
bound that assumes `sys.columns` alone consumes the entire range. That
assumption is generous in a way that is itself source-read: two of the
other chains grow with **every** `CREATE TABLE` — one `sys.objects` row
(`src/catalog/catalog.cpp:1339`) and one `sys.tables` row (`:1345`) per
relation — so in practice they are co-tenants of the same 112-page pool
(§7 prices that). `well_known.hpp:338-339`'s own framing computes the
ceiling as `range × 68`:

| reading | before RA2 (range 15..127, 113 pages) | after RA2 (range 16..127, 112 pages) | delta |
|---|---|---|---|
| range only (the comment's own framing, `well_known.hpp:338-339`) | 113 × 68 = **7,684** | 112 × 68 = **7,616** | **68 rows** (1 page) |

The comment's own stated figure, "~7,600 with the range starting at 16"
(`well_known.hpp:339-340`), matches the after-RA2 reading (7,616) rounded
to the nearest hundred; that is the reading carried forward below.

**The root page is deliberately not added.** A "range + root" reading
would need `sys.columns`' root (`kCatalogPageColumns = 5`,
`well_known.hpp:272`) empty, and it is not: the `kSysTables` bootstrap
array (`catalog.cpp:532-563`) writes no column rows for itself, but two
bootstrap relations sit **outside** that array and do —
`sys.pattern_defs` (5 columns, built `catalog.cpp:713-721`, inserted
`:764-770`) and `sys.assertions` (6 columns, `:816-826`, `:865-871`) — so
page 5 carries **11** of its 68 row slots from first boot
(`Catalog::Bootstrap()` runs on a fresh file only, `catalog.cpp:524`). A
root-inclusive ceiling would therefore be 112 × 68 + 57 = 7,673 after
RA2, not a clean +68; the range-only figure above is the honest bound and
the only one this cell carries forward.

## 6. The widest schema the scenario benches declare

`bench/docs/README.md:71` — *"The four `scenarioN_*.py` drivers are
workloads"* — names four; `tools/` holds five (`scenario0_stockmarket.py`
through `scenario4_cabinopt_days.py`). `bench/docs/README.md:78,97,150,214`
give `scenario0`/`scenario1`/`scenario2`/`scenario3` their own sections with
PostgreSQL twins and results files (`results-scenario1-vs-pg.md`,
`results-scenario2-freight.md`, `results-scenario3-library.md`) — these are
the four the phrase denotes. `scenario4_cabinopt_days.py` gets its own
section too (`README.md:380`) but sits outside "the four," so it is read
separately below rather than silently folded in.

**Every CREATE TABLE column, including the pk, lands a `sys.columns`
row.** `src/catalog/catalog.cpp:1354-1362`, `CreateTable`'s column loop,
iterates `schema.columns` with no skip for position 0: `InsertColumnRow`
runs once per declared column, pk included. Every scenario's own
`SCHEMA` dict writes the Keystone/pk column out explicitly (e.g.
`tools/scenario1_backtest.py:210-212`: *"Column 0 of every relation is the
Keystone primary key … written out in each column list anyway because
CREATE TABLE declares it; only INSERT omits it"*) — confirmed against the
catalog code path rather than accepted from the comment, and the two agree.

| scenario | relation | columns declared (pk included) | site |
|---|---|---|---|
| `scenario0_stockmarket.py` | accounts | 6 | `tools/scenario0_stockmarket.py:168-170` |
| | users | 5 | `:171-172` |
| | assets | 4 | `:173-174` |
| | trades | 7 | `:176-178` |
| | user_periodic_profit | 5 | `:179-181` |
| | **scenario total** | **27** | matches the driver's own count, `:157` |
| `scenario1_backtest.py` | exchanges | 4 | `tools/scenario1_backtest.py:235-236` |
| | symbols | 5 | `:237-239` |
| | sessions | 5 | `:240-242` |
| | daily_bars | 9 | `:244-247` |
| | **daily_stats** | **12** | `:252-256` |
| | models | 6 | `:257-259` |
| | model_results | 8 | `:260-263` |
| | **scenario total** | **49** | matches the driver's own count, `:229` |
| `scenario2_freight.py` | organizations | 11 | `tools/scenario2_freight.py:87-90` |
| | ships | 11 | `:91-94` |
| | operations | 9 | `:95-97` |
| | cargos | 10 | `:98-101` |
| | fees | 7 | `:102-104` |
| | recipes | 7 | `:105-107` |
| | freights | 8 | `:108-110` |
| | charges | 5 | `:111-113` |
| | **scenario total** | **68** | counted from `SCHEMA`; the driver states the same figure at `:83-84` |
| `scenario3_library.py` | users | 9 | `tools/scenario3_library.py:114-117` |
| | **books** | **10** | `:118-121` |
| | reservations | 7 | `:122-124` |
| | loans | 8 | `:125-128` |
| | **scenario total** | **34** | counted from `SCHEMA` |

**The widest single relation across the four is `daily_stats`
(`scenario1_backtest.py:252-256`) at 12 columns.** `organizations` and `ships`
(`scenario2_freight.py`) tie for second at 11.

**`scenario4_cabinopt_days.py`, read separately, is not wider.** Its
`COLUMNS` template (`tools/scenario4_cabinopt_days.py:102`) —
`"id int64, symbol varchar, qty int64, price int64"` — is 4 columns,
declared identically for all five relations it creates (`board_a`,
`board_b`, three `tape_*` sizes; table names built by `table_names()` at
`tools/scenario4_cabinopt_days.py:291-295`, `CREATE TABLE` issued per
relation at `:303-311`). Five relations × 4 columns = 20 columns for the whole scenario,
below every one of the four scenarios' own totals and far below the widest
single relation (12). Stated explicitly per the task's instruction: **the
fifth driver is narrower, not wider, so it changes nothing about the
per-relation ceiling comparison** — worth naming because it would have been
easy to assume "more drivers, more risk" without checking.

**A caveat that bears on how these totals should be read**: nothing
reclaims a catalog row. `tools/scenario1_backtest.py:229-232`: *"the
ceiling is thousands of columns for the whole instance rather than the ~68
it was … but nothing reclaims a catalog row, so it is a ceiling on columns
ever created, not on live ones."* Every scenario suffixes its relation names
per run precisely to avoid colliding on a shared data file, which means a
scenario run repeated N times with N suffixes spends its column total N
times over against the one instance-wide ceiling computed in §5 — the
ceiling is consumed cumulatively across every CREATE TABLE the instance
ever executes, not per snapshot of live schema.

## 7. The margin, as a ratio, against the 10× falsifier

Using the after-RA2, range-only ceiling from §5 (**7,616** — the reading
that matches the source comment's own framing):

| reading | numerator | denominator | ratio | vs. 10× threshold |
|---|---|---|---|---|
| falsifier's literal reading — widest single relation | 7,616 | 12 (`daily_stats`, `scenario1_backtest.py:252-256`) | **~635×** | two orders of magnitude clear |
| instance-wide reading — largest scenario's total column footprint | 7,616 | 68 (`scenario2_freight.py`'s eight-relation total, §6) | **exactly 112×** | one order of magnitude clear |

The 112× figure is worth reading as what it structurally is, not as a
coincidence: `7,616 = 112 pages × 68 rows/page`, so dividing by 68 (the
column count `scenario2_freight.py` happens to also total) returns the page
count itself. As a "how many runs fit" reading, though, 112 overstates it:
each `CREATE TABLE` also writes one `sys.objects` row (96 bytes,
`rows.hpp:43-44` → 121-byte footprint → 67/page) and one `sys.tables` row
(106 bytes, `rows.hpp:136-148` → 131 → 62/page) into the **same** shared
overflow pool (§5), so a scenario2-sized run costs
`68/68 + 8/67 + 8/62 ≈ 1.25` overflow pages and the pool absorbs
**~89** such runs, not 112. Still ~89× past the falsifier's 10×.

**RA2's own delta as a fraction of the ceiling**: 68 rows (§3's one page)
against a 7,616-row ceiling is **68 / 7,616 ≈ 0.89%** — RA2 costs under one percent of the instance's
column ceiling, an order of magnitude below even the widest scenario's own
share of it (68/7,616 ≈ 0.9% for RA2's one-page cost vs. daily_stats's
12/7,616 ≈ 0.16% for one relation's whole schema, or `scenario2_freight`'s
full eight-relation, 68-column total occupying 68/7,616 ≈ 0.89% — the same
share as RA2's own cost, since both happen to be 68 rows).

## 8. Verdict on H4

**H4 holds, by the falsifier as written.** *"If a real relation comes
within an order of magnitude of the ceiling, the bump is priced
differently and C1 re-opens."* Neither reading in §7 comes close to an
order of magnitude (10×) of the ceiling — the widest single relation the
four scenario benches declare sits at ~635× below it, and even the
instance-wide reading (the heaviest scenario's *entire* eight-relation
schema, all 68 columns of it, counted against the ceiling in one sum) sits
at exactly 112× below it. RA2's own cost — the one page, 68 rows, that
`sys.ranges` claiming page 15 removes from the shared overflow range — is
under 1% of the post-RA2 ceiling (§5, §7). C1 does
not re-open on this evidence.

## 9. Versus PostgreSQL

**No comparison, and none applies.** This cell measures an internal
capacity constant of ckdbs's own fixed-page catalog format — how many
`sys.columns` rows fit in a page range reserved by `kCatalogOverflowFirst`/
`kCatalogOverflowLimit` (`include/kds/catalog/well_known.hpp`). PostgreSQL's
`pg_attribute` has no equivalent fixed low-page reservation or per-relation
root-page scheme for CLA to compare against — its catalog is ordinary heap
storage with no analogous ceiling to price. `bench/docs/README.md`'s
per-driver entries (§6 of this file's citations) name PostgreSQL twins for
every *workload* driver; there is no twin for a catalog-format constant
because the constant is not a behavior PostgreSQL and ckdbs both implement
differently — it is a byte-layout fact with no PostgreSQL counterpart to
name. No task is proposed to build one, because there is nothing on the
PostgreSQL side this cell's question could be asked of.

## 10. Wait/latency decomposition

**Does not apply.** This cell computed a static row-count ceiling from
struct layouts and page-capacity constants; nothing here is a duration, a
round trip, or a wait of any kind, so there is no fsync/write/read/
client-socket/lock-wait breakdown to give one a share of. Per
`.claude/agents/ck-tester.md` rule 3's own escape clause: *"If the
measurement has no meaningful decomposition, say that it does not apply
rather than omitting the section silently."*

## 11. What this does not measure

- **Nothing was executed.** No server started, no `CREATE TABLE` run, no
  `sys.columns` row actually inserted in this session. Every number is
  arithmetic over constants and struct offsets read at `3a60dc6`
  (`v2.2.1-69-g3a60dc6` on `v2.4.0-range-foundation-1`), not a count taken
  from a live catalog.
- **Runtime behavior at or near the ceiling is unobserved.** What actually
  happens when the overflow range fills — `AllocateCatalogPage`
  (`src/catalog/catalog.cpp:352-363`) walks `[kCatalogOverflowFirst,
  kCatalogOverflowLimit)` (`:354`) and returns `Status::OutOfSpace` naming
  the exact range once every id in it is taken (`catalog.cpp:359-362`'s
  message) — whether that path is reachable in practice, what a caller sees, whether
  any path handles it gracefully, is not exercised here. §7's ratios say
  the ceiling is far away for every schema read, not that hitting it
  has been tried.
- **The shared-range bound's real-world consumption is not measured.**
  §5 establishes by source-read that `sys.objects` and `sys.tables` grow
  one row per `CREATE TABLE` into the same pool, and §7 prices a
  scenario2-sized run at ~1.25 overflow pages; what a real deployment's
  accumulated DDL history amounts to — including the chains this cell did
  not price (`sys.indexes`, `sys.fkeys`, `sys.assertions`,
  `sys.patterns`, `kAllCatalogPages` at `well_known.hpp:313-317`) — is
  not measured or estimated here.
- **No overhead A/B, per the 2026-08-24 operator amendment.** This cell
  carries no per-statement or per-mount overhead claim of any kind — it is
  not a timing measurement to begin with, so the suspension does not need
  invoking to explain an absence, but it is stated here so the absence
  reads as policy rather than oversight.
- **No archive.** Per the 2026-08-25 archiving rule
  (`.claude/agents/ck-tester.md` rule 1b), only a *scenario run* archives
  its raw driver JSON/logs under `bench/<version>/archive/`; this is a
  narrower, source-read cell with no driver output to archive, so
  `bench/v2.4.0/archive/` gains nothing from this cell.
- **No test suite was run.** This cell touches no engine code, so
  `.claude/agents/ck-tester.md`'s "run the suite before/after a code
  change" rule does not bind — nothing changed.
- **The row-set sweep (200/1K/10K) does not bind here in its usual
  sense.** Rule 9 asks every test and matrix to sweep row-set cardinality
  because most findings in this engine turn on distinguishing a fixed cost
  from a per-row one. The `sys.columns` ceiling is a function of *schema
  width* (columns declared per `CREATE TABLE`) and *DDL count* (how many
  `CREATE TABLE`/`ALTER`-adjacent calls have ever run), not of how many
  *data rows* a relation holds — `scenario4_cabinopt_days.py`'s own
  200/1,000/10,000-row "tape" sweep (`README.md:389`) is exactly this
  distinction: three data-row cardinalities of the *same* 4-column schema,
  which costs the same three `sys.columns` rows (one CREATE TABLE, one row
  per column) regardless of which tape size is loaded after. This is the
  "shape genuinely does not scale with rows" case rule 9 itself carves out;
  stated explicitly rather than silently skipped.

## 12. What this teaches about the engine

**Source-read**, at `3a60dc6` on `v2.4.0-range-foundation-1`:

- **The comment's "~68" is not a rounded estimate; it is the exact
  answer**, and it is exact specifically *because* catalog rows are a
  distinct, lighter tuple shape than user rows — `InsertRow<RowT>`
  (`catalog.cpp:368-374`) deliberately skips the Keystone word every
  ordinary heap tuple carries, on the grounds that a catalog row has no key
  to order pages by. This is a case where invariant 5 ("the Keystone
  column is exactly `id:40 | flags:8 | reserved:16`") does not apply
  uniformly to every stored tuple in the engine, and the distinction is
  load-bearing for this exact arithmetic: get it wrong (assume every tuple
  carries the word) and the derived rows/page is 64, not 68 — an 8-byte
  difference on a 119-byte row moving the answer by 4 rows and the ceiling
  by hundreds.
- **The format-epoch bump's catalog cost is genuinely small, and by a wide
  margin on every schema this repo actually exercises.** RA2's one page
  (68 rows, under 1% of the ceiling) sits two orders of
  magnitude below the widest relation's headroom and a clean order of
  magnitude below even the heaviest single scenario's entire column
  footprint counted at once. C1's necessity argument (whether the format
  epoch is required at all) has to rest on the mount-time readability
  point the work order frames it around, not on this cost — M1
  (`bench/v2.4.0/results-m1-mount-cost-v2.2.1-68-g7318e7e.md`) already
  reached the same conclusion from the mount-time angle; M2 reaches it
  independently from the catalog-capacity angle asked for here.
- **The real risk this ceiling names is cumulative, not per-relation.**
  §6's caveat — nothing reclaims a catalog row, so the ceiling bounds
  columns *ever created*, not *live* ones — means the number that matters
  operationally is not "how wide is one relation" but "how many
  `CREATE TABLE` statements will this instance's whole lifetime run,"
  which this cell cannot answer and no scenario driver measures either
  (each is written to run once per data file, per `tools/scenario1_backtest.py:190-193`'s
  "there is no DROP TABLE … use a scratch data file and delete it between
  runs"). H4's falsifier is about a *relation's* width against the
  ceiling and holds comfortably; a different, unasked question — an
  instance's *lifetime* DDL count against the same ceiling — is the one
  the "ever created, not live" framing actually warns about, and nothing
  in this milestone's scope computes it.
