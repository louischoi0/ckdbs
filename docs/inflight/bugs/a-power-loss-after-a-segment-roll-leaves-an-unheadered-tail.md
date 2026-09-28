# A power loss after a segment roll leaves an unheadered tail the mount refuses

## What is wrong

A segment roll makes the new segment's name and body durable, but not its
header:

- `CreateSegment` zero-fills the new segment and `fsync`s it, then `fsync`s
  the directory (`src/wal/file_log_device.cpp:117`, `:287`). At that point the
  segment's name and its zeroed body are on the device.
- `StartSegment` then writes the header block with `WriteAt`, and does not
  sync it (`src/wal/stream.cpp:79`).
- Segments are opened without `O_DIRECT` or `O_DSYNC`
  (`src/wal/file_log_device.cpp:127-128`). The header therefore sits in the
  host's page cache until the next `Sync`, which covers every segment, or
  until the kernel writes it back.

If power is lost in between, the log's last segment can be a full-size file
whose first block is zeros. At the next mount, `WalManager::Open` →
`WalStream::Open` → `ScanTail` validates that header
(`src/server/expeditor.cpp:777`, `src/wal/manager.cpp:80`,
`src/wal/stream.cpp:53`, `:101`) and refuses it as `Corruption` "magic
mismatch" (`src/wal/record.cpp:231-232`). No path treats an all-zero header on
the last segment as an interrupted roll.

**The simulator cannot reach this state.** `FileLogDevice` makes a new
segment's name durable at creation. The crash model is more forgiving than
that, exactly at a roll:

- `MemoryLogDevice::Crash()` drops every segment created since the last
  sync, name and all (`src/wal/memory_log_device.cpp:139-146`).
- `Crash(keep_bytes)` drops such a segment when none of its bytes survive
  (`:203-209`).
- `Crash(keep_bytes)` can keep a *partial* header. The mount refuses a torn
  header the same way, but `SimInstance::CrashWithLogPrefix`
  (`sim/instance.cpp:163`) has no caller, so the corpus has never produced
  one.

Verified at `dec4729` on `cn9-wal-device-mirror`, 2026-09-28, and re-read at
`d0d1d1b`. Found by the second `critics-developer` pass over CN-9's
`docs/inflight/` entries. Found by reading, not reproduced.

## Smallest reproduction

A cell over `FileLogDevice` and `WalStream`:

1. Open a stream over a device with a small segment size, and append until
   it rolls into segment 1.
2. Before any `Sync`, destroy the stream and the device.
3. Zero segment 1's first 4 KiB block through the file. This stands in for a
   header that never reached the device.
4. Reopen both. **Expected:** `Corruption` "magic mismatch".

A true power cut is the fuller test. CN-9 §6 item 4 is where an appliance
would run it.

## What it costs

**A refusal: the instance does not mount.** No commit acknowledged under D1 or
D2 is lost, because of the order in which the watermark moves:

1. `flushed_lsn_` enters the new segment only after the header write
   (`src/wal/stream.cpp:79`, then `:85`).
2. Every publisher publishes a watermark read from `flushed_lsn_` before its
   device sync. `WalStream::Sync` captures it under the latch
   (`src/wal/stream.cpp:270-283`). On an attached core, `WalManager` reads
   `flushed_lsn()` after a flush (`src/wal/manager.cpp:197`, `:257`), and
   `WalWriter` publishes that requested target (`src/wal/writer.cpp:72`,
   `:93-97`).
3. The device sync covers every segment that existed when it started.

So any durable point inside the new segment means an `fdatasync` covered the
header. A `Sync` that ran after the roll but before the header write is
harmless: its target is still in the old segment.

- D3 (`relaxed`) commits are acknowledged before any sync
  (`include/kds/wal/manager.hpp:53-54`, `docs/spec/wal.md:28`). They are lost
  here as they would be anywhere in D3's loss window.
- Removing the last segment by hand restores the mount, because it is the
  highest-numbered and leaves no gap. The refusal needs an operator who knows
  which file to remove.

## The fix

Not decided. Three shapes:

- **Write the header before the prewrite's `fsync`.** Pass the header block
  into `CreateSegment`, so the `fsync` at `src/wal/file_log_device.cpp:117`
  covers it. This adds no sync to the latched roll. It is the cheapest option.
- **Sync the header inside the roll.** `fdatasync` the new segment after the
  header write. This adds one sync to a roll that already runs inside the
  append latch (`known-gaps.md`, WAL).
- **Recognise the interrupted roll at mount.** Treat a last segment whose
  header is all zeros, or torn, as a segment that never started, and remove or
  re-header it. By the ordering above, neither can hold a D1/D2-durable
  record. This changes the recovery rules in `docs/spec/wal.md` §4.1 and §12.

Separately, the simulator's crash model should keep a created segment's name,
as `FileLogDevice` does, and `CrashWithLogPrefix` should have a caller.
Otherwise the corpus cannot find this class of defect.
