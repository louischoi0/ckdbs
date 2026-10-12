2026-10-12 03:13 UTC

**Report: how a bulk writer stalls readers on every core, read-only, in worktree `bl-session-balancer` at `a3208be7`.** No build, test or server was run. The uncommitted BL diff touches no `src/wal/` code. Both traces' citations hold where re-read. Trace A has one error that changes the fix: making `TouchPattern` unlogged is not a free change (see 2b).

## 1. Mechanism

1. **[source-read]** Core 3's 1,024-row INSERT appends every record under `WalStream::latch_` (`src/wal/stream.cpp:259`). Past half a segment it asks the preparer thread to build the next 64 MiB body (`stream.cpp:291-295`). That build does an fallocate, a 64 MiB zero write and an fsync, and competes for the device (`file_log_device.cpp:358-374`).
2. **[source-read]** Core 0's drain runs after every task and on a 1 ms timer. Because core 3 keeps appending, `appended != durable`, so the early-out never fires (`manager.cpp:483`). Every 10 ms (`manager.hpp:115`) the drain calls `RequestSyncNow`, which does a latched `Flush` and wakes the writer (`manager.cpp:533-539`, `:300-309`). The writer then calls `SyncDevice` holding the sync gate, which runs `fdatasync` on **every** live segment (`file_log_device.cpp:544-565`). Under a bulk load the gate is held almost continuously.
3. **[source-read]** When a segment fills, `Append` runs the whole roll while still holding the latch (`stream.cpp:262-268`):
   - `SealLocked`, then `FlushLocked`. This yield-spins on any unlatched flush in flight, then does a `pwrite` (`:217`, `:345-356`).
   - `Roll`, then `StartSegment`, then `SyncDevice`, which waits out the writer's sync and then runs its own all-segment sync (`:95-97`, `:73-78`).
   - `CreateSegment`: waits on `prepare_mutex_` if the body is not yet built, then writes the header and `fdatasync`s it, renames, and fsyncs the directory (`file_log_device.cpp:382-441`).

   The 4.14 s that core 3 spent in `wal_sync_gate` is only the gate part of this. The `prepare_mutex_` wait and the directory fsync are not counted.
4. **[source-read]** `Latch::lock` is a plain blocking `std::mutex::lock` (`include/kds/base/latch.hpp:70-75`). A reactor that hits a held latch serves none of its sessions until the latch is free.
5. **[source-read]** Core 0's D3 flush takes the same latch (`stream.cpp:308`), so core 0 blocks for nearly every roll. That is why its `wal_stream` (4.13 s) matches core 3's gate wait (4.14 s).
6. **[source-read]** On cores 1 and 2, a pk point SELECT seen for at least the second time (`kAutoRecordThreshold = 2`, `trail_recorder.hpp:72`) writes a trail (unlogged) and then calls `TouchPattern` (`trail_recorder.cpp:125-156`). That call:
   - holds the `sys.patterns` page exclusively (`ForFirstRow` with `kWrite`, `catalog.cpp:162-184`);
   - writes the row through `OverwriteLogged` (`catalog.cpp:2864`), which ends in `wal->Append` (`:247`) and needs the stream latch.

   This happens once per repeated statement.
7. **[inference]** A reader blocked on the stream latch is still holding page 9 (`sys.patterns`). The other reader core's `TouchPattern` then blocks on page 9's latch, and that wait is counted as `page_latch_waits`, not `wal_stream`. This would explain why core 1 has the same >100 ms holds as the other cores with only 0.69 s of `wal_stream`: whichever core reaches the stream latch first is charged for it. A checkpoint turn falling on one core (`checkpointer.cpp:28`, `:298`, `:581`) is the other candidate. Comparing per-core `page_latch_waits` and `checkpoint_runs` would decide between them.
8. **[inference]** Every session on a stalled reactor waits for the whole roll. That gives the 0.1-0.7 s holds and the 325 ms p99. Core 3's own table-walk session also stalls behind core 3's roll.

## 2. Smallest decoupling changes

**(a) Core 0's drain skips while a roll is running.** Set an atomic `rolling_` under the latch for the duration of `Roll`. `DrainOnce`'s D3 branch and its `pending_group_commits_` branch would check it and return without updating `last_sync_ns_`, so the next 1 ms tick retries.
- *Correctness:* this adds **zero** to the loss window. While the roll holds the latch, no append can finish. The roll itself flushes the seal and syncs the predecessor, then publishes it durable (`stream.cpp:95-97`). A waiting group committer therefore becomes durable through the roll's `PublishDurable` and is resolved on a later tick by `ResolveBatches`.
- *Fail-stop:* unchanged. A failed roll sets `stopped_` before `rolling_` is cleared.
- *Blast radius:* `manager.cpp` (`DrainOnce`) and one atomic in `stream.hpp`.
- *Spec touched:* `wal.md` §1 (the D3 row) and §6 steps 2-3. It keeps the asymmetry argument in `manager.cpp:470-482`.
- *Leaves open:* core 0's 50 ms writeback tick, which waits on the WAL gate with no defer (`device_page_store.cpp:1809-1830`). The fix there is to defer a page whose LSN is not yet durable, as the checkpoint already does (`wal.md` §11 step 2).

