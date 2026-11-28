# Work order EJ - what leaves the instance: a feed of committed changes, and a second copy that can serve, explored from the one log

Status: **WRITTEN 2026-11-28, not opened.** Written on worktree
`service-txn-orders` (branch `worktree-service-txn-orders`) from
`72580aa04` (`v3.5.1-6-g72580aa04`) on the operator's word of 2026-11-28
(`raft-marks-2026-11-28.md` §6, §7). **Exploratory, with two proofs of
concept behind its census**: EJ-S0 reads and measures, and decides which
arm of §2 is carried to a proof; no stage is started and EJ-S0 waits for
the operator's word and for the marks on §4's items. Every `[source-read]`
claim with a `path:line` was read by CLA at `72580aa04` in the session
that wrote this order.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/wal.md` §1, §4, §5.2, §11; `docs/spec/backup.md`;
`docs/spec/txn.md` §4.1; `docs/spec/heap-and-tuple.md` §4.1, §4.1a;
`docs/spec/parser-v2.md` I11a; `docs/conceptnotes/cn12-read-resumed-by-key.md`
(CN-12); `docs/conceptnotes/cn9-dedicated-mirrored-wal-device.md` (CN-9);
EI, EK and EL, written the same day.

## 0. What EJ is

`[operator]` The operator's premise of 2026-11-28 and the word that asked
for this order are EI's §0. C2 is the survey's second constraint: **no
change data capture, no logical decoding, no `LISTEN`/`NOTIFY`, and no
replication.**

`[design]` Those are two needs with one source. A service publishes what
it committed to other services (**a feed**: the outbox's relay, a search
index, a cache), and an operator keeps the instance's data servable when
the instance is lost (**a standby**). Both read the one thing this engine
already makes durable in one order - its log - or read the data through a
snapshot. EJ explores each from the tree as it stands and builds neither
before its census: **the feed's first question is whether a client can
already read committed rows in commit order with no row skipped, and the
standby's is how far the existing archive and restore already are from
one.**

## 1. The tree as read `[source-read]`

### 1.1 Nothing leaves but backups and sealed segments

- "A logical dump and a streaming standby are not built"
  (`docs/spec/backup.md:171-173`). Replication is named as later work
  only: the log is "the substrate for point-in-time recovery and, later,
  replication" (`docs/spec/wal.md:20`), and "the sealed-segment/stream tap
  is the future log-shipping source. No format concession needed now; the
  constraint is only 'never break §4 self-description'"
  (`docs/spec/wal.md:234`). `txn.md` §1 lists "later the WAL-replication
  ack timeout" among the quantities sized by the 10 s transaction
  (`docs/spec/txn.md:14-17`).
- **The archive ships every sealed segment, in order, fail-closed**: on
  under `archive_dir` or `archive_command`, each segment below the durable
  LSN's archived before it can be removed, every archived name carrying
  the lineage, and writes refused by name while archiving fails and the
  WAL filesystem runs short (`docs/spec/backup.md:118-152`). A segment is
  64 MiB, a constant of the volume (`docs/spec/wal.md:60`).
- **Restore is mount recovery**, from the backup's start, refusing to
  open short of its stop; a target is an LSN, "the commit record carries
  no time"; and every restore takes a new lineage
  (`docs/spec/backup.md:9-21`, `:141-144`, `:154-158`).

### 1.2 What the log says about a row

- The log is physiological: "each record targets one page and is
  idempotently replayable". `HEAP_INSERT` carries the slot and the tuple's
  bytes with its Keystone word and writer id; `HEAP_OVERWRITE` the new
  version; `HEAP_DELETE_MARK` the flag and the writer; the before-image is
  the `UNDO_WRITE` that precedes an `UPDATE`'s overwrite; a spilled value
  is its own `VARHEAP_APPEND` (`docs/spec/wal.md:96-103`, `:178-179`).
- **A record names a page, not a relation.** The relation's oid is in the
  page's common header, stamped by `PAGE_INIT` (`docs/spec/wal.md:99`),
  and a tuple's bytes mean nothing without the relation's columns as they
  stood at that LSN, which are catalog rows changed by ordinary logged
  writes (`docs/spec/txn.md:1404-1419`).
- **One stream gives every commit one LSN in one order**, and the
  instance read view carries a window `trx_id -> commit_lsn` for every
  transaction committed and not yet reclaimed (`docs/spec/txn.md:504-511`).

### 1.3 What a polling client meets today

- **Issued keys are in issue order, not commit order**: "issued ids are
  monotonic in issue order on every relation; named keys carry no order"
  (`docs/spec/heap-and-tuple.md:166`, §4.1a at `:268`). A row issued id 7
  may commit after the row issued id 8.
- **`FOR UPDATE` is the one locking clause**; `FOR SHARE`, `FOR KEY
  SHARE`, `FOR NO KEY UPDATE` and every option are refused by name
  (`docs/spec/parser-v2.md:135`), so two relays cannot divide one queue
  by skipping each other's claimed rows.
- **A statement can park and be woken across cores**: the lock family's
  waits, a streamed reply's back-pressure and the page reader all park a
  statement, and the one cross-core path is write-then-kick through the
  instance's wake registry (`docs/spec/sched.md:66-68`). Nothing parks a
  read until "a row was committed".

## 2. The candidates `[design]`

None is designed past what S0 needs to judge it.

### 2.1 The feed

- **F0 - the outbox as a relation, polled, nothing built.** A service
  inserts its message row in the transaction that did the work; a relay
  reads `WHERE keyidentity > <last seen>` and publishes. **Its defect is
  §1.3's first item**: a relay that saw id 8 and remembers 8 never reads
  id 7 when its writer commits later. `[quiet-wrong]` for the consumer:
  a committed message never delivered, with no error anywhere. The
  field's workarounds - a relay that waits out every open writer, a
  single writer, a gap list - are the client's.
- **F1 - a read in commit order.** The engine answers "the rows of `t`
  committed after position `p`, and the position this answer is complete
  up to", `p` a commit LSN. The position is the instance's own order
  (§1.2's third item), so no row is skipped and none repeated. What S0
  must find: where a row's commit position can be read once its writer's
  entry has left the view's window (the header carries a `trx_id`, not an
  LSN), and what bounds the answer when it cannot.
- **F2 - the log decoded.** A reader of sealed segments (the archive's
  tap) turns records into row changes: page to relation by the page's
  owner, bytes to columns by the catalog as of that LSN, before-images
  from undo, only winners. It is complete and ordered by construction and
  off the statement path; it needs a catalog history the engine does not
  keep in decodable form, and its lag is a segment's fill time unless it
  reads the unsealed tail.
- **F3 - a read that waits.** A statement that parks until a commit
  touches a named relation (or F1's position moves), in place of a
  client's poll interval: `LISTEN`/`NOTIFY`'s use without its channel
  namespace. It rides on F0 or F1 and is nothing alone.

### 2.2 The standby

- **H0 - the archive and restore as they are.** A second host restores a
  backup and every archived segment after it. It is a cold copy: it does
  not open short of a stop, and each restore draws a new lineage.
- **H1 - a warm standby by continuous restore.** A mounted instance that
  keeps applying archived segments as they arrive and serves nothing (or
  `READ ONLY` at its applied point) until promoted. Its loss bound is one
  unsealed segment plus the archive's lag; its distance from H0 is S0's
  census.
- **H2 - the log's tail shipped.** A peer receives appended bytes as they
  become durable; `strict` and `group` may wait for its acknowledgement,
  under the ack timeout `txn.md` §1 already names. Zero-loss failover, a
  second durability point on every commit, and a protocol of its own.

## 3. Rules

- **EJ-R1. A feed delivers every committed change of what it covers, in
  commit order, and nothing uncommitted** `[quiet-wrong]`: a skipped
  commit, a rolled-back row published, or two commits reordered are each
  a wrong answer no client can detect.
- **EJ-R2. A feed position is the instance's commit order**, never a
  tuple key, a wall clock or a transaction id; a position that cannot be
  honoured (recycled log, a purged version) is **refused by name**, never
  answered from what is left.
- **EJ-R3. A feed never fails or delays the commit it reports**, and
  holds no lock a writer waits on. What it may hold - the read horizon, a
  log segment against recycling - is stated with its bound.
- **EJ-R4. A standby that serves, serves a state the primary committed**:
  a prefix of the log, whole transactions only, recovery's own redo and
  no second one (`backup.md` B1).
- **EJ-R5. A promoted standby takes a new lineage** (`backup.md` B12),
  so the old primary's log can never be applied over it.
- **EJ-R6. The log's self-description is not broken** (`wal.md:234`): no
  candidate changes a record's format to suit a reader unless the
  operator marks the superblock version it costs.
- The standing rules, as DT's order lists them; the measurement budget
  applies to S0's and S3's runs.

## 4. Items for the operator (EJ-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| EJ-Q0 | Is EJ opened, and how far? | (a) S0 only, both arms; (b) S0 and the feed's proof S1; (c) not opened | (a) |
| EJ-Q1 | One order or two | (a) one order through S0, split into a feed order and a standby order at S0's verdict if both go on; (b) split now | (a) |
| EJ-Q2 | Which need comes first if only one proof is carried | (a) the feed: it is what a service's own code is written against, and F0's defect is silent; (b) the standby | (a) |
| EJ-Q3 | The feed's first proof | (a) F1 over one relation, behind a cell that first shows F0's skip on this engine; (b) F2; (c) F0 documented as the recipe with its workaround, nothing built | (a) |
| EJ-Q4 | Many relays on one queue (`SKIP LOCKED`'s use) | (a) outside EJ: a feed with positions has one reader per position by construction, and a second locking-clause option is `parser-v2.md` I11a's to open; (b) inside | (a) |
| EJ-Q5 | The standby's first proof | (a) H1's distance measured in S0 and built only on the operator's word; (b) H2 designed now; (c) H0 documented as the standby procedure, nothing built | (a) |
| EJ-Q6 | A commit that waits for a second copy (H2) | (a) not decided by this order: it is a new meaning beside `strict`, `group` and `relaxed` and the operator's to name; (b) decided now | (a) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **EJ-S0** | The census and the premise. **Feed**: a cell on the two-core rig that makes F0's skip happen (two writers, the lower id committing second, a polling reader between), so the defect is `[measured]` and not argued; where a commit position is and is not readable for a row (F1); what F2 would need of the catalog, read against `catalog.md` and `ddl-transactional.md`. **Standby**: H0 run once on this host - a backup, a load that fills segments, a restore to `latest` - reporting the seconds a segment takes to fill under a named load (H1's loss bound) and to apply (its catch-up rate), in `build-release`, inside the stage budget | a results file under `bench/<version>/`, named by `git describe --tags`; a verdict per arm; the operator's word |
| **EJ-S1** | (gated on EJ-Q3) The feed's proof of concept: F1 over one relation, EJ-R1 pinned by a cell that runs the S0 skip shape and a history of commits against the feed's output | the suite; what the proof did not carry (relations, DDL, retention) |
| **EJ-S2** | (gated on EJ-Q5) The standby's proof of concept, if the operator opens one | the sim corpus and the suite |
| **EJ-S3** | The close: the owning spec sections (`backup.md` §5's "does not do" list, a feed's section where one was built), `known-gaps.md`, the overhead measurement over the milestone's change | the operator's word on landing |

## 6. What EJ does not do

- No multi-primary, no sharding, no automatic failover or election.
- No message broker inside the engine: no topics, consumer groups,
  delivery acknowledgements or retries. A feed ends at a position.
- No trigger, no stored procedure, no `LISTEN`/`NOTIFY` channel names.
- No connector for a named external system.
- No change to what `strict`, `group` and `relaxed` promise (EJ-Q6).
- Nothing of CN-9's mirrored log device, which is the same host's.
- Nothing of EL's token: a relay's at-most-once publication is its own
  position, and a service's at-most-once commit is EL's.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-28

On `service-txn-orders` from `72580aa04`, on the operator's word. No stage
is opened and no code is written. F0's skip is `[design]` until EJ-S0's
cell shows it: it follows from §1.3's first item and has not been run.
