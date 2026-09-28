# WAL segment descriptors exhaust the open-file limit

**Found by reading, not reproduced.** Verified at `8f9a887` on
`cn9-wal-device-mirror`, 2026-09-28, while writing CN-9 §4 C3.

## What is wrong

`FileLogDevice` holds one open descriptor for every segment of the log, for
the life of the process:

- `segments_` is a `std::vector<FileDescriptor>`, one per segment
  (`include/kds/wal/file_log_device.hpp:112-113`).
- `CreateSegment` appends to it and nothing closes an entry
  (`src/wal/file_log_device.cpp:244`, the `push_back` at the end).
- `Open` opens **every** segment on the device at mount and keeps each one
  (`src/wal/file_log_device.cpp:208-237`).
- `Sync` needs all of them, because it syncs every segment
  (`src/wal/file_log_device.cpp:365-377`).

The log is never recycled (`known-gaps.md`, WAL, "The log is never
recycled"), so the number of descriptors grows with every 64 MiB of log
(`include/kds/wal/log_device.hpp:41`). No `RLIMIT_NOFILE` or `setrlimit`
appears under `src/` or `include/`: the server runs with whatever soft limit
it inherited.

## What it costs

**A refusal, and then an instance that cannot restart.**

- **While running:** once the descriptors in use reach the soft limit,
  `CreateSegment`'s open fails with `EMFILE`, the roll fails, and every
  append after it fails. The descriptors in use are the segments plus the
  data file, the client sockets and the listener. The limit is
  process-wide, so a new connection's `accept` can hit `EMFILE` before a
  roll does.
- **At the next mount:** `Open` needs one descriptor per segment before
  recovery can begin, so a log with more segments than the limit refuses
  the mount. Raising the limit restores the mount; deleting old segments
  does not, because `Open` refuses a gap in the numbering with `Corruption`
  (`src/wal/file_log_device.cpp:208-213`).

Under a soft limit of 1024 — a common service-manager default — the limit
is reached at about 1000 segments, about 64 GiB of cumulative log. That is
`[design]` arithmetic, not measured. The dev host's limit is 1048576, which
is why no run here has met it.

No wrong answer: every failure is a refused operation.

## Smallest reproduction

A cell over `FileLogDevice` directly, with a small segment size, which `Open`
takes as a parameter (`include/kds/wal/file_log_device.hpp:60-62`):

1. Lower the process's soft `RLIMIT_NOFILE` to the descriptors already open
   plus 32.
2. `FileLogDevice::Open(dir, 0, 64 * 1024)`, then call `CreateSegment(n)`
   for n = 0, 1, … until it fails. **Expected: an `EMFILE` status after about
   32 segments.**
3. Destroy the device, then `Open` the same directory again under the same
   limit. **Expected: the open fails.** Raise the limit and `Open` succeeds.

## The fix

Not decided, and not only this file's. There are two fixes:

- **Treat the symptom:** close a segment's descriptor once the segment is
  sealed and synced. That changes `Sync`'s every-segment rule, so the rule
  would need replacing with a record of which segments still owe a sync,
  and that record is exactly what `known-gaps.md`'s "Every sync covers every
  segment" entry says is missing.
- **Treat the cause:** recycle segments (wal.md §11-4), which waits on the
  archive decision (CN-9 §9 O4).

Until either lands, a deployment raises the limit (`LimitNOFILE=` in the
service unit). That is CN-9 §9 O6's deployment arm.
