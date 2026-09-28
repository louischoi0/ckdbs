# WAL segment descriptors exhaust the open-file limit, and the roll that meets it strands a segment

## What is wrong

`FileLogDevice` holds one open descriptor for every segment of the log, for
the life of the process:

- `segments_` is a `std::vector<FileDescriptor>`, one per segment
  (`include/kds/wal/file_log_device.hpp:112-113`).
- `CreateSegment` appends a descriptor to it (`src/wal/file_log_device.cpp:296`).
- Nothing closes an entry.
- `Open` opens **every** segment on the device at mount and keeps each one
  (`src/wal/file_log_device.cpp:208-239`).
- `Sync` needs all of them, because it syncs every segment
  (`src/wal/file_log_device.cpp:365-377`).

The log is never recycled (`known-gaps.md`, WAL, "The log is never
recycled"), so the number of descriptors grows by one for every 64 MiB of log
(`include/kds/wal/log_device.hpp:41`). Nothing under `src/` or `include/`
calls `RLIMIT_NOFILE` or `setrlimit`, so the server runs with whatever soft
limit it inherited.

**When a roll meets the limit, it strands a segment.** `CreateSegment` opens
two descriptors: the segment file, which it keeps, and the directory, which
`SyncDirectory` opens briefly (`src/wal/file_log_device.cpp:412-415`). In a
process with no other descriptor churn, the first roll to reach the limit
goes like this:

1. The segment file opens.
2. The file is preallocated, zero-filled and `fsync`ed.
3. The directory open fails.
4. `CreateSegment` returns at `:287-289` without removing the file. Its
   other failure points do remove it (`:264`, `:274`, `:281`).
5. The file stays behind at full size with no header, because
   `StartSegment` writes the header only after `CreateSegment` returns
   (`src/wal/stream.cpp:63-79`).

If another descriptor takes the last free slot first, the segment open fails
instead. Then no file is created, and a later roll can succeed once
descriptors are free again.

Verified at `8f9a887` on `cn9-wal-device-mirror`, 2026-09-28, while writing
CN-9 §4 C3. The stranding path was found by that change's second
`critics-developer` pass and re-read at `dec4729`. This is found by reading,
not reproduced.

## Smallest reproduction

A cell over `FileLogDevice`. `Open` takes the segment size as a parameter
(`include/kds/wal/file_log_device.hpp:60-62`) and checks only that it is
non-zero, so a small size works.

1. Lower the process's soft `RLIMIT_NOFILE` to the descriptors already open
   plus 32.
2. Call `FileLogDevice::Open(dir, 0, 64 * 1024)`, then `CreateSegment(n)` for
   n = 0, 1, … until it fails. **Expected:** the 32nd call returns `IoError`
   from "open directory". `ErrnoStatus` maps `EMFILE` to its default case
   (`src/wal/file_log_device.cpp:25-35`), so "Too many open files" appears
   only in the message. `wal-0-31.log` is left on disk.
3. Call `CreateSegment(31)` again. **Expected:** `AlreadyExists`
   (`src/wal/file_log_device.cpp:134-136`).
4. Destroy the device. Open the directory again with `FileLogDevice::Open`,
   then open a `WalStream` over it. **Expected:**
   - `FileLogDevice::Open` succeeds.
   - `WalStream::Open` returns `Corruption` "magic mismatch"
     (`src/wal/stream.cpp:53`, `:101`; `src/wal/record.cpp:231-232`).
   - After removing `wal-0-31.log`, both opens succeed.

## What it costs

**A refusal that does not clear, and then an instance that does not mount.**
No wrong answer: every failure is a refused operation.

- **While running.**
  - Once a roll strands a segment, every later roll hits `O_EXCL` on it and
    returns `AlreadyExists`. From then on every append fails, whatever the
    descriptor count does next.
  - The limit is process-wide. A new connection's `accept` can meet it before
    a roll does.
  - The descriptors in use are the segments plus the data file, the client
    sockets and the listener.
- **At the next mount.**
  - The mount opens the log before any socket exists
    (`src/server/expeditor.cpp:752`), so it usually has room to open every
    segment.
  - It then adopts the stranded segment, whose size matches, and refuses it as
    `Corruption` (reproduction step 4).
  - Raising the limit does not restore the mount. Removing the stranded
    segment does: it is the highest-numbered, so removing it leaves no gap for
    `Open` to refuse.
  - A mount refused by the descriptor count alone needs a lower limit at
    restart than the running process had.

At a soft limit of 1024, a common service-manager default, this happens at
about 1000 segments, about 64 GiB of cumulative log. That figure is
`[design]` arithmetic, not a measurement. The dev host's limit is 1048576,
which is why no run here has met it.

## The fix

Not decided, and not only this file's.

- **Always:** `CreateSegment`'s `SyncDirectory` failure should remove the file
  as its other failure points do. This is local and stops the stranding.
- **For the symptom:** close a segment's descriptor once the segment is sealed
  and synced. That breaks `Sync`'s every-segment rule, which would then need a
  record of which segments still owe a sync. That record is exactly what
  `known-gaps.md`'s "Every sync covers every segment" entry says is missing.
- **For the cause:** recycle segments (wal.md §11-4). That waits on the archive
  decision (CN-9 §9 O4).

Until the second or third lands, a deployment raises the limit with
`LimitNOFILE=` in the service unit. This is CN-9 §9 O6's deployment arm.
