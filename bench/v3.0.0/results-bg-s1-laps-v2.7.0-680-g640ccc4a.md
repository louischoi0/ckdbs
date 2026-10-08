# BG-S1 — the premise, measured: the hand's laps under a scan four times the pool

**Measured at `v2.7.0-680-g640ccc4a`** on `worktree-bg-s1-laps`.
- `640ccc4a` is `8e63ee70` (BG-S0 on main) plus BG-S1's one code change:
  `pool_batch_steps_background`, the tick's reclaim steps, beside the
  inline `pool_batch_steps`.
- The order is `instructions/v3.0.0/workorder-bg-scan-queue.md`, §3's BG-S1
  row.
- The cell is BE-S6's scan-resistance cell
  (`archive/be-close-v2.7.0-664-g2f08b71c/drivers/scanres.py`).
  - It counts the hand's laps.
  - It reads the `after` pass one lookup at a time, so which hot rows were
    lost is known by position.
- Raw output and drivers are in `archive/bg-s1-laps-v2.7.0-680-g640ccc4a/`.

## The run, and what bounds it

| | |
|---|---|
| Binary | `build-release` at `640ccc4a`, copied, sha256 `8a9e1fdf…a1bbc8` |
| Device | `/dev/root`, ext4 (`df -T` at run time) |
| Port | 15640 |
| Config | `buffer_pool_frames = 16384`, `cores = 1`, `durability = group` |
| Table | `big4`, 3,538,944 rows, about 65,500 pages, 4× the pool |
| Host load | loadavg 9.18 at start, 9.14 at end |
| Competing work | another session's `cmake --build` and `ctest -j8`, recorded in `sr2.log` |

**This file quotes counts only, never times.** The host was loaded by
another session's suite throughout. The counts this file rests on are
steps, slots, misses and hits, and they do not depend on load:
- The cell is single-core and the scan is one statement.
- Run 1 was made at `640ccc4a`'s parent tree with the counter under its
  pre-review name (`sr.log`). Run 2 was made at `640ccc4a` (`sr2.log`).
- The two runs gave **identical** steps, laps and scan misses in every
  variant.
- The `after` survivors differed by at most 38 rows (878 against 840).

**The denominator.** Laps are (`pool_batch_steps` +
`pool_batch_steps_background`) over `pool_slots`, both as deltas across
the scan. The slot array was already full when the scan began (16,384
slots before and after it), so no step was walked over a smaller array
than the one it is divided by.

## 1. The laps, against the hot frames' usage

`u` is what the order's §1.1 gives a hot leaf. A fault enters at 1, each
hit adds 1, and one warm pass followed by `before` gives `u` = 2; three
warm passes followed by `before` give `u` = 4. A frame of usage `u` is
reclaimed on the hand's `u + 1`-th pass.

| Hot rows | Warm passes | `u` | Inline steps | Tick steps | Laps | Hot rows lost | Kept |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,024 | 1 | 2 | 56,862 | 2,036 | **3.60** | 814 | 210 |
| 8,192 | 1 | 2 | 79,157 | 2,019 | **4.96** | 7,352 | 840 |
| 8,192 | 3 | 4 | 95,706 | 1,987 | **5.96** | 7,359 | 833 |
| 12,288 | 1 | 2 | 91,604 | 1,986 | **5.71** | 11,838 | 450 |

"Hot rows lost" counts rows whose `after` lookup missed at least once.
Leaf and internal misses together match BE-S6's: 7,446 and 8,259, against
its 7,510 and 8,243.

**The gate: passed.** In every variant the laps exceed `u + 1`, and the
hot set goes. That is §1.1's mechanism, not a contradiction of it, so BG
continues.

**The tick walks 2-3 % of the steps.** The rest are the inline batches.
Counting only `pool_batch_steps`, as BE did, under-reports the laps by that
much and no more.

**§1.1's "about eight laps" overestimates them.** It holds `C − H`
constant, but the hot frames are themselves reclaimed after `u + 1` passes.
From then on a lap frees the whole pool, not `C − H`, so:

    laps ≈ (u + 1) + (frames reclaimed − (u + 1)(C − H)) / C

"Frames reclaimed" is the measured `pool_reclaimed_inline` +
`pool_reclaimed_background` across the scan:

| Variant | Frames reclaimed | Predicted | Measured |
|---|---:|---:|---:|
| 1,024 hot, `u` = 2 | 52,462 | 3.39 | 3.60 |
| 8,192 hot, `u` = 2 | 57,202 | 4.99 | 4.96 |
| 8,192 hot, `u` = 4 | 55,125 | 5.86 | 5.96 |
| 12,288 hot, `u` = 2 | 62,218 | 6.05 | 5.71 |

