# `FlushMaps` walks the shared region map unlatched (defect 1 closed at AW-S1b)

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

## Owner

AM-S3 owns writeback under sharing, and defect 2 with it. Verified still
open after AW-S1b.
