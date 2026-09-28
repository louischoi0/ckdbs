# CN-9 — Concept Note: A Dedicated, Mirrored WAL Device — `wal_dir` on two PLP NVMe drives in RAID1, for a KDS appliance

Status: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
**No work starts on this note until the operator rules on §9.** It is a
deployment concept: its subject is a machine KDS runs on, and most of it
would change no line of the engine. Ordering relative to CN-1 to CN-8 is
not fixed and none of them depends on it.
Author: CLA, 2026-09-28, against `8f9a887`
Origin: the operator's conversation of 2026-09-28 — a business idea
spanning device (machine) design and build, consulting, and in-house-network
database installation; the operator then narrowed it to the device, and
asked for the WAL-on-a-dedicated-mirrored-NVMe layout in detail and for this
note. The operator's statements are the `[operator]` claims in §0; the
layout, the tool choices and the sizing are CLA's proposals (§9), none
ratified.
Claim tags: `[operator]` for the operator's statements; `[source-read]`
with `path:line` at `8f9a887`; everything else `[design]`, including every
statement about Linux md, filesystems, NVMe drives and other vendors' products,
which comes from CLA's training knowledge and is to be re-checked before a
work order cites it. Nothing is `[measured]`.
Relation to `docs/inflight/`: §4 records four properties of the WAL as built
that bound this layout. **They are gaps in what is built, and this note is not
their record** — `docs/inflight/known-gaps.md` at `8f9a887` does not carry
them, and whether it or `bugs/` should is O3.

---

## 0. The concept

`[operator]` The business under consideration is *device (machine) design and
build, consulting, and in-house-network DB installation*; *"focus on the
device area first"*; *"explain in detail the plan to separate `wal_dir` onto
dedicated NVMe and mirror two drives in RAID1."*

`[design]` Restated: **the appliance gives the log its own pair of devices.**
A KDS appliance is a general-purpose server with a pinned bill of materials,
not purpose-built hardware. Its storage has three roles on separate devices:

| role | devices | why separate |
|---|---|---|
| OS | small mirrored boot pair | an OS update or a full root filesystem never touches the log or the data |
| **WAL** (`wal_dir`) | **two PLP NVMe drives, Linux md RAID1** | every commit waits on this device's flush (§2); nothing else queues ahead of it |
| data (`data_file`) | PLP NVMe, mirrored (RAID1 or RAID10) | the checkpoint's random 8 KiB writeback stays out of the commit path |

The concept is the middle row. The two properties it buys are **commit latency
that does not depend on anything else writing** and **surviving the loss of one
log device**.

## 1. What other systems ship

`[design]`, from training knowledge.

| system | mechanism |
|---|---|
| Oracle | redo log groups with multiplexed members on separate disks: the engine writes every member itself; Exadata adds Smart Flash Log (redo written to disk and flash at once, first completion wins) |
| PostgreSQL | `pg_wal` placed on its own volume (by `initdb --waldir` or a symlink); redundancy is the volume's, not the engine's |
| MySQL InnoDB | `innodb_log_group_home_dir` on its own volume; same division |
| SQL Server | vendor guidance: the log file on its own RAID1 or RAID10 volume, never RAID5 |
| DB2 | `MIRRORLOGPATH`: engine-level dual log writes |

Two families: **the engine mirrors the log itself** (Oracle, DB2) or **the
engine writes one log and the block layer mirrors it** (PostgreSQL, MySQL,
SQL Server). This note takes the second; §9 O1 keeps the first open.

## 2. What this tree has

- `[source-read]` **One log for the instance; one thread issues its syncs.**
  Core 0 owns the stream, peers append under its latch and ask core 0's writer
  for a sync, *"so every `fdatasync` this instance pays is issued once"*
  (`docs/spec/wal.md:46`). A commit's latency is therefore this device's flush
  latency plus queueing behind the previous flush.
- `[source-read]` **Segments are fixed-size files, created at full size.**
  64 MiB by default (`include/kds/wal/log_device.hpp:41`). Each is
  `posix_fallocate`d, zero-filled and `fsync`ed at creation, so a
  commit-path sync is data-only `fdatasync`
  (`include/kds/wal/file_log_device.hpp:22-31`, `src/wal/file_log_device.cpp:96-121`).
  The directory is `fsync`ed at creation, not on the commit path
  (`src/wal/file_log_device.cpp:287`).
- `[source-read]` **Ordering between the log device and the data device is the
  engine's, not the hardware's.** Page writeback waits on the WAL gate before
  it writes (`src/storage/device_page_store.cpp:1290`, `:1368`). The superblock
  anchor is persisted only after `CHECKPOINT_END` is durable
  (`docs/spec/wal.md:145`). Putting the two on different devices therefore
  asks nothing of either device about the other.
- `[source-read]` **The durable end of the log is found by CRC.** Recovery takes
  the first record with a bad CRC or an impossible length as the end
  (`docs/spec/wal.md:87`). Torn *pages* are healed from `FULL_PAGE_IMAGE`s,
  **which live in the log** (`docs/spec/wal.md:135-137`).
