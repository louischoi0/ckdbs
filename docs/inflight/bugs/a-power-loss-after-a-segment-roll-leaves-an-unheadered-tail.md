# A power loss after a segment roll leaves an unheadered tail the mount refuses

## What is wrong

A segment roll makes the new segment's name and body durable, but not its
header:

- `CreateSegment` zero-fills the new segment and `fsync`s it, then `fsync`s
  the directory (`src/wal/file_log_device.cpp:117`, `:287`). At that point the
  segment's name and its zeroed body are on the device.
- `StartSegment` then writes the header block with `WriteAt` and does not sync
  it (`src/wal/stream.cpp:79`). The header reaches the device at the next
  `Sync`, which covers every segment, or when the kernel writes it back.

If power is lost in between, the log's last segment is a full-size file whose
first block is zeros. At the next mount, `WalManager::Open` →
`WalStream::Open` → `ScanTail` validates that header
(`src/server/expeditor.cpp:777`, `src/wal/manager.cpp:80`,
`src/wal/stream.cpp:53`, `:101`) and refuses it as `Corruption` "magic
mismatch" (`src/wal/record.cpp:231-232`). No path treats an all-zero header on
the last segment as an interrupted roll.

**The simulator cannot reach this state.** `MemoryLogDevice::Crash` drops every
segment created since the last sync, name and all
(`src/wal/memory_log_device.cpp:139-146`). `FileLogDevice`, by contrast, makes
the name durable at creation. The crash model is therefore more forgiving than
the device it stands in for, exactly at a roll.

Verified at `dec4729` on `cn9-wal-device-mirror`, 2026-09-28. Found by the
second `critics-developer` pass over CN-9's `docs/inflight/` entries and
re-read before filing. This is found by reading, not reproduced.

## Smallest reproduction

A cell over `FileLogDevice` and `WalStream`:

1. Open a stream with a small segment size, and append until it rolls into
   segment 1.
2. Before any `Sync`, zero segment 1's first 4 KiB block through the file.
   This stands in for a header that never reached the device.
3. Reopen the stream. **Expected:** `Corruption` "magic mismatch".

A true power cut is the fuller test. CN-9 §6 item 4 is where an appliance
would run it.

## What it costs

**A refusal: the instance does not mount.** No committed data is lost:

- No `Sync` ran after the roll, so the durable point never entered the new
  segment.
- Every acknowledged commit therefore lies in earlier segments.
- Removing the last segment by hand restores the mount, because it is the
  highest-numbered and leaves no gap.

The refusal needs an operator who knows which file to remove.

## The fix

Not decided. Two shapes:

- **Make the header durable inside the roll.** `fdatasync` the new segment
  after the header write. That adds one more sync to a roll that already runs
  inside the append latch (`known-gaps.md`, WAL).
- **Recognise the interrupted roll at mount.** Treat an all-zero header on the
  last segment as a segment that never started, and remove it or re-header it.
  This is a recovery-rule change for `docs/spec/wal.md` §4.1 and §12.

Separately, the simulator's crash model should keep a created segment's name
as `FileLogDevice` does. Otherwise the corpus cannot find this class of
defect.