The model is within 0.35 laps in every variant. The residue runs both ways
and is **not attributed**. Candidates are the slots the hand passes and
cannot take (internal pages read hot on every descent, frames the walk has
pinned), and a hot frame's usage differing from `u` by the scan's own hit
on it, but neither was measured.

The order's conclusion does not depend on the constant. Any lap count
above `u + 1` loses the set, and BG-R2 removes the laps rather than
changing their number.

## 2. The survivors: the scan's own last pages, not hot frames

The order asked about BE-S6's 682 one-pass survivors. This run kept 840
rows in that variant, by a different count: rows with no miss, where BE
counted 8,192 − 7,510 misses. **They are not hot frames that outlived the
laps.** By position in the warm order, which is pk order and the scan's
order:

| Variant | Kept | In the last tenth | In the ninth | Elsewhere |
|---|---:|---:|---:|---:|
| 1,024 × 1 | 210 | 102 | 102 | 6, of which row 0 is one |
| 8,192 × 1 | 840 | 816 | 23 | 1 (row 0) |
| 8,192 × 3 | 833 | 806 | 27 | 0 |
| 12,288 × 1 | 450 | 450 | 0 | 0 |

Each variant's survivors are one contiguous tail: rows 815+ of 1,024, and
7,348+ of 8,192. These are the leaves at the end of the table, which the
scan faulted last. They entered cold (`kScan`) and were still resident when
the scan ended.

**The 1,024-row variant checks the arithmetic.**
- Its `after` pass faulted only 814 frames, too few to disturb the tail.
- It kept 20.5 % of its rows.
- At scan end the pool held the last ~13,300 scan pages: 16,384, less the
  tick's free reserve, less internal pages. That is 20.5 % of the scan's
  64,848 faults.

**In the larger variants the `after` pass reclaims part of that tail
before it reaches it.** Those variants' `after` passes faulted 7,352 and
11,838 frames, taking cold scan frames, so fewer of the tail's rows are
left: 840 and 450, where 20 % would be 1,640 and 2,450.

**Every hot frame that existed before the scan was lost**, apart from one
row: row 0, the table's first leaf, survived in both one-pass 8,192-row
runs. It is reported as unexplained. One row in 8,192 is not a population
the BG-R2 queue is designed around.

## 3. The census at the tip (`8e63ee70`)

**`FetchHeat::kScan` producers: one.**
- `RunWalkStep`'s outermost walk (`src/exec/step_vm.cpp:1958-1971`),
  through `BtreeVisitLeafPage` and `ChainVisitOnePage`, via
  `PageStore::Fetch` (`page_store.hpp:214`).
- Nested inner walks stay `kRead`.

**`FetchHeat::kRing` producers: one.**
- `DevicePageStore::ScanRing` (`device_page_store.cpp:1128`).
- Its two consumers are the Cabin build (`cabin_optimizer_exec.cpp:140`)
  and the relayout survey (`relayout_planner.cpp:255`).

**Erasers: three.** Every one removes a frame through `ReleaseFrameLocked`
(`:918`), whose `frames_.erase` (`:920`) is the only one in the store.
- `ReleaseScanSlot` (`:1054`): the ring's rotation.
- `EvictClean` (`:2052`).
- `SweepLocked` (`:2838`). Its three callers are the inline batch (`:802`),
  the tick (`:1931`) and `EvictColdFrames` (`:2782`). `EvictColdFrames` has
  no caller in `src/` or `sim/`.

`clock_hand_` moves only in `SweepLocked` (`:2799`). BF has not landed:
only BF-S0 is on main, so its discard eraser is not here yet. BG-R1's
stale-entry rule is what makes it not matter.

**Pages a scan faults outside its own walk, warm and outside any queue:**
- **Var-heap values.** A spilled column read during the scan's row decode
  faults its var-heap page through `GetForRead`
  (`src/storage/varheap.cpp:396`), as `kWarm`. A scan over spilled columns
  therefore drives the hand with frames BG-R2 would not take first. This
  is BG-S0's review finding, confirmed.
- **Undo pages.** A row whose visible version is older than its tuple reads
  the undo chain through `GetForRead` (`src/txn/undo_log.cpp:189`), as
  `kWarm`. This matters for a scan running beside writers, which BE-S6's
  cell has none of.
- **The descent to the first leaf.** `BtreeSeekLeaf` and
  `BtreeLeftmostLeaf` read internal pages warm. That is correct: they are
  the hot set's own.

## What BG-S2 takes from this

- **The premise holds.** The laps are the loss, and in every variant they
  exceed `u + 1`.
- **The census adds two warm faulters a scan reaches**: var-heap values and
  undo pages. BG-S2 either queues them (they would need a heat of their own
  at the call sites) or states them as the queue's bound.
- **BG-S4's close measurement now has a lap counter to report.** Under
  BG-R2, the scan's laps should fall to near zero.
