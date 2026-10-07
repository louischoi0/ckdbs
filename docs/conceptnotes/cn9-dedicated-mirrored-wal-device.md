# CN-9 — Concept Note: A Dedicated, Mirrored WAL Device — `wal_dir` on two PLP NVMe drives in RAID1, for a KDS appliance

Status: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
**No work starts on this note until the operator rules on §9.** It is a
deployment concept: its subject is a machine KDS runs on, and most of it
would change no line of the engine. Ordering relative to CN-1 to CN-8 is
not fixed and none of them depends on it.
Author: CLA, 2026-09-28, against `8f9a887`
Origin: the operator's conversation of 2026-09-28. The operator described a
business idea spanning device (machine) design and build, consulting, and
installing databases on in-house networks; then narrowed it to the device;
then asked for the plan to put the WAL on a dedicated, mirrored NVMe pair, in
detail, and for this note. The operator's statements are the `[operator]`
claims in §0 and §9. The layout, the tool choices and the sizing are CLA's
proposals (§9), none ratified.
Claim tags: `[operator]` for the operator's statements; `[source-read]`
with `path:line` at `8f9a887`; everything else `[design]`, including every
statement about Linux md, filesystems, NVMe drives and other vendors'
products, which comes from CLA's training knowledge and is to be re-checked
before a work order cites it. Nothing is `[measured]`.
Relation to `docs/inflight/`: §4 names five properties of the WAL as built
that bound this layout. They are recorded in `docs/inflight/`, not here; §9 O3
says where.

---

## 0. The concept

`[operator]` The business under consideration is *device (machine) design
and build, consulting, and in-house-network DB installation*. The operator
said to *"focus on the device area first"*, and asked to *"explain in detail
the plan to separate `wal_dir` onto dedicated NVMe and mirror two drives in
RAID1."*

`[design]` Restated: **the appliance gives the log its own pair of devices.**
A KDS appliance is a general-purpose server with a pinned bill of materials,
not purpose-built hardware. Its storage has three roles, each on its own
devices:

| role | devices | why separate |
|---|---|---|
| OS | small mirrored boot pair | an OS update or a full root filesystem never touches the log or the data |
| **WAL** (`wal_dir`) | **two PLP NVMe drives, Linux md RAID1** | every commit waits on this device's flush (§2); nothing else queues ahead of it |
| data (`data_file`) | PLP NVMe, mirrored (RAID1 or RAID10) | the checkpoint's random 8 KiB writeback stays out of the commit path |

The concept is the middle row. It buys two things: **commit latency that does
not depend on anything else writing**, and **surviving the loss of one log
device**.

## 1. What other systems ship

`[design]`, from training knowledge.

| system | mechanism |
|---|---|
| Oracle | redo log groups with multiplexed members on separate disks: the engine writes every member itself; Exadata adds Smart Flash Log (redo written to disk and flash at once, first completion wins) |
| PostgreSQL | `pg_wal` placed on its own volume (by `initdb --waldir` or a symlink); redundancy is the volume's, not the engine's |
| MySQL InnoDB | `innodb_log_group_home_dir` on its own volume; same division |
| SQL Server | vendor guidance: the log file on its own RAID1 or RAID10 volume, never RAID5 |
| DB2 | `MIRRORLOGPATH`: engine-level dual log writes |

There are two families. In the first, **the engine mirrors the log itself**
(Oracle, DB2). In the second, **the engine writes one log and the block layer
mirrors it** (PostgreSQL, MySQL, SQL Server). This note takes the second;
§9 O1 keeps the first open.

## 2. What this tree has

- **Core 0 issues every sync of the one log.**
  - `[source-read]` Peers append under the stream's latch and ask core 0's
    writer for a sync. The sync is issued on core 0's reactor or its writer
    thread (`docs/spec/wal.md:46`).
  - `[design]` A commit therefore waits on this device's flush latency (see
    §4 C2 for how many calls that is), plus any queueing behind the previous
    sync.
