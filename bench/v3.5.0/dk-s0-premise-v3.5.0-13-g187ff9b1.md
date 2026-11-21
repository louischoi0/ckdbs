# DK-S0 premise: every write descent fetches its leaf twice, and a stale descent is refused in most multi-core insert cells

**Thesis.** On the census spike `worktree-dk-s0-spike` at `187ff9b1`
(`v3.5.0-13-g187ff9b1`; the engine at `2c2639df` plus thirteen counters,
never landed), **every write descent of both trees fetched its leaf twice**
(1.00 second fetches a write descent, in every write shape), so DK-Q2's bar
for D1 is met. **Restarts are rare but they reach the bound**: 0.005-0.039 %
of clustered descents at `cores` 2 and 4 on a shared relation, and in 4 of
the 6 multi-core cells of the second run and 1 of 6 in the first, one
descent exhausted all five attempts and was refused `TxnConflict`
(`refused_btree_descend` 4 times, `refused_index_descend` once). The
restarts cluster in each cell's first second, the window where the empty
relation's root grows a level under load (M3). DK-Q3's bar for D2 is met by
its second clause (any refusal in the standard cells), not its first (no
cell reached 0.1 % restarts).

| field | value |
|---|---|
| date executed | 2026-11-21, run 1 00:53:39-00:54:57 UTC, run 2 00:55-00:57 UTC |
| branch / commit | `worktree-dk-s0-spike` at `187ff9b1`, `git describe --tags` = `v3.5.0-13-g187ff9b1` |
| work order | `instructions/v3.2.0/workorder-dk-descent-low-key.md` §5, DK-S0 |
| build | Release, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, from `git archive 187ff9b1` |
| binary | a copy at `/home/louis/dk-s0-run/kds_server_spike`, sha256 `d3593a70d405025ce4b6121ff18cd6bf0b55dd814fb1a7927546eb78d081386c` |
| device | `/dev/nvme0n1p1`, ext4 (`df -T` at run time) |
| server | `durability = relaxed`, `buffer_pool_frames = 131072`, reactors on CPUs 4.., one fresh server per `cores`, port 15741 |
| driver | `tools/dk_s0_premise.py` (written for this stage) |
| host | **busy**: load 10-14, 7-8 `cc1plus` from other sessions' builds throughout both runs. The counts below are not timings, but how often a descent races a split depends on scheduling, so the restart rates are this host's under that load |
| prior number | none: no restart below the bound was counted before this file |

## 1. M1 - what a descent fetches, per statement (`cores = 1`)

50,000 rows, ids even, trees two levels deep (a root over leaves);
5,000 statements a shape; no error in any shape. Counts per statement.

| shape | bt descents | bt fetches | bt write descents | bt second fetches | bt sibling fetches | ix descents | ix fetches | ix second fetches | ix sibling fetches |
|---|---|---|---|---|---|---|---|---|---|
| `point` (tuple key read) | 1.00 | 2.00 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `insert1` (issued key) | 1.00 | 2.00 | 1.00 | **1.00** | 0 | 0 | 0 | 0 | 0 |
| `named` (named key mid-chain) | 1.00 | 2.00 | 1.00 | **1.00** | 1.00 | 0 | 0 | 0 | 0 |
| `ixprobe` (secondary probe + resolve) | 1.00 | 2.00 | 0 | 0 | 0 | 1.00 | 2.00 | 0 | 0 |
| `ixinsert` (both trees) | 1.00 | 2.00 | 1.00 | **1.00** | 0 | 1.00 | 2.00 | **1.00** | 0.74 |

"bt fetches" counts the descent's own `GetForRead` calls; the second fetch
is the exclusive `Get` of the same leaf after the shared one is released,
and the sibling fetch is the coverage check's read of the right neighbour.
**A write descent is three page fetches where two would do** on a two-level
tree (four with a mid-chain sibling), on every write shape: D1 removes one.
A named key mid-chain and a secondary insert also pay the sibling fetch
DM prices (`workorder-dm-leaf-upper-bound.md`).

## 2. M2 and M3 - restarts and refusals

Four writer sessions a core, 8 s a cell, every cell from an empty relation.
`named_ix` adds one reader a core doing point reads of random keys (lookup
misses). "restarts" is attempts after the first; a refusal is five failed
attempts. "first s" is restarts in the cell's first second. No client saw
an error in any cell. Each clustered refusal has one dispatcher structural
re-run (`structural_reruns`) beside it; the `refused_index_descend` of run 2
has none (the index descent's refusal does not call
`NoteStructuralRefusal`). What answered it is the index maintenance's own
retry (`src/exec/index_maintain.cpp:204-218`, CI-Q11 (b)): on `TxnConflict`
it re-reads the tree's root from the anchor slot and inserts again only if
the root moved, so **that refusal was a stale root by construction** - the
retry would have broken off otherwise and the client would have seen it.

