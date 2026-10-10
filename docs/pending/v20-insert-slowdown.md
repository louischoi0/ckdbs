# Pending: the xrock trade load ran ~3x slower after the move to superblock 20

Status: **PENDING** - the exact investigation is deferred by the operator.
Decides nothing, opens no stage.
Raised: 2026-10-08, in conversation with the operator.
Read at: `0446b3b0` (`[source-read]` line numbers are at this commit unless
another is named). Measured binaries: `v2.7.0-640-gb76261bb` (superblock 19),
`v2.7.0-659-ge4b107af` (superblock 19 - BE's branch before BD merged, not
v20), `v2.7.0-688-g19843fee` (superblock 20, the binary xrock ran).
Related: `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md`
(BD-R10 - nothing BD lands is measured; BD-R12 - a split is one record);
`docs/spec/wal.md`; `bench/README.md`;
`bench/v3.0.0/results-be-close-v2.7.0-665-geef442cf.md` (single-row insert
flat across BE, superblock 19 on both arms).

## 1. What prompted it

`[operator]` *"By superblock, inserts on v20 are confirmed
markedly slower than on v19, by about 3x. Find the cause"*. After a first
pass the operator deferred it: *"I will put off the exact investigation of
this issue for now; leave it in a document"*.

The load is xrock's `fetch` (`/home/cdkbs/xrock`, outside this tree): one
session, `CREATE TABLE ... BTREE EXPLICIT` with five columns (`id int64, ts
int64, side int8, price decimal(18,8), qty decimal(18,8)`), 1024-row
multi-row `INSERT`s, named pks ascending (`day << 25 | seq`),
`durability = group`, `checkpoint_interval_ms = 5000`. Each day's archive
is downloaded whole over HTTP before it is inserted.

## 2. What the first pass found

Preliminary: the measurements below broke `bench/README.md`'s rules (a
scratch driver, a host shared with xrock's own parallel load and other
sessions' suites), so none of them is a results file.

### 2.1 The compared runs differ in more than the superblock

From xrock's server logs (`var/kds/kdb.log*`, `var/kds/server.out`) and its
fetch logs (`var/*.log`):

| run | server | `cores` | rows/s, download included |
|---|---|---|---|
| `fetch90.log`, 2026-10-07 01:31 - the per-day baseline | **superblock 17** (mounted 2026-10-06 08:12) | 1 | 07-09..07-28: 30,150,676 rows in 600 s, 50.3k |
| `fetch30b.log`, 2026-10-07 07:23 - the only v19 trade load | superblock 19, fresh volume | 1 | 09-07..10-06: 52,222,432 rows in 764 s, 68.3k |
| `fetch90_v20.log`, 2026-10-08 05:46 | superblock 20, fresh volume | **4** | 07-09..07-28: 30,150,676 rows in 777 s, 38.8k |

- xrock's `conf/kds.conf` went from `cores = 1` to `cores = 4` at 2026-10-07
  11:48, under superblock 19. No trade load ran on v19 at `cores = 4`.
- The "3x" is the per-day ratio of the v20 run to the **v17** run on its
  first six days: 3.16, 2.04, 3.22, 3.44, 2.69, 2.39 (07-09..07-14). On the
  next eight days (07-15..07-22) the same v20 run, with the same binary and
  configuration, was 0.68-1.05x of v17.

### 2.2 Inside the v20 run, the time is in four stall windows

From the per-anchor `durable_lsn` series in the v20 server log:

- In four windows - 05:47:01-05:47:32, 05:48:59-05:49:38, 05:50:26-05:51:05
  and 05:52:04-05:52:32, 137 s in all - the WAL grew 37-50 MB each. At 504
  B/row that is about 3k rows/s. The bursts around the windows ran at
  60-80k rows/s, and after 05:52:40 no window recurs.
- The windows are slow inserts, not an idle server: an idle `cores = 4`
  server logs about 80 bytes per checkpoint (05:46:10-05:46:11).
- Days 07-09..07-14 took 385 s on v20 against 143 s on v17. The windows,
  plus slower bursts before them, are the difference.
- The gaps between days (the download) are 1-2 s in the v20 run, so the
  per-day times are mostly insert time.
- A per-row engine cost does not turn on and off within one run, so the
  windows point at the I/O path. What the host's I/O was at 05:46-05:52 was
  not recovered.
- An unrelated anomaly, not investigated: from 05:55:38 to 05:57:22 the WAL
  grew 3,764 MB, where the three days' rows account for about 1,780 MB.

### 2.3 Controlled A/B, same host, same shape