- **Segments are fixed-size files, created at full size.**
  - `[source-read]` A segment is 64 MiB by default
    (`include/kds/wal/log_device.hpp:41`). At creation it is
    `posix_fallocate`d, zero-filled and `fsync`ed
    (`src/wal/file_log_device.cpp:257-277`, `:96-124`), so a commit-path sync
    is data-only `fdatasync` (`include/kds/wal/file_log_device.hpp:22-31`).
    The directory is `fsync`ed at creation, not on the commit path
    (`src/wal/file_log_device.cpp:287`).
- **Ordering between the log device and the data device is the engine's, not
  the hardware's.**
  - `[source-read]` Page writeback waits on the WAL gate before it writes
    (`src/storage/device_page_store.cpp:1290`, `:1368`). The checkpoint
    anchor is published only after `CHECKPOINT_END` is durable
    (`src/wal/checkpointer.cpp:223-236`).
  - `[design]` Putting the two on different devices therefore asks nothing
    of either device about the other.
- **The durable end of the log is found by CRC.**
  - `[source-read]` A bad length or CRC ends the scan
    (`src/wal/record.cpp:187-192`). Torn *pages* are healed from
    `FULL_PAGE_IMAGE`s, **which live in the log**
    (`docs/spec/wal.md:135-137`).
- **There is no backup, archive or replication.**
  - `[source-read]` Archiving and PITR are `[PROPOSED]`, and replication is a
    readiness note (`docs/spec/wal.md:200-201`).
  - `[design]` A device mirror is therefore the only protection against a
    device failure the engine can be deployed with. Without the log, the data
    file is at no consistent point, and nothing can heal a torn page.

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

- **Latency is the slower member's.**
  - A write goes to both members in parallel and completes when both have
    finished.
  - If the two members were independent, the mirror's p99 would be roughly a
    single drive's p99.5. Same-model drives under the same load are
    correlated, so the real figure is measured, not derived.
  - The two members are **the same model on the same firmware**, pinned in the
    BOM.
- **Both members must be PLP.**
  - md forwards a flush to each member. A member without a volatile write
    cache completes it at once; a member with one pays a real flush on every
    `fdatasync`.
  - Check both members: `nvme id-ctrl <dev> | grep -i vwc` and
    `/sys/block/<dev>/queue/write_cache`. A drive that reports a volatile
    cache needs its datasheet to settle whether it has PLP.
- **After a crash, the members may disagree** over the region that was in
  flight.
  - md reads a region still awaiting resync from the first member only, so a
    reader sees one consistent version. §2's CRC rule then finds the end of
    the log in that version.
  - The two rules compose only if resync is never skipped: no
    `--assume-clean` on a member that has held a log.
- **Write-intent bitmap on, with a large chunk.**
  - Without a bitmap, a crash forces a resync of the whole device.
  - With one, each chunk dirtied again costs a synchronous bitmap write. On a
    sequential log that happens about once per (log bytes ÷ chunk size), plus
    once each time idle time lets md clear the bits. Use a large
    `--bitmap-chunk`.
- **Degraded and rebuild.**
  - With one member gone, the instance keeps running unmirrored, and
    `mdadm --monitor` must alert.
  - Under a log that never stops writing, md throttles the rebuild down to
    `speed_limit_min`, about 1 MB/s by default. The rebuild then takes days,
    and the array stays unmirrored for all of it. That unmirrored window is
    the larger risk.
  - Set `speed_limit_min` (or per array, `/sys/block/mdX/md/sync_speed_min`)
    to bound that window. `speed_limit_max` caps how much of the log's write
    bandwidth a rebuild may take.

### 3.3 Filesystem and configuration

- XFS or ext4 both serve: neither lacks anything §2's preallocate, prewrite
  and `fdatasync` pattern needs. Mount with `noatime`.
