# AG3 — what a fold over a fan-in costs, and what the routing rule is worth

`workplan-insert-spreading.md` §12d built the widening and stated its cost
without a number: **a fold over a fan-in ships every row it folds**, and a
widened shape therefore takes the fan-in only when no single core owns the
relation whole — where one does, the statement ships as text and is folded
on the owner. That rule was taken on the argument alone. This file is the
number, from `bench/fanin_fold_cost_probe.py`, a new driver that imports
its fixture from `bench/self_directed_stage_probe.py` rather than restating
it, so this cell and RS's S-a measure the same rig by construction.

**The headline: the routing rule is worth 122.6 µs per statement at 600
rows.** Folding at the session over every row (`Ffold`) costs that much
more at p50 than folding on the owner and shipping one row (`Ship`) — and
the gap is wire volume, so it grows with the row count while `Ship` does
not. Had a widened shape fanned in a single-owner relation instead of
shipping it, every such statement would have paid it.

**And the correctness result is the stronger half.** The fixture loads both
relations with the same multiset of `v`, so `SELECT SUM(v)` must answer
identically over all three routes. It does, **byte for byte**, 300 reps
each with one distinct reply per arm: `sum(v)\n179700` from a local walk,
from a statement shipped to the owner, and from a two-stage fan-in folded
at the session. The unit suite proved this in a loopback rig; this proves
it through a real ring, a real reactor and a real TCP client.

## 1. Provenance