| run | cores | cell | writes | bt descents | bt restarts (rate) | first s | ix descents | ix restarts | refusals |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1 | `ins1_shared` | 154,734 | 154,734 | 0 | 0 | 0 | 0 | 0 |
| 1 | 1 | `ins1_own` | 156,104 | 156,104 | 0 | 0 | 0 | 0 | 0 |
| 1 | 1 | `named_ix` | 107,823 | 131,712 | 0 | 0 | 107,823 | 0 | 0 |
| 1 | 2 | `ins1_shared` | 124,653 | 124,653 | 22 (0.018 %) | 3 | 0 | 0 | 0 |
| 1 | 2 | `ins1_own` | 135,809 | 135,809 | 0 | 0 | 0 | 0 | 0 |
| 1 | 2 | `named_ix` | 90,918 | 112,497 | 0 | 0 | 90,918 | 0 | 0 |
| 1 | 4 | `ins1_shared` | 83,354 | 83,355 | 26 (0.031 %) | 6 | 0 | 0 | **1 descend** |
| 1 | 4 | `ins1_own` | 82,842 | 82,842 | 0 | 0 | 0 | 0 | 0 |
| 1 | 4 | `named_ix` | 55,738 | 69,779 | 0 (1 lookup miss restart) | 0 | 55,738 | 0 | 0 |
| 2 | 2 | `ins1_shared` | 184,331 | 184,332 | 21 (0.011 %) | 7 | 0 | 0 | **1 descend** |
| 2 | 2 | `ins1_own` | 182,195 | 182,195 | 0 | 0 | 0 | 0 | 0 |
| 2 | 2 | `named_ix` | 114,759 | 144,019 | 0 | 0 | 114,760 | 4 | **1 index descend** |
| 2 | 4 | `ins1_shared` | 103,642 | 103,643 | 40 (0.039 %) | 11 | 0 | 0 | **1 descend** |
| 2 | 4 | `ins1_own` | 106,020 | 106,020 | 0 | 0 | 0 | 0 | 0 |
| 2 | 4 | `named_ix` | 69,449 | 86,649 | 4 (0.005 %) | 4 | 69,449 | 0 | **1 descend** |

How to read it.

- **A restart needs two writers on one relation**: every `ins1_own` cell,
  and every `cores = 1` cell, is zero.
- **Issued keys restart most.** Every `ins1_shared` writer descends to the
  rightmost leaf, which is the one being split; a descent routed there
  before the split's separator lands finds the key past it.
- **A refusal costs exactly five restarts, and they come at the start.** The
  refused cells' first-second counts (4-11) hold the refusal's five; the
  later restarts are single. That is the shape the stale root predicts
  (`btree.cpp`'s refusal text, M3): a descent from the root read before the
  tree grew a level fails every attempt alike, and here the relation grows
  from one leaf to two levels inside the first second. A split racing a
  descent fails once and the second attempt succeeds. The run does not
  separate the two causes per refusal: no counter says which root a failed
  attempt started from.
- **The secondary tree restarts too** (run 2, `cores = 2`, `named_ix`: four
  restarts and one refusal), so DK-Q5's "both trees" has a case.

## 3. M4 - not taken

M4 is a sampled profile of `point` and `insert1` attributed to
`FetchAndPin`, `AcquirePageLatch` and `UnpinFrame`. This host has no
sampling profiler (`perf` is not installed), so M4 was not taken. What
stands in for it: D1's price is measured directly at the close (DK-S4,
interleaved A/B), and DN-S0 (`bench/v3.4.18/dn-s0-premise-v3.4.18-9-g34958393.md`)
priced one clustered descent at 0.3-0.6 us of a resolved row. CW, which
needs M4 for D3, does not open with this stage.

## 4. What this decides

- **D1: go** (DK-Q2 (a)) - the second fetch is on every write descent.
- **D2: go** (DK-Q3 (a), its second clause) - five refusals in twelve
  multi-core cells; the first clause (0.1 % restarts) was not reached in
  any cell.
- **D3: handed to CW** (DK-Q1 (a)) with M1 and without M4; CW is not open,
  so D3 does not build in DK.

## 5. Cuts and budget

Both runs together took about 4 minutes, the spike's build about 4 more,
inside DK-S0's 20 minutes. Cut: M4 (no profiler); `cores` above 4 (eight
CPUs shared with other sessions' builds); separate per-refusal cause
attribution (no counter). The JSON of both runs is in
`bench/v3.5.0/archive/dk-s0-v3.5.0-13-g187ff9b1/`.

Reproduce: `python3 -I tools/dk_s0_premise.py --bin <spike copy> --workdir <dir on a block device> --out <json> [--cores 2,4]`.