- Set `wal_dir = <mount point>` in `kds.conf`.
- The service unit raises `LimitNOFILE`, as proposed (§4 C3, §9 O6).

```
mdadm --create /dev/md/kdswal --level=1 --raid-devices=2 --metadata=1.2 \
      --bitmap=internal --bitmap-chunk=128M /dev/nvme0n1 /dev/nvme1n1
mkfs.xfs /dev/md/kdswal
mount -o noatime /dev/md/kdswal /var/lib/kds/wal
```

## 4. What the engine as built does to the device

Each item points at its record in `docs/inflight/`, where the code is cited.
What follows each pointer is the `[design]` consequence for this layout. None
was measured.

- **C1 — The log is never recycled, and old segments cannot be removed by
  hand** (`known-gaps.md`, WAL).
  - The WAL device's capacity is the instance's lifetime.
  - A small dedicated log device, the usual reason to separate one, is the
    device this hurts most.
- **C2 — Every sync covers every segment ever created** (`known-gaps.md`,
  WAL).
  - With C1, the number of `fdatasync` calls per commit grows with the
    instance's age.
  - On PLP drives each call is cheap, but the calls are not free.
- **C3 — One open descriptor per segment, and nothing raises the limit**
  (`bugs/wal-segment-descriptors-exhaust-the-open-file-limit.md`).
  - At a soft limit of 1024, the roll that meets the limit, at about 64 GiB of
    cumulative log, can leave a stranded segment behind.
  - After that, every append fails, and the next mount refuses the log until
    the stranded segment is removed.
  - Raising the limit prevents this; it does not repair it. The cause is C1.
- **C4 — A segment roll does I/O inside the instance's append latch**
  (`known-gaps.md`, WAL; deliberate per `include/kds/wal/stream.hpp:35-43`).
  - Once per 64 MiB of log, every core's append waits for a segment-sized
    write.
  - The slower member's sequential write bandwidth sets the length of that
    wait. That makes the bandwidth a device-selection criterion (§5) and a
    number every shipped unit records (§6).
- **C5 — A roll's header is not synced**
  (`bugs/a-power-loss-after-a-segment-roll-leaves-an-unheadered-tail.md`).
  - A power loss between a roll and the next sync can leave a last segment
    the mount refuses.
  - **PLP does not close this window.** PLP protects what has reached the
    drive. Until a sync or kernel writeback, this header may still be in the
    host's page cache (the bug file says why).
  - The simulator's crash model cannot produce this state, so only §6's power
    cut would find it on a unit.

**C1 and C3 stop a long-running instance whatever the device is. C5 stops a
mount after an ill-timed power loss. C2 and C4 degrade the instance.**

## 5. Sizing

`[design]`, estimates.

- **Host writes to the WAL device** ≈ log volume (each segment's zero fill,
  §2) + max(log volume, syncs × 4 KiB).
  - The second term is there because a buffered partial tail page is written
    whole on every sync, and the record bytes land inside it.
  - Example: at 5,000 syncs/s, the second term is at least
    5,000 × 4 KiB ≈ 20 MB/s ≈ 1.7 TB/day. The first term adds the day's log
    volume on top.
- **Endurance**: choose the drive so that DWPD × capacity ≥ daily host writes.
  - 1.7 TB/day alone needs at least 0.9 DWPD on a 1.92 TB drive and 1.8 DWPD
    on a 960 GB drive. These are lower bounds, before the log volume.
- **Capacity**
  - While C1 stands: daily log volume × intended days of operation.
  - After recycling exists: checkpoint interval × log rate, plus headroom for
    a checkpoint that runs long.
- **Sequential write bandwidth**: this bounds C4's wait, so it counts beyond
  throughput.
- **LBA format**: format the NVMe namespace to 4 KiB LBAs
  (`nvme format --lbaf`, destructive).

## 6. What a shipped unit is validated against

