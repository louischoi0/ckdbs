# The data file is grown from every core, and three places still say core 0 alone grows it

**Found** 2026-09-07 while landing AW-S1b on `worktree-aw-m1-close`,
verified at `70178b2` plus that stage's uncommitted tree. **Not fixed.**
The defect predates AW-S1b — it arrived with the shared pool at `2663001` —
and AW-S1b widens it, which is why it is filed here rather than in that
stage's record alone.

## The declared rule

`docs/rules/rules.md` §3 lists the data-file device as a shared structure
whose serialization is *"core 0 alone writes; readers see a monotone
value"*. `include/kds/storage/file_page_device.hpp:48-51` states the same
thing as the reason a read of `page_capacity_` needs no atomic: **"the only
member a read touches is `page_capacity_` … and core 0 alone grows the
file"**. `docs/spec/crosscore.md` CC11 said it too until AW-S1b corrected
that sentence.

## What is true

`FilePageDevice::EnsureCapacity` (`src/storage/file_page_device.cpp:169`)
writes `page_capacity_`, a plain `std::uint32_t`, after a `posix_fallocate`.
`MemoryPageDevice` has the same shape. Neither takes a lock, and the
`DevicePageStore` paths that reach them do so **outside** the frame-table
structure latch:

| call site | reachable from a peer since |
|---|---|
| `device_page_store.cpp:1047` — `CreateNewUnpinned`'s `EnsureAddressable` after the claim loop | `2663001` (AM-S2 step 3): a peer borrows core 0's store, so it takes the unleased allocation path |
| `device_page_store.cpp:135` — `EnsureHeaderlessMap` claiming its bitmap's id | AW-S1b: the leased arm skipped the claim and the growth with it |
| `device_page_store.cpp:171` — `EnsureRegionResident` creating region N+1 | AW-S1b: a leased store got a private empty region instead of creating one |

So two cores allocating past the file's end concurrently race on
`page_capacity_` — a data race by the standard, and two overlapping
`posix_fallocate` calls for the same extent — while every reader of
`page_capacity()` reads the field with no synchronization on the strength of
a comment that is no longer true.

`device_page_store.cpp:976` (`CreateAtUnpinned`) stays core 0's: its callers
are bootstrap and fixed system pages.

## Why it has not bitten

The window is narrow (both cores must run off the end of the file within
the same growth) and `EnsureCapacity` is idempotent by value — the second
caller's `nr_pages <= page_capacity_` test returns OK if it observes the
first's write. What it is not idempotent against is *not* observing it: a
torn or stale read grows the file twice, and the ceiling check and the
extent rounding both run on a value another thread is writing.

Nothing in the suite drives two cores into device growth at once; the
concurrency cells (`alloc_race_test.cpp`, `am_s2_pin_protocol_test.cpp`)
allocate inside a file the fixture has already sized.

## What the fix has to decide

Not a latch placement but an owner. Either

- **growth stays core 0's**, which means the allocation path on a peer has
  to reach core 0 for it — a message, which is exactly the thing extent
  leases existed to avoid and which AW-S1b removed the machinery for; or
- **growth becomes any core's**, which means `page_capacity_` is an
  `std::atomic<uint32_t>` with a CAS or a mutex around the fallocate, and
  `rules.md` §3's row plus `file_page_device.hpp`'s paragraph are rewritten
  to say so.

The second is the smaller change and the one the engine has already drifted
into; it is a decision rather than a patch because the first is what the
declared rule says.

## Owner

AM-S3, with the free-map writeback question it already carries
(`flushmaps-lease-guard-and-unlatched-region-walk.md`) — both are "what does
one store serving every core do about the structures underneath it".
