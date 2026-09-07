# `FlushMaps` still keys on the lease, and walks the shared region map unlatched

**Found** 2026-09-07 by the `critics-developer` pass on AW-a (finding C),
verified at `8eabbc3` on `worktree-aw-m1-close`. **Not fixed.** Two
defects in one function, one of them the same shape AW-a just repaired in
`MayWrite` and one of them worse.

## 1. The lease guard is vacuous on a shared store

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
the map writeback**. This is exactly the defect AW-a fixed one function
away, and it was hidden the same way: nothing distinguishes the cores once
the lease is gone.

**It also makes a claim beside it false.** The comment at `:351-356` argues
the arm is equivalent to `MayWrite`'s refusal. It is not, since AW-a:
region 0's map pages are ids 1 and 2, below the 128-page system boundary,
so `MayWrite` now refuses a peer those pages while `FlushMaps` writes them
straight to the device through `device_.WritePage`, which consults no
predicate at all.

## 2. The region walk takes no structure latch

The same loop, and the writeback below it, iterate `map_regions_` and read
`pages.free_map` **without the structure latch**, while peers allocate
under it (`:336`, `EnsureRegionResident`). On a shared store that is a
`std::map` iterated during another core's insertion — undefined behaviour,
not a stale read — and a page checksummed over bytes another core is
changing.

This is the same class as AM-S2's frame-table work (`frames_` got a
structure latch; `map_regions_` did not), and the free map is the one
structure a peer both reads and grows.

## Why it is not fixed here

AW-a's subject was `MayWrite`. Fixing this is behaviour-changing in a way
that needs a decision rather than a patch: **who runs the map writeback
when one store serves every core?** Today N checkpointers would each run
it. The latch is the smaller half and could land alone, but landing it
would make the unlatched walk correct without making the *duplicate* walk
right, and a half-fix here reads as done.

`wal.md`'s fuzzy-checkpoint sentence carries the same open question for the
frame table (AW-S1 corrected the prose and left the behaviour to AM-S3);
this is the free-map half of it.

## Owner

AM-S3 owns writeback under sharing. AW-S1b's deletion removes `lease_`
entirely, which turns defect 1 into a compile error and forces the
decision — so the natural home is AW-S1b or AM-S3, whichever lands first.
Defect 2 is independent of both and could be pulled forward.
