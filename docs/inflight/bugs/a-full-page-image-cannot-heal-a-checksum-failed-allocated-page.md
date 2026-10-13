# A full page image cannot heal a checksum-failed page that is allocated

**Found by reading, not reproduced.** Verified at `3b9d5550` on
`br-backup-restore` by BR-S0's census (`instructions/v3.0.0/br-s0-census.md`
H4), while reading whether redo could heal a page a backup copy tore;
re-read at `f3da0413`, 2026-10-13.

## What is wrong

`docs/spec/wal.md` §10 says the header CRC32C detects a torn page and a
`FULL_PAGE_IMAGE` in the replay range heals it. Redo does poison the page on
a checksum failure and skip its later deltas (`src/wal/redo.cpp`'s
`PoisonedPages`). But the image's apply recreates the page through
`store.CreateAt` (the same file), and
`DevicePageStore::ClaimNamedIdLocked` refuses that for an id whose free-map
bit is set and whose device bytes are not zeros:

```cpp
if (!device_zeros) return Status::AlreadyExists(...);
```

(`src/storage/device_page_store.cpp`, line 1289 at `f3da0413`). A torn page is exactly an
allocated id with non-zero bytes, so the heal is refused and the mount fails
with that refusal rather than with the poison set's "no full page image
restored it".

No cell covers the heal: `recovery_pages_healed` is printed by `SHOW META`
and asserted by no test, and `tests/wal_redo_test.cpp` has no poison or heal
case.

## What it costs

**A refused mount, never a wrong answer**: either way the page is not
interpreted. The cost is that the images the engine does log (index builds,
bulk fills, chain-link edits, `BTREE_SPLIT`'s pages) do not buy the
recovery `wal.md` §10 says they buy. With the per-checkpoint image cadence
unbuilt (`known-gaps.md`), a torn page fails the mount either way today, so
the defect becomes material when that cadence is ordered.

## What BR does about it

Nothing in BR depends on the heal: BR-S0 replaced BR-R5's window images with
a fenced copy, so a copied page is never torn by the engine. The fix - an
image applying over an allocated, checksum-failed page in redo - belongs to
whatever order builds the image cadence (BR §7's successor list).