`[design]`.

1. **Commit-latency baseline.** Run `fio --rw=write --bs=4k --ioengine=sync
   --fdatasync=1 --overwrite=1` on the mounted mirror, over a file already
   written, as KDS's segments are. Record p50, p99 and p99.9 for a single
   drive and for the mirror.
2. **C4 wait.** Measure it under load and record it for each BOM.
3. **Member pull.** Pull one member under load. Check the degraded
   transition and that the alert arrives, then measure commit p99 during the
   rebuild and how long the rebuild takes.
4. **Power cut.** Cut power under load; a process kill is not a substitute.
   `sim/` models neither PLP nor §4 C5's durable, unheadered segment. md
   resync plus KDS recovery must return exactly the acknowledged commits, and
   the cuts must include some timed at a segment roll.
5. **Burn-in.** Burn in every unit before shipment.

## 7. What this note does not license

- **No engine change.** Segment recycling, a background roll, a synced roll
  header, a descriptor cap and engine-level log multiplexing each need a
  work order the operator has not issued.
- **No new configuration key.** If one is ever needed, it re-scopes an
  existing setting (`CLAUDE.md`, Working Rules).
- **No performance claim.** Every number in §5 is an estimate; the §6 checks
  are what would produce real ones.
- **No appliance product decision.** The business shape is the operator's.

## 8. Known consequences

- **RAID1 survives one device failure and nothing else.** A logical error, an
  operator's `DROP`, filesystem damage or the loss of the chassis reaches both
  members. Until wal.md §13's archiving exists, the appliance cannot recover
  from any of them.
- **It does not transfer to cloud block storage.** There, flush cost is a
  network round trip plus the provider's replication, and neither PLP nor a
  local mirror changes it. The concept is for on-premises and bare-metal
  units.

## 9. Operator decisions this note leaves open

Engine-side:

- **O3 — Where C1 to C4 are recorded** (and C5, found while filing). CLA proposed: C1 and C2 in
  `known-gaps.md`, C3 and C4 in `bugs/`.
  - **Answered 2026-09-28** `[operator]`: *"C1~C4도 inflight에 기록해줘"* —
    record C1 to C4 in `docs/inflight/` too.
  - **As filed:**
    - C1, C2 and C4 are `known-gaps.md` entries under WAL. C4 is filed as a
      spec self-contradiction plus an unpriced cost, not as a defect, because
      `stream.hpp` makes the behaviour deliberate.
    - C3 is a `bugs/` file.
    - C5 was found while filing and has its own `bugs/` file.
- **O4 — Whether segment recycling waits for archiving.** Today §11-4 ties
  them together, so recycling cannot exist before wal.md §13's archive seam.
  The alternative is to recycle below the redo start with no archive, which
  gives up PITR for those segments. This is a durability decision, not a
  device one.
  - **Answered 2026-10-07** `[operator]`: BC-Q1 marked (a)
    (`instructions/v3.0.0/raft-marks-2026-10-07.md` §2). Recycling does not
    wait for archiving; when archiving is built, it adds its floor to the
    recycling bound (`workorder-bc-wal-recycling.md` BC-R1).

Device-side:

- **O1 — Block-layer mirror or engine mirror.** This note proposes md RAID1.
  The engine-level alternative (§1) would let KDS take whichever write
  completes first, as Smart Flash Log does. That is a feature.
- **O2 — The data role's redundancy.** RAID1 or RAID10, and whether it shares
  a controller or PCIe root with the log pair.
- **O5 — Where appliance material lives.** This tree states the engine. A
  BOM, a golden OS image and a validation record belong to the appliance and
  may belong outside `docs/`.
- **O6 — `LimitNOFILE` in the unit, or `setrlimit` in the engine.** The first
  is deployment and needs no code. The second is a change to the platform
  layer (`docs/rules/rules.md` #4). Either treats C3's symptom, not its cause,
  C1.
