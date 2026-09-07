# The free map has no concurrency protocol (defect 1 closed at AW-S1b; defects 2 and 3 open)

> **Re-titled 2026-09-07.** This file opened as a finding about `FlushMaps`
> and is really about `map_regions_`: §3 is the one that matters, and it is
> a write racing a read rather than a walk racing a write.

**Found** 2026-09-07 by the `critics-developer` pass on AW-a (finding C),
verified at `8eabbc3` on `worktree-aw-m1-close`. Two defects in one
function. **Defect 1 is closed** — AW-S1b deleted the lease and with it the
guard and the private map copy the guard protected; see below for why the
deletion is a fix rather than a removal of the check. **Defect 2 is open.**

## 1. The lease guard is vacuous on a shared store — CLOSED at AW-S1b

`src/storage/device_page_store.cpp:366`:

```cpp
if (lease_ != nullptr) {
    for (auto& [region, pages] : map_regions_) pages.dirty = false;
    return Status::OK();
}
```

The arm exists so a peer never publishes its own free-map copy: "publishing
this core's copy would write back the map as it stood when this store
opened — reverting every allocation and every extent reservation core 0 has
made since". Under AM-S2 step 3 a peer *borrows core 0's store*, whose
`lease_` is null (`core_runtime.cpp:348` guards `SetCoreOwnership` on
`owned_store_ != nullptr`), so **every core takes the else branch and runs
the map writeback**. This was exactly the defect AW-a fixed one function
away, and it was hidden the same way: nothing distinguishes the cores once
the lease is gone.

**Why the deletion closes it.** The arm existed because a peer's map copy
was taken at that peer's mount and went stale; publishing it would have
reverted every allocation core 0 had made since. AW-S1b removed the private
copy along with the lease, so the writeback every core now runs writes the
one live map — there is nothing stale to publish. What is left is
*duplicate* work (N checkpointers each flushing the same map), which is
AM-S3's writeback-under-sharing decision and not a correctness defect.

**It also made a claim beside it false.** The comment there argued the arm
was equivalent to `MayWrite`'s refusal. It was not, since AW-a: region 0's
map pages are ids 1 and 2, below the 128-page system boundary, so
`MayWrite` refuses a peer those pages while `FlushMaps` writes them
straight to the device through `device_.WritePage`, which consults no
predicate at all. That asymmetry survives the deletion and is stated in the
function's own comment now.

## 2. The region walk takes no structure latch — OPEN

The same loop, and the writeback below it, iterate `map_regions_` and read
`pages.free_map` **without the structure latch**, while peers allocate
under it (`:336`, `EnsureRegionResident`). On a shared store that is a
`std::map` iterated during another core's insertion — undefined behaviour,
not a stale read — and a page checksummed over bytes another core is
changing.

This is the same class as AM-S2's frame-table work (`frames_` got a
structure latch; `map_regions_` did not), and the free map is the one
structure a peer both reads and grows.

## Why defect 2 is not fixed here

Taking the structure latch around the walk is not the one-line change it
looks like: `FlushMaps` calls `device_.WritePage` inside the loop, and
AM-S2's whole discipline is that device work runs *outside* the hold. The
fix is the copy-then-write shape the frame table already uses, which is the
same work as deciding **who runs the map writeback when one store serves
every core** — today N checkpointers each would.

`wal.md`'s fuzzy-checkpoint sentence carries the same open question for the
frame table (AW-S1 corrected the prose and left the behaviour to AM-S3);
this is the free-map half of it.

## 3. Defect 2 is wider than this file said — OPEN, and it is the worse half

**Re-surveyed 2026-09-07 at `6fae9ae`** on `worktree-am-s4d-s3-close`,
sizing AM-S3. §2 above names `FlushMaps`' walk. That is one reader among
several, and it is not the dangerous one: **`map_regions_` is *mutated*
outside the structure latch, on the ordinary allocation path.**

`CreateNewUnpinned` (`device_page_store.cpp:1010-1020`) loops:

```cpp
{ LatchGuard alloc(structure_latch());
  claimed = ClaimNextFreeIdLocked(&missing_region); }   // iterates map_regions_
if (page_id != kInvalidPageId) break;
if (auto region = EnsureRegionResident(missing_region); ...)  // <- emplaces, NO hold
```

The hold is dropped deliberately — `EnsureRegionResident` reads the device
and may grow the file, which AM-S2's discipline forbids under the latch —
and `EnsureRegionResident` ends in `map_regions_.emplace` (`:199`), a
`std::map` insertion that rebalances the tree. Concurrently, on another
core:

- `ClaimNextFreeIdLocked` iterates `map_regions_` **under** the hold, which
  does not help: the writer is not taking it.
- `IsAllocated` (`:399`) and `IsHeaderless` (`:270`) reach
  `free_map_bytes_for` / `headerless_map_bytes_for` → `FindRegion` →
  `map_regions_.find`, **unlatched**, and `IsAllocated` sits on the fault
  path, the writeback path and the WAL gate by its own comment.
- `CreateAtUnpinned` (`:956`) calls `EnsureRegionResident` outside its hold
  too, and `CreateNewHeaderlessUnpinned` (`:298-303`) calls it twice, with
  a comment saying it is deliberately before the hold.

So this is a `std::map::find` racing a `std::map::insert` — undefined
behaviour, not a stale read, and the same shape AM-S2 step 3c-ii measured
as *"a segfault, not a slow path"* for the frame table (two inserters plus
two readers segfaulted in 3 of 5 runs). It is reachable in production at
`cores > 1` whenever two cores allocate and one runs off the end of the
last resident region.

**`EnsureRegionResident`'s own comment asserts the opposite** (`:154-158`,
written at AW-S1b): *"every caller reaches this map under the structure
latch, so the unsynchronised read that arm avoided cannot arise"*. Three
of its four call sites reach it with the hold **not** held, and two of them
say so in their own comments one line away. That claim is what has to be
struck.

**Why it is not a one-line fix, same as §2.** The latch may not span the
device read, so the shape is `FetchPinned`'s: resolve outside, re-take,
re-check, and let the loser of a race find the region already present —
which `EnsureRegionResident`'s existing double-check at `:150`/`:161`
already half is. What it needs is for the *emplace* and the *find* to be
inside a hold, with the device work between them.

## Owner

AM-S3 owns writeback under sharing, defect 2, and §3. Verified still open
after AW-S1b and re-surveyed after AM-S4(d).