**(b) Take `TouchPattern` off the statement path.** Trace A's proposal, passing `nullptr` for the WAL, **conflicts with `docs/rules/rules.md:67`**. That rule allows `sys.access_stats` as the only unlogged catalog relation, and admits another only if its content's class is purely a statistic. `sys.patterns` also holds `waystone_root` and `dir_depth`, which readers act on. Mixing unlogged writes into a logged page also risks a torn page with no full-page image (FPI) in range, and a torn catalog page refuses the mount. Two options stay within the rules:
- **Coalesce:** keep a per-core in-memory map of `fetch_id` to (use-count delta, `last_seen`) in `TrailRecorder`, and fold it into the page with a logged write on a `system`-group tick, skipping while `rolling_` is set. Only two readers consume heat: `relayout_planner.cpp:69-71` (heap-only, and dark since SUS-1) and `SHOW` (`catalog_view.cpp:160-169`). A crash loses at most one tick of heat, and failures are already ignored (`trail_recorder.cpp:153-156`). Blast radius: the recorder and one tick.
- **Delete `TouchPattern`:** this removes the source, in line with the operator's preference for removal. It needs an operator ruling, because heat would then exist only as zeros.

**(c) Release the latch for the roll's I/O.** This is the structural fix and the only one that also helps writers on other cores:
- Under the latch: seal, run `FlushLocked`, advance `append_lsn_` to the new segment's first LSN, and claim `rolling_`.
- Without the latch: `SyncDevice`, `PublishDurable`, `CreateSegment`.
- Then release the claim. `Flush`, `FlushLocked` and `DetachBelow` wait out the claim, the same way they already wait out `flush_in_flight_` (`stream.cpp:190-203`, `:323-326`).

Risks:
- It rewrites the `wal.md` §6 step 1 statement that the whole append, "any segment roll" included, runs under the latch. That is the AL-R1 boundary, which was decided once already.
- The `wal.md` §4.1 roll order (predecessor synced before the successor exists) still holds, because the steps stay sequential.
- Fail-stop: a failed roll must stop the stream before it releases the claim, as an unlatched flush already does.
- **New hazard:** appenders keep staging into the 1 MiB ring. When it fills, `Append` gets `OutOfSpace` and retries the drain 4 times before fail-stop (`manager.cpp:369-386`). Those drains must block on the roll and not count as attempts, or the change turns a slow roll into a stopped log.

Blast radius: `stream.cpp` and `stream.hpp`, plus the roll and fail-stop tests.

**(d) Lesser item.** `FileLogDevice::Sync` could `fdatasync` only the tail segment. Every earlier segment was already synced at its own roll. The gain is unmeasured and may be small, since an `fdatasync` on a clean file is cheap [inference].

## 3. Log admission / throttling

This is secondary: it lowers how often rolls happen and so how much time is lost, but every roll still stalls every reactor. It **cannot sit inside `Append`**. A record is appended after its page has already been changed (`wal.md` §6 step 1), so an append cannot be refused or parked without a fail-stop. What it would need:
- **Admission point:** at the statement boundary, before bind. A 1,024-row VALUES is one statement, so it would have to park between statements, and also between rows of a multi-row INSERT if finer pacing is wanted. Parking while holding row X locks pushes the delay onto lock waiters.
- **Accounting:** the per-core `stats_.bytes_appended` delta across the statement. Write statements do not yield, so the delta is attributable, with lock-wait parks as the exception.
- **Budget:** a token bucket keyed by the BL priority level (W5/W6). This is a new knob, so it must be shown that no existing setting already expresses the quantity (CLAUDE.md working rules).

Rolls happen once per 64 MiB, so a budget of X MB/s caps the stall fraction at about (roll time × X / 64). The fault stays in place.

## 4. Verdict on placement

Core placement cannot isolate interactive reads from this coupling. The coupling runs through objects that belong to the whole instance, not to any one core:
- one stream latch (AR0 M0, `wal.md` §3);
- core 0's 10 ms drain, which by design must cover every peer's bytes (`manager.cpp:470-482`), so core 0 stalls whichever core the writer is on;
- the shared `sys.patterns` page 9, which every core's recorder writes;
- the checkpoint rotation, which writes back the shared pool from every core.

Moving the bulk session only changes which slot shows the gate wait. If (a) and (b) land, placement becomes a useful addition rather than a fix. Interactive sessions kept off core 0 and off the writer's core would then meet the latch only on their core's checkpoint turn. Until then, BL's per-transaction placement cannot buy read isolation against a bulk writer.