- `[source-read]` **There is no backup, archive or replication.** Archiving and
  PITR are `[PROPOSED]` and replication is a readiness note
  (`docs/spec/wal.md:200-201`). **A device mirror is therefore the only
  protection against a device failure the engine can be deployed with.**
  Losing the log loses more than the tail: the data file alone is behind the
  last checkpoint and cannot heal a torn page.

## 3. The layout

`[design]` throughout.

### 3.1 Why md RAID1

| option | verdict |
|---|---|
| **Linux md RAID1 (mdadm)** | proposed: standard, no extra part, forwards flushes to both members, resyncs after a crash |
| hardware RAID (NVMe tri-mode controller) | adds controller latency in front of NVMe; the controller is a single point of failure; its protected cache duplicates what PLP already gives |
| Intel VROC | ties the BOM to a platform and a licence |
| ZFS mirror | a sync write goes through the ZIL: a log under a log on the commit path |
| engine-level log multiplexing (§1's first family) | a feature KDS does not have; O1 |

### 3.2 How the mirror behaves under this log

- **Latency is the slower member's.** A write completes when both members do,
  so the mirror's p99 is roughly a single drive's p99.5. The two members are
  **the same model on the same firmware**, pinned in the BOM.
- **Both members must be PLP.** md advertises a volatile write cache if any
  member has one, and then every `fdatasync` pays a real flush on that member.
  `nvme id-ctrl <dev> | grep -i vwc` on both; a drive reporting `vwc 1` needs
  its datasheet to settle PLP.
- **After a crash the members may disagree** over the region that was in flight.
  md reads a region still awaiting resync from the first member only, so a
  reader sees one consistent version. §2's CRC rule then finds the end in that
  version. The two rules compose only if resync is never skipped: no
  `--assume-clean` on a member that has held a log.
- **Write-intent bitmap on, with a large chunk.** Without a bitmap, recovery
  resyncs the whole device. With one, a newly dirtied chunk costs a synchronous
  bitmap write, so the chunk is sized at or above the 64 MiB segment
  (`--bitmap-chunk=128M`) to keep that write rare on a sequential log.
- **Degraded and rebuild.** With one member gone the instance keeps running
  unmirrored. `mdadm --monitor` must alert, and `speed_limit_max` caps the
  rebuild so that it does not take the log's write bandwidth.

### 3.3 Filesystem and configuration

- XFS or ext4 both serve: §2's preallocate, prewrite and `fdatasync` pattern asks
  nothing either lacks. `noatime`.
- `wal_dir = <mount point>` in `kds.conf`.
- The service unit raises `LimitNOFILE` (§4 C3).

```
mdadm --create /dev/md/kdswal --level=1 --raid-devices=2 --metadata=1.2 \
      --bitmap=internal --bitmap-chunk=128M /dev/nvme0n1 /dev/nvme1n1
mkfs.xfs /dev/md/kdswal
mount -o noatime /dev/md/kdswal /var/lib/kds/wal
```

## 4. What the engine as built does to the device

Each item is `[source-read]` for the code and `[design]` for the consequence.
None was measured.

- **C1 — The log is never recycled, so it only grows.** `wal.md` §11-4 makes a
  segment recyclable once archived (`docs/spec/wal.md:146`), and archiving is
  `[PROPOSED]` (`:200`). The only segment removal in the device is cleanup of a
  failed creation (`src/wal/file_log_device.cpp:264`, `:274`, `:281`).
  *Consequence:* the WAL device's capacity is the instance's lifetime. When it
  fills, `CreateSegment` fails and appends stop. A small dedicated log device,
  which is the usual reason to separate one, is exactly the device this hurts.
- **C2 — Every sync covers every segment ever created.** `FileLogDevice::Sync`
  syncs all open segments, and its comment forbids narrowing that to the tail
  for a correctness reason (`src/wal/file_log_device.cpp:365-377`).
  *Consequence, with C1:* the number of `fdatasync` calls per durable-point
  advance grows with the total log written. Some filesystems can issue a device
  flush even for a clean file; on a PLP device that flush is cheap, but the
  syscalls are not free. Note that `wal.md:46`'s *"issued once, over one file"*
  describes the logical log, not what the device issues.
- **C3 — Every segment holds an open descriptor, and nothing raises the limit.**
  `segments_` keeps one `FileDescriptor` per segment
  (`include/kds/wal/file_log_device.hpp:112-113`). `Open` adopts every existing
  segment (`:55-60`). No `RLIMIT_NOFILE` or `setrlimit` appears under `src/` or
  `include/`. *Consequence:* under a service manager's common default soft
  limit of 1024, the instance fails to create a segment after roughly
  1000 segments, about 64 GiB of cumulative log, minus the sockets and the data
  file. The limit is raised in the unit (§3.3). The cause is C1.
- **C4 — A segment roll runs inside the instance's append latch.**
  `WalStream::Append` takes the stream latch and, when the segment is full,
  calls `Roll` under it (`src/wal/stream.cpp:210-219`). `Roll` reaches
  `CreateSegment` (`:142-148`), which zero-fills 64 MiB and `fsync`s it, then
  `fsync`s the directory (`src/wal/file_log_device.cpp:96-121`, `:287`).
  *Consequence:* once every 64 MiB of log, every core's append waits for a
  segment-sized write to reach the device. **The length of that stall is set by
  the WAL device's sequential write bandwidth, and a mirror writes it twice.**
  It is a device-selection criterion (§5) and a shipped-latency number (§6).

C1 to C3 bound the layout from above: they stop a long-running instance whatever
the device is. **An appliance cannot be shipped for sustained operation while
they stand**, which is why O3 and O4 are listed first among the engine-side
decisions.

## 5. Sizing

`[design]`, estimates.

- **Host writes to the WAL device** ≈ 2 × log volume (each segment is written
  once as zeros, §2, then with records) + about 4 KiB per sync (a buffered
  partial tail page is written whole each time).
  - Example: at 5,000 syncs/s, the tail rewrite alone is about 20 MB/s, or about
    1.7 TB/day, before any record bytes.
  - That puts the drive class at mixed use (about 3 DWPD). Read-intensive
    (about 1 DWPD) is too low.
- **Capacity**, while C1 stands: daily log volume × intended days of operation.
  After recycling exists: checkpoint interval × log rate, plus headroom for a
  checkpoint that runs long.
- **Sequential write bandwidth** matters beyond throughput because of C4: it is
  what bounds the roll stall.
- NVMe namespace formatted to 4 KiB LBAs (`nvme format --lbaf`, destructive).

## 6. What a shipped unit is validated against

`[design]`.

1. `fio --rw=write --bs=4k --ioengine=sync --fdatasync=1` on the mounted mirror.
   Record p50, p99 and p99.9, single drive against mirror; the result becomes
   the unit's commit-latency baseline.
2. The C4 roll stall under load, measured and recorded per BOM.
3. Pull one member under load: degraded transition, alert delivered, commit p99
   during rebuild.
4. Cut power under load (not a process kill; `sim/` covers crashes, not PLP):
   md resync plus KDS recovery must return exactly the acknowledged commits.
5. Burn-in before shipment.

## 7. What this note does not license

- No engine change: not segment recycling, not a background roll, not a
  descriptor cap, not engine-level log multiplexing. Each is a work order the
  operator has not issued. §4 names them as constraints only.
- No new configuration key. If one is ever needed, it re-scopes an existing
  setting (`CLAUDE.md`, Working Rules).
- No claim that the layout performs. Every number in §5 is an estimate; §6 is
  what would make one.
- No record of C1 to C4 as gaps (O3).
- No appliance product decision: the business shape is the operator's.

## 8. Known consequences

- **RAID1 is not a backup.** It survives one device failure. It does not
  survive a logical error, an operator's `DROP`, filesystem damage, or losing the
  chassis. Until wal.md §13's archiving exists, the appliance offers no recovery
  from those.
- **Mirroring the log alone is not enough.** Losing an unmirrored data device
  loses the instance just the same (§2), so the data role is mirrored too (O2).
- **Tail latency rises** by the max-of-two effect (§3.2), in exchange for
  surviving a failure.
- **It does not transfer to cloud block storage.** There, flush cost is a
  network round trip plus the provider's replication, and neither PLP nor a
  local mirror changes it. The concept is for on-premises and bare-metal units.

## 9. Operator decisions this note leaves open

Engine-side:

- **O3 — Where C1 to C4 are recorded.** `docs/inflight/known-gaps.md` (missing
  function: recycling) or `docs/inflight/bugs/` (C3's descriptor exhaustion
  and C4's latched roll read as defects), each entry verified at its own commit.
  CLA's proposal: C1 and C2 in `known-gaps.md` under the WAL, C3 and C4 in
  `bugs/`.
- **O4 — Whether segment recycling waits for archiving.** Today §11-4 ties them
  together, so recycling cannot exist before wal.md §13's archive seam. The
  alternative is recycling below the redo start with no archive, which gives up
  PITR for those segments. This is a durability decision, not a device one.

Device-side:

- **O1 — Block-layer mirror or engine mirror.** This note proposes md RAID1.
  The engine-level alternative (§1) would let KDS take the first completion,
  which is Smart Flash Log's trick, but it is a feature.
- **O2 — The data role's redundancy:** RAID1 or RAID10, and whether it shares
  a controller or PCIe root with the log pair.
- **O5 — Where appliance material lives.** This tree states the engine. A BOM,
  a golden OS image and a validation record are the appliance's, and may belong
  outside `docs/`.
- **O6 — `LimitNOFILE` in the unit, or `setrlimit` in the engine.** The first is
  deployment and needs no code. The second is a change to the platform layer
  (`docs/rules/rules.md` #4). Either treats C3's symptom, not C1.