| | |
|---|---|
| Date/time (UTC) | 2026-08-30, 07:33–07:35 |
| Worktree | `r4s` (`/home/cdkbs/ckdbs/.claude/worktrees/r4s`) |
| Engine measured | `eb03112` (`git describe --tags` → **`v2.2.1-141-geb03112`**), which is **byte-identical in engine terms to `3446666`**: the intervening commit touched only `docs/` and `bench/`. The binary itself was linked from `3446666`'s sources |
| Tree state | The driver this file names (`bench/fanin_fold_cost_probe.py`) was untracked at the moment of the run and is committed with this file. No engine source was dirty; `src/` and `include/` were clean throughout |
| Binary provenance | `/home/cdkbs/ag3_run/kds_server`, the same staged copy the read-surface run used; `sha256sum` = `0383bcbb11019ed3982565a3a393dc8ede4aede2e90a213e9d1f5fbc791b480a`. Copied out of `build-release/` and never touched again, per `.claude/agents/ck-tester.md` rule 5 |
| Device | `/home/cdkbs` on `/dev/root`, **ext4** (`df -T`) — a real block device, not tmpfs |
| Build type | `CMAKE_BUILD_TYPE=Release`, `-O3 -DNDEBUG`, `KDS_WITH_TLS=ON` |
| Host | AMD EPYC 9V74, **2 logical CPUs = 2 physical cores, 1 thread/core** (no SMT), 1 socket, 1 NUMA node, Hyper-V guest, 15 GiB RAM, Ubuntu 24.04.4 LTS, Linux 6.17.0-1022-azure |
| Host quiet | `uptime` load average **0.00** at the run; `pgrep cc1plus` empty. No build overlapped it — on a two-CPU host a build alongside a run measures the build |
| Server config | `cores = 2`, `placement = creating`, `range_size_ids = 512`, `durability = relaxed`, `peer_listeners = on` |
| Fixture | `spread` 600 rows round-robin from both cores' sessions → `split_relation_detail=4000:2@2` (two ranges, two owners); `twin` 600 rows from core 0 alone, never split. `placed spread=600 twin=600` |
| Reps | 300, **rep-interleaved** — one rep touches all five arms once (RD9(a)'s standing rule), so drift lands on every arm rather than on whichever ran last |
| Raw output | `bench/v2.6.0/archive/fold-cost-v2.2.1-141-geb03112/fold-cost.json` |

## 2. The arms

| arm | relation | core | statement | stages | what it exercises |
|---|---|---|---|---|---|
| `Lstar` | `twin` | 0 | `SELECT * FROM twin` | 0 | local walk, whole rows |
| `Lfold` | `twin` | 0 | `SELECT SUM(v) FROM twin` | 0 | local walk, folded locally |
| `Ship` | `twin` | 1 | `SELECT SUM(v) FROM twin` | 0 | **shipped as text**, folded on the owner, one-row reply |
| `Fstar` | `spread` | 0 | `SELECT * FROM spread` | 2 | fan-in, whole rows |
| `Ffold` | `spread` | 0 | `SELECT SUM(v) FROM spread` | 2 | **fan-in, folded at the session** — what AG3 added |

`Ship` is itself a check on §12d's routing rule: `twin` is unsplit and
core 0's, so a fold of it from core 1 must **not** open a stage. It did
not, and it answered — which is the rule working end to end rather than in
a fixture.

## 3. The numbers

300 ops per arm, **0 errors on every arm**, one distinct reply per arm.

| arm | mean µs | p0 | p25 | p50 | p90 | p95 | p99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `Lstar` | 128.0 | 116.1 | 121.5 | **123.0** | 132.3 | 135.3 | 151.2 | 830.0 |
| `Lfold` | 85.5 | 78.6 | 83.2 | **84.4** | 92.2 | 95.0 | 97.7 | 102.9 |
| `Ship` | 126.3 | 110.2 | 122.6 | **124.5** | 133.8 | 135.8 | 150.2 | 157.4 |
| `Fstar` | 331.9 | 293.0 | 321.8 | **328.0** | 339.2 | 345.1 | 394.5 | 866.7 |
| `Ffold` | 256.3 | 231.1 | 242.3 | **247.1** | 259.6 | 265.3 | 299.6 | 2063.9 |

| difference | p50 µs | what it says |
|---|---:|---|
| `Ffold − Ship` | **+122.6** | **the routing rule's price.** Fold at the session over every row, against fold on the owner and ship one |
| `Ffold − Lfold` | **+162.7** | what a two-stage fan-in costs a fold, against the same fold on a local walk |
| `Ffold − Fstar` | **−80.9** | *not* the fold's cost — see §4 |
| `Lfold − Lstar` | **−38.6** | the same effect with no wire at all, which is what identifies it |

## 4. The fold's own cost is **not** what the negative differences say

`Ffold − Fstar` and `Lfold − Lstar` are both negative: the aggregated
statement is *faster* than the star over the same rows, on the fan-in route
and on the local walk alike. **That is a reply-size effect, not a fold that
costs less than nothing.** A star reply over 600 rows is ~7 KB of text the
server formats and the client reads; a `SUM` reply is 13 bytes. The
local-walk pair isolates it: with no wire and no stage in either arm, the
same shape difference is worth −38.6 µs, so most of the −80.9 µs on the
fan-in route is the same formatting-and-reading saving.

**So this run does not price the fold's own CPU**, and does not claim to.
Separating it needs two arms with equal reply sizes and different per-row
fold work — `COUNT(*)` against `SUM(v)`, say, both one-row replies — which
is a cell this file does not contain. What the run does establish is the
comparison AG3's design turned on, which is between *routes*, not between
shapes:

- **Against the alternative route** (`Ship`), the session-side fold costs
  **+122.6 µs at 600 rows**, and the cost is wire volume: every row crosses
  the ring to be folded. Doubling the rows moves this number; it does not
  move `Ship`.
- **Against the same fold with no fan-in** (`Lfold`), it costs **+162.7 µs**
   — which is the fan-in itself, and is in line with `Fstar − Lstar` =
  +205.0 µs for the star pair over the same two stages.

## 5. What this validates, and what it bounds

**The routing rule was right, and now has a number.** §12d sends a widened
shape to the fan-in only when no single core owns the relation whole. Every
single-owner fold that would otherwise have been fanned in saves 122.6 µs
at this row count, and more at larger ones. This is measured on the arms
that exist — `Ship` and `Ffold` are different relations by necessity, since
a relation cannot be split and unsplit at once — so it is a comparison of
routes over the same rows, not of two runs of one route.

**And it bounds AG-M.** `aggregate.hpp` reserves `Merge` for exactly the
missing mechanism: a stage folding its own partition and shipping states,
the session merging them. That is the owner-side fold with the fan-in's
stage count — so `Ffold − Ship` = 122.6 µs is the right order for what
partial aggregates would recover on a two-owner relation of 600 rows,
which is the first real estimate this line has of what AG-M is worth. It
is an upper bound rather than a prediction: a merged fold still pays k
stage opens and k EOFs, which `Ship` pays once.

## 6. Versus PostgreSQL

**No counterpart concept exists, and none is claimed.** Every arm here is a
routing decision inside this engine's fan-in over a relation split across
its own core-owned page ranges — which core folds, and how many rows cross
a ring between reactors pinned one per core. PostgreSQL has no analogue to
a relation owned by one of several core-pinned execution units, so there is
no PostgreSQL twin of the question. The same reasoning
`bench/v2.5.0/results-r6b-cross-owner-cost-v2.4.0-28-g53f6aae.md` §10 gives
for the cross-owner line, and RS's §6 for the self-directed stage. Stated
rather than the section omitted.

## 7. What this run does not measure

- **The fold's own per-row CPU** (§4). Not separable in this rig, and
  reported as not measured rather than read off a negative difference.
- **Scaling in the row count.** Every number is at 600 rows and two owners.
  The claim that `Ffold − Ship` grows with rows is an argument from what
  the two routes put on the wire, not a swept curve.
- **k > 2.** `kds_server` refuses `cores 3` on this host (`CheckCoreCount`:
  two logical CPUs, reactors pinned one per core and never blocking). A
  three-owner relation would open three stages, and no cell here says what
  that costs.
- **`GROUP BY` and the wide folds.** Only `SUM(v)` is timed. A grouped fold
  returns more than one row and would move the reply-size term §4
  identifies; `COUNT(DISTINCT)` allocates per distinct value at the
  session. Both are reachable (the read-surface enumeration says so) and
  neither is priced.
- **Per-statement overhead A/B for AG3's diff generally.** Suspended by
  operator amendment since 2026-08-24.
- **Any comparison against a pre-AG3 build.** These shapes did not answer
  before `3446666`; there is no earlier number for `Ffold` to be measured
  against, which is why §3 carries no before-column.