The driver replays xrock's shape on a fresh volume, one session, 1,048,576
rows (archived at `/home/cdkbs/bench-runs/v20-insert-slowdown/xload.py`,
with the binaries' hashes).

| | v19 `b76261bb` | v20 `19843fee` |
|---|---|---|
| `cores = 1`, wall, two interleaved pairs | 19.16 / 23.43 s | 19.37 / 13.01 s |
| `cores = 1`, server CPU | 7.27 / 4.57 s | 6.68 / 4.37 s |
| WAL per row | 446.5 B (`e4b107af`: 446.5 B) | **504.0 B (+12.9%)** |
| 262,144 rows under `strace -c`, `fdatasync` / `fsync`, `cores` 1 → 4 | 378 / 45 → 590 / 72 | 392 / 44 → 577 / 75 |

- **At `cores = 1`, v19 and v20 are the same** in throughput and server CPU.
  At equal `cores` they make the same number of syncs. Above one core both
  versions sync about 50% more.
- **`cores = 4` could not be measured on this host.** The measured server's
  peers were pinned to the CPUs xrock's server peers held, and the same v19
  binary took 21.78 s and 187.68 s. Moving the peers to CPUs 4-7 with an
  `LD_PRELOAD` shim (`pinshift.c`, archived) gave v20 at 512k rows 13.48 /
  24.50 s at `cores = 1` and 15.78 / 8.90 s at `cores = 4`: no resolvable
  difference.
- **The shape is I/O-bound.** Sampled server threads sat in
  `rq_qos_wait`, `folio_wait_bit_common`, `jbd2_log_wait_commit` and
  `do_get_write_access`, on the reactor thread itself. The host's I/O PSI
  read `full avg300 = 32.56` during xrock's parallel load (06:36 UTC).

### 2.4 The one engine difference the shape exposes: the new leaf's image

- `[source-read]` Before BD-S2, an insert logged each structural change on
  its own. A new leaf was a `PAGE_INIT`, which the row's `HEAP_INSERT` then
  filled, and every other page was a `FULL_PAGE_IMAGE`
  (`4debe8d9:src/server/command_dispatcher.cpp:4293`).
- `[source-read]` A btree insert now skips that loop
  (`src/server/command_dispatcher.cpp:4317`) and logs one `BTREE_SPLIT`
  (`:4350`). `LogBtreeSplit` images every page in `changes()`, the new leaf
  included (`include/kds/storage/log_page_image.hpp:62`), at 8 + 8192 bytes
  each (`include/kds/wal/payload.hpp:493`).
- `[design]` On an ascending load that is one extra 8 KiB image per append
  split. The measured +57.5 B/row puts a split at about every 140 rows on
  this schema, which is the leaf's capacity.
- `[design]` Even on a wholly WAL-bound path this costs at most about 13%.
  It does not account for 3x.

### 2.5 What `cores > 1` changes on this path

- `[source-read]` Every core schedules its own checkpoint at the cadence:
  peers at `src/server/core_runtime.cpp:539` (run gated at `:578`), core 0
  at `src/server/expeditor.cpp:1881`.
- `[source-read]` A run snapshots the shared pool's whole dirty table
  (`src/wal/checkpointer.cpp:155`) and flushes it to completion on that
  core's reactor (`:305`). It then syncs the data file
  (`src/storage/device_page_store.cpp:2101`), makes `CHECKPOINT_END` durable
  (`src/wal/checkpointer.cpp:269`) and publishes the anchor.
- At `cores = 4` that is four checkpoints per 5 s for the instance where
  `cores = 1` has one, matching the extra syncs counted in §2.3.
- `[source-read]` Only peers are pinned (`src/server/expeditor.cpp:1825`).
  Core 0 inherits the process's affinity.

## 3. What is not established

- What made the four windows of §2.2: host I/O from other work, the kernel's
  writeback, the volume's burst budget (an Azure NVMe-attached managed
  disk), or the engine's own I/O pattern at `cores = 4`.
- Whether `cores = 4` costs a single-session load anything on a quiet host.
- Whether v20 costs more than its 12.9% WAL on any shape. Only this
  ascending named-key shape was measured, and BD-R10 measured nothing.

## 4. Options for the investigation

| option | what | answers |
|---|---|---|
| (a) | On a quiet host: {v19 `b76261bb`, v20} × `cores` {1, 4}, at least three interleaved runs each, with the archived driver, `/proc/pressure/io` and `iostat` sampled | whether `cores = 4` or v20 costs anything beyond §2.4 |
| (b) | Re-run xrock's own fetch at v20 with `cores = 1` over 07-09..07-28 on a quiet host | the comparison the report needs, with only the superblock changed |
| (c) | Reproduce a stall window and capture it: PSI, per-thread `wchan`, and the checkpoint's debug line (`pages_flushed` per run) | what the windows are |
| (d) | Only if (a)-(c) put cost on the WAL: shrink the new leaf's image in `BTREE_SPLIT`, e.g. elide the page's free hole | a WAL format change that BD-R12's one-record atomicity must survive; the operator's decision |
| (e) | Only if (a) or (c) puts cost on `cores > 1`: one checkpoint cadence for the instance rather than one per core | touches AT-S8's per-core checkpoint; the operator's decision |
