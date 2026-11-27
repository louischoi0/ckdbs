# Milestone register

One row per milestone work order across `instructions/<version>/`, plus a
row for milestone work that lives on a branch with no order on `main`. **Each
order's status header is the authority - its row in its version's
`index.md` where the header carries none (AP, AX, AY, AZ, BI, BJ, BW, CJ) -
and this file is the index to it**,
and a row is updated in the same commit that changes its order's status.
Dates are `YYYY-MM-DD`; `last` is the latest of `written`, `opened`,
`closed` and the date of the `landed` commit, and is the sort key. `landed`
is the commit by which the order's work reached `main` (the one the order
names, else the commit that recorded its close; for an open order, its last
landed stage), `n/a` where there is nothing to land and `-` where nothing
has landed. `tag` is the first version tag containing that commit; `(local)`
marks a tag not on `origin`. As of `c22a1a95` (`origin/main`, 2026-11-22),
with DG's close record `e98163d2`.

## Queries

Run from the repository root.

```sh
# one milestone (two rows where a letter has two orders: AM, AT)
grep '^| DN ' instructions/milestones.md

# the 10 most recent (rows are kept sorted by last, newest first)
grep -E '^\| [A-Z]{2} ' instructions/milestones.md | head -10

# by status: open, written, paused, closed, closed-narrow, closed-paper, abandoned, branch-only
grep -E '^\| [A-Z]{2} \| open ' instructions/milestones.md

# not yet closed
grep -E '^\| [A-Z]{2} \| (open|written|paused|branch-only) ' instructions/milestones.md

# by month, or by day, of the last event
grep -E '^\| [A-Z]{2} \| [a-z-]+ \| 2026-10-' instructions/milestones.md
grep -E '^\| [A-Z]{2} \| [a-z-]+ \| 2026-11-21 ' instructions/milestones.md

# alphabetical by id
grep -E '^\| [A-Z]{2} ' instructions/milestones.md | sort

# landed but untagged
awk -F' *[|] *' '$2 ~ /^[A-Z][A-Z]$/ && $8 ~ /^[0-9a-f]+$/ && $9 == "-"' instructions/milestones.md

# carried first by a given tag
grep -E '\| v3\.4\.15( \(local\))? \|' instructions/milestones.md

# columns by number: 2 id, 3 status, 4 last, 5 written, 6 opened, 7 closed,
# 8 landed, 9 tag, 10 title, 11 order
awk -F' *[|] *' '$2 ~ /^[A-Z][A-Z]$/ {print $2, $3, $8, $9}' instructions/milestones.md

# closed in a given month, by the closed column
awk -F' *[|] *' '$2 ~ /^[A-Z][A-Z]$/ && $7 ~ /^2026-11-/ {print $7, $2, $3}' instructions/milestones.md | sort
```

## Status vocabulary

- `written` - written, not opened.
- `open` - opened, not closed; includes an order built but not closed (BS).
- `paused` - opened, then stopped on the operator's word until resumed.
- `closed` - closed.
- `closed-narrow` - closed with a narrower scope than written.
- `closed-paper` - closed on paper, on the operator's word.
- `abandoned` - closed with nothing built.
- `branch-only` - work on a branch with no order on `main`.

## Register

| id | status | last | written | opened | closed | landed | tag | title | order |
|---|---|---|---|---|---|---|---|---|---|
| DV | closed-narrow | 2026-11-27 | 2026-11-26 | 2026-11-27 | 2026-11-27 | - | - | pages no root reaches: counted, then reclaimed | v3.2.0/workorder-dv-unreachable-page-reclaim.md |
| DW | closed | 2026-11-27 | 2026-11-26 | 2026-11-27 | 2026-11-27 | - | - | what a btree relation's deletes leave, purged unasked: index entries first | v3.2.0/workorder-dw-btree-purge-driver.md |
| EE | written | 2026-11-26 | 2026-11-26 | - | - | n/a | - | the decide slot: one wake behind every wait for a transaction's decide | v3.2.0/workorder-ee-decide-slot.md |
| ED | written | 2026-11-26 | 2026-11-26 | - | - | n/a | - | backup, a second pass: recovery to a time, incremental, compressed and encrypted, a snapshot's torn page healed | v3.2.0/workorder-ed-backup-second-pass.md |
| EC | written | 2026-11-26 | 2026-11-26 | - | - | n/a | - | a multi-core instance crashed and mounted under a seed: the simulator given cores | v3.2.0/workorder-ec-multi-core-crash-harness.md |
| EB | written | 2026-11-26 | 2026-11-26 | - | - | n/a | - | the key forms in the executor: ranges, joins by a whole key, an elided order, an upsert by any key | v3.2.0/workorder-eb-key-forms-in-the-executor.md |
| EA | written | 2026-11-26 | 2026-11-26 | - | - | n/a | - | a prepared statement parsed once, and the compile's reuse put before the operator again | v3.2.0/workorder-ea-prepared-statement-reuse.md |
| DZ | written | 2026-11-26 | 2026-11-26 | - | - | n/a | - | what a checkpoint still does on its reactor: the dirty-table walk, the page writes, the map barrier | v3.2.0/workorder-dz-checkpoint-reactor-residue.md |
| DY | written | 2026-11-26 | 2026-11-26 | - | - | n/a | - | a WAL segment roll that does not hold the append latch across its I/O, a proof before any code | v3.2.0/workorder-dy-segment-roll-off-the-latch.md |
| DX | written | 2026-11-26 | 2026-11-26 | - | - | n/a | - | the buffer pool under pressure: a full pool that waits for its writeback, an abort never refused a frame | v3.2.0/workorder-dx-pool-under-pressure.md |
| DT | closed-narrow | 2026-11-26 | 2026-11-26 | 2026-11-26 | 2026-11-26 | 6e43ec8c | - | a write that gives its core back: the write path's page miss off the reactor, a write walk that yields | v3.2.0/workorder-dt-write-path-yield.md |
| DU | closed | 2026-11-26 | 2026-11-26 | 2026-11-26 | 2026-11-26 | `5f9b6546` | - | a write that finds its rows by a key: a bound on the tuple key, an index | v3.2.0/workorder-du-keyed-write.md |
| DS | closed | 2026-11-26 | 2026-11-26 | 2026-11-26 | 2026-11-26 | f42341d3 | - | four contained known gaps: the backup tool behind authentication, a hung archive command, two refusals with no byte | v3.2.0/workorder-ds-contained-known-gaps.md |
| DR | closed | 2026-11-23 | 2026-11-23 | 2026-11-23 | 2026-11-23 | 71a148e1 | - | a primary key of any type and column count over an unchanged Keystone | v3.2.0/workorder-dr-primary-key-over-keystone.md |
| BG | closed-narrow | 2026-11-23 | 2026-10-07 | 2026-10-07 | 2026-11-23 | b27916f3 | v3.1.0 | scan resistance: a scan's frames reclaimed before the hand laps | v3.0.0/workorder-bg-scan-queue.md |
| BS | closed-narrow | 2026-11-23 | 2026-10-13 | 2026-10-13 | 2026-11-23 | b5928bf2 | v3.4.0 | test hardening: the verification methods of a production engine | v3.0.0/workorder-bs-test-hardening.md |
| DQ | closed | 2026-11-23 | 2026-11-22 | 2026-11-23 | 2026-11-23 | 38b5aa69 | - | a Cabin's miss answered from a map: the correlated probe's unobserved keys served by the build | v3.2.0/workorder-dq-cabin-miss-build.md |
| CW | abandoned | 2026-11-22 | 2026-11-17 | 2026-11-22 | 2026-11-22 | da5d5c05 | - | the optimistic page read, measured before it is built | v3.2.0/workorder-cw-optimistic-page-read.md |
| DM | abandoned | 2026-11-22 | 2026-11-20 | 2026-11-22 | 2026-11-22 | e3f6bb8b | - | a leaf that knows its own upper bound | v3.2.0/workorder-dm-leaf-upper-bound.md |
| DP | closed | 2026-11-22 | 2026-11-22 | 2026-11-22 | 2026-11-22 | 8fef04fa | - | VIEW: a stored SELECT absorbed by text before any plan | v3.2.0/workorder-dp-view.md |
| DO | closed | 2026-11-22 | 2026-11-21 | 2026-11-21 | 2026-11-22 | c22a1a95 | - | the frame table's long holds | v3.2.0/workorder-do-frame-table-long-holds.md |
| DN | closed | 2026-11-21 | 2026-11-20 | 2026-11-20 | 2026-11-21 | 48a3276c | - | the heap redefined again: an Oracle-style heap | v3.2.0/workorder-dn-heap-rowid.md |
| DL | closed | 2026-11-21 | 2026-11-20 | 2026-11-20 | 2026-11-20 | f0c95a09 | - | statistics queue no statement, an off consumer collects nothing | v3.2.0/workorder-dl-statistics-queue-no-statement.md |
| DK | closed-narrow | 2026-11-21 | 2026-11-20 | 2026-11-21 | 2026-11-21 | 46ee8a6e | - | a btree descent that leans on the immutable low key | v3.2.0/workorder-dk-descent-low-key.md |
| DG | abandoned | 2026-11-21 | 2026-11-18 | 2026-11-21 | 2026-11-21 | dcab41e5 | - | a WAL segment's body: what the zero fill and its absence cost | v3.2.0/workorder-dg-wal-segment-body.md |
| DD | closed | 2026-11-21 | 2026-11-18 | 2026-11-20 | 2026-11-20 | 2c2639df | - | secondary index entries removed: an index entry purge | v3.2.0/workorder-dd-index-entry-purge.md |
| DJ | closed | 2026-11-20 | 2026-11-20 | 2026-11-20 | 2026-11-20 | 56d57f16 | v3.4.18 (local) | connections, sessions and cursors | v3.2.0/workorder-dj-connections-sessions-cursors.md |
| DI | closed | 2026-11-20 | 2026-11-20 | 2026-11-20 | 2026-11-20 | fba5529f | v3.4.17 | a shape's statistics named by what the statement binds | v3.2.0/workorder-di-shape-identity.md |
| DH | closed | 2026-11-20 | 2026-11-20 | 2026-11-20 | 2026-11-20 | 799a767d | v3.4.17 | the page image's price where pages are written often | v3.2.0/workorder-dh-page-image-volume.md |
| DA | closed | 2026-11-20 | 2026-11-18 | 2026-11-19 | 2026-11-19 | 1eeab8ce | v3.4.15 | access statistics kept, their storage moved to memory | v3.2.0/workorder-da-access-stats-memory-resident.md |
| CV | closed | 2026-11-20 | 2026-11-15 | 2026-11-20 | 2026-11-20 | fda024cf | v3.4.16 | ORDER BY ... LIMIT served by the order a structure already holds | v3.2.0/workorder-cv-ordered-index-walk.md |
| DF | closed | 2026-11-19 | 2026-11-18 | 2026-11-18 | 2026-11-19 | 135d2570 | v3.4.15 | the statistics the engine records, widened | v3.2.0/workorder-df-statistics-coverage.md |
| DE | closed-narrow | 2026-11-19 | 2026-11-18 | 2026-11-19 | 2026-11-19 | 12887a14 | v3.4.15 | a page read that does not stop its core: the miss off the reactor | v3.2.0/workorder-de-page-read-off-the-reactor.md |
| DC | closed | 2026-11-19 | 2026-11-18 | 2026-11-19 | 2026-11-19 | 35086008 | v3.4.14 | SELECT ... FOR UPDATE: a read that claims the rows it returns | v3.2.0/workorder-dc-select-for-update.md |
| DB | closed | 2026-11-19 | 2026-11-18 | 2026-11-19 | 2026-11-19 | 480add9d | v3.4.15 | a torn page healed, not refused: the full-page image the log owes | v3.2.0/workorder-db-torn-page-healing.md |
| CZ | closed | 2026-11-19 | 2026-11-18 | 2026-11-18 | 2026-11-18 | 0f4edbd5 | v3.4.14 | a checkpoint that does not hold its reactor on a segment roll | v3.2.0/workorder-cz-checkpoint-roll-wait.md |
| CY | closed | 2026-11-19 | 2026-11-18 | 2026-11-18 | 2026-11-18 | 647ad6d4 | v3.4.13 | managed statistics | v3.2.0/workorder-cy-managed-statistics.md |
| CX | closed | 2026-11-19 | 2026-11-18 | 2026-11-18 | 2026-11-18 | 828899f6 | v3.4.13 | the plan as a managed object | v3.2.0/workorder-cx-managed-plans.md |
| CU | closed | 2026-11-17 | 2026-11-15 | 2026-11-16 | 2026-11-17 | a0672010 | v3.4.12 | a value list as a key: IN (...) served by one descent per value | v3.2.0/workorder-cu-key-set-access.md |
| CS | closed | 2026-11-16 | 2026-11-15 | 2026-11-15 | 2026-11-15 | 1edf5c54 | v3.4.12 | BEGIN READ ONLY: no id, nothing logged, a held snapshot | v3.2.0/workorder-cs-read-only-transaction.md |
| CR | written | 2026-11-12 | 2026-11-12 | - | - | n/a | - | a READ COMMITTED write that lost to a newer version is run again | v3.2.0/workorder-cr-read-committed-rerun.md |
| CQ | closed-narrow | 2026-11-12 | 2026-11-11 | 2026-11-11 | 2026-11-12 | ce5347e6 | v3.4.9 | column reservations: a numeric delta that does not hold its row | v3.2.0/workorder-cq-column-reservation.md |
| CP | closed | 2026-11-11 | 2026-11-07 | 2026-11-08 | 2026-11-11 | b0c11e0a | v3.4.8 | the lock model re-examined: modes and units | v3.2.0/workorder-cp-lock-model-exploration.md |
| CL | closed | 2026-11-07 | 2026-11-01 | 2026-11-02 | 2026-11-06 | 6c4b9a6d | v3.4.7 | an undecided transaction's reservations: crash loser, failed commit | v3.2.0/workorder-cl-assertion-reservations-of-an-undecided-transaction.md |
| CM | branch-only | 2026-11-02 | 2026-11-02 | 2026-11-02 | - | - | - | a physical standby: the one log shipped and replayed without end | - |
| CK | closed | 2026-11-01 | 2026-10-31 | 2026-10-31 | 2026-11-01 | a59f9a2e | v3.4.7 | an assertion reservation's commit settle, removed | v3.2.0/workorder-ck-assertion-commit-settle.md |
| CJ | closed | 2026-11-01 | 2026-10-31 | 2026-10-31 | 2026-11-01 | 175deeac | v3.4.7 | wait events and ASH | v3.2.0/workorder-cj-wait-events-ash.md |
| CA | closed | 2026-10-31 | 2026-10-17 | 2026-10-17 | 2026-10-31 | 3f225096 | v3.4.7 | the advisory optimizer, in the background | v3.2.0/workorder-ca-background-advisory-optimizer.md |
| CI | closed | 2026-10-24 | 2026-10-23 | 2026-10-23 | 2026-10-24 | fb36e416 | v3.4.6 | an online index build: CREATE INDEX without the relation X | v3.2.0/workorder-ci-online-index-build.md |
| CG | closed | 2026-10-24 | 2026-10-21 | 2026-10-23 | 2026-10-24 | 0de51f22 | v3.4.6 | INSERT ... ON CONFLICT DO UPDATE: an exploratory concept check | v3.2.0/workorder-cg-on-conflict-do-update.md |
| BL | closed | 2026-10-22 | 2026-10-11 | 2026-10-12 | 2026-10-22 | 27e9d03a | v3.4.3 | no session is pinned: a session balancer and priority | v3.0.0/workorder-bl-session-balancer-priority-affinity.md |
| CH | written | 2026-10-21 | 2026-10-21 | - | - | n/a | - | a tuple key change in one transaction: an exploratory concept check | v3.2.0/workorder-ch-tuple-key-update.md |
| CF | closed | 2026-10-21 | 2026-10-21 | 2026-10-21 | 2026-10-21 | 2a1da9ac | v3.4.1 | Waystone's premise: measure, then remove, default off, or opt in | v3.2.0/workorder-cf-waystone-premise.md |
| CE | closed | 2026-10-20 | 2026-10-20 | 2026-10-20 | 2026-10-20 | 244b031c | v3.4.0 | streaming the KWP reply: a result leaves while its statement runs | v3.2.0/workorder-ce-streaming-reply.md |
| CB | closed | 2026-10-20 | 2026-10-18 | 2026-10-18 | 2026-10-20 | 65dc45c5 | v3.3.0 | a join planner: join order, build side, range bounds via a join key | v3.2.0/workorder-cb-join-planner.md |
| BZ | closed | 2026-10-20 | 2026-10-17 | 2026-10-17 | 2026-10-20 | b98271a9 | v3.4.0 | the heap redefined: a btree relation with no tuple key column | v3.2.0/workorder-bz-heap-hidden-key.md |
| BY | closed | 2026-10-20 | 2026-10-16 | 2026-10-16 | 2026-10-20 | b98271a9 | v3.4.0 | REINDEX, and a dropped index's pages reclaimed | v3.2.0/workorder-by-reindex-drop-index-reclaim.md |
| BV | closed | 2026-10-20 | 2026-10-14 | 2026-10-14 | 2026-10-20 | 2dc67c6d | v3.2.0 | Transition: a column's allowed state changes, row-local | v3.2.0/workorder-bv-transition-constraints.md |
| BR | closed | 2026-10-20 | 2026-10-12 | 2026-10-12 | 2026-10-20 | fe47814c | v3.1.0 | backup and restore: a volume snapshot and a base backup plus archive | v3.0.0/workorder-br-backup-restore.md |
| BO | closed | 2026-10-20 | 2026-10-11 | 2026-10-13 | 2026-10-20 | ff708dfb | v3.4.0 | four statistics relations, and SHOW as sugar over SELECT | v3.0.0/workorder-bo-sys-statistics-show-sugar.md |
| AV | closed-paper | 2026-10-20 | 2026-09-07 | 2026-09-07 | 2026-10-20 | e0dd4d04 | v3.4.0 | the deterministic two-core rig | v3.0.0/workorder-av-two-core-rig.md |
| AU | closed-paper | 2026-10-20 | 2026-09-05 | 2026-09-05 | 2026-10-20 | e0dd4d04 | v3.4.0 | ring retirement: the wake first, the transport last | v3.0.0/workorder-au-ring-retirement.md |
| AN | closed-paper | 2026-10-20 | 2026-09-03 | 2026-09-03 | 2026-10-20 | e0dd4d04 | v3.4.0 | the instance read view | v3.0.0/workorder-an-read-view.md |
| AL | closed-paper | 2026-10-20 | 2026-09-02 | 2026-09-02 | 2026-10-20 | e0dd4d04 | v3.4.0 | AR0 M0: the single WAL stream | v3.0.0/workorder-al-m0-single-wal.md |
| CD | closed | 2026-10-19 | 2026-10-19 | 2026-10-19 | 2026-10-19 | 420f584d | v3.4.0 | cleaning dropped columns: clean in place, then reuse their slots | v3.2.0/workorder-cd-dropped-column-cleaning.md |
| CC | closed | 2026-10-19 | 2026-10-18 | 2026-10-18 | 2026-10-19 | 8fbad5f7 | v3.3.0 | statistics and identifiers in the join planner | v3.2.0/workorder-cc-plan-statistics.md |
| AS | closed | 2026-10-17 | 2026-09-05 | 2026-09-05 | 2026-10-17 | ecf17cc3 | v3.1.0 | SUS-1: heap relations suspended (lifted by BZ) | v3.0.0/workorder-as-sus1-heap-suspended.md |
| BX | closed | 2026-10-16 | 2026-10-15 | 2026-10-15 | 2026-10-16 | 0479cb29 | v3.3.0 | UNIQUE: the unique index and the unique constraint | v3.2.0/workorder-bx-unique-index.md |
| BU | closed | 2026-10-15 | 2026-10-13 | 2026-10-13 | 2026-10-15 | 621b93da | v3.3.0 | the basic query surface: OR, NOT, lists, HAVING, outer joins, set ops | v3.0.0/workorder-bu-basic-query-surface.md |
| AQ | closed | 2026-10-15 | 2026-10-06 | 2026-10-06 | 2026-10-15 | 87d2893c | v3.3.0 | expression and predicate Cabins, expr_id, cover serving | v3.0.0/workorder-aq-expression-cabins.md |
| BW | closed | 2026-10-14 | 2026-10-14 | 2026-10-14 | 2026-10-14 | 3b5daa80 | v3.3.0 | instant ADD COLUMN / DROP COLUMN over reserved slots | v3.2.0/workorder-bw-instant-add-drop-column.md |
| BT | closed | 2026-10-13 | 2026-10-13 | 2026-10-13 | 2026-10-13 | 0807eaf1 | v3.2.0 | the rejected row's fixed cost in AcceptTupleAt | v3.0.0/workorder-bt-accept-tuple-row.md |
| BQ | closed | 2026-10-13 | 2026-10-12 | 2026-10-12 | 2026-10-13 | d0022b19 | v3.1.0 | the WAL segment roll: its latched hold measured and cut | v3.0.0/workorder-bq-wal-segment-roll.md |
| BP | closed | 2026-10-13 | 2026-10-12 | 2026-10-12 | 2026-10-13 | d0022b19 | v3.1.0 | what the per-core engine left | v3.0.0/workorder-bp-per-core-residue.md |
| BM | closed | 2026-10-13 | 2026-10-12 | 2026-10-13 | 2026-10-13 | ae4aca0f | v3.1.0 | the scan row kernel: a filter that rejects before it decodes | v3.0.0/workorder-bm-scan-row-kernel.md |
| BN | closed | 2026-10-11 | 2026-10-11 | 2026-10-11 | 2026-10-11 | 2f4f7c61 | v3.1.0 | the read and update paths: less work per statement and per row | v3.0.0/workorder-bn-opt-read-update-path.md |
| BK | closed | 2026-10-11 | 2026-10-11 | 2026-10-11 | 2026-10-11 | 8a0dd59a | v3.1.0 | a 48-bit Keystone id | v3.0.0/workorder-bk-keystone-48.md |
| BI | closed | 2026-10-11 | 2026-10-08 | 2026-10-11 | 2026-10-11 | ec827a19 | v3.1.0 | the checkpoint stops holding its reactor | v3.0.0/workorder-bi-checkpoint-off-the-reactor.md |
| BJ | closed | 2026-10-10 | 2026-10-10 | 2026-10-10 | 2026-10-10 | 683ba291 | v3.1.0 | expressions in UPDATE ... SET | v3.0.0/workorder-bj-expression-update.md |
| BA | closed | 2026-10-10 | 2026-10-02 | 2026-10-09 | 2026-10-10 | 836b9551 | v3.1.0 | parallelism against the PostgreSQL floor: twelve serialisation points | v3.0.0/workorder-ba-parallelism.md |
| BH | closed | 2026-10-08 | 2026-10-08 | 2026-10-08 | 2026-10-08 | 4015736d | v3.1.0 | PURGE: a deleted key freed, K1's one named exception | v3.0.0/workorder-bh-purge-key.md |
| BF | closed | 2026-10-08 | 2026-10-07 | 2026-10-07 | 2026-10-08 | 3e53df01 | v3.1.0 | DROP TABLE page reclamation | v3.0.0/workorder-bf-drop-table-page-reclaim.md |
| BB | closed | 2026-10-08 | 2026-10-06 | 2026-10-06 | 2026-10-08 | 1e3233a5 | v3.1.0 | the id fixed under the leaf it lands on | v3.0.0/workorder-bb-issue-under-the-leaf.md |
| BE | closed | 2026-10-07 | 2026-10-07 | 2026-10-07 | 2026-10-07 | e9ff9599 | v3.1.0 | a bounded buffer pool: the configured frames are a ceiling | v3.0.0/workorder-be-bounded-pool.md |
| BD | closed | 2026-10-07 | 2026-10-07 | 2026-10-07 | 2026-10-07 | bc144dbf | v3.1.0 | a named key lands where it sorts: leaves in key order by placement | v3.0.0/workorder-bd-sorted-leaf-named-keys.md |
| BC | closed | 2026-10-07 | 2026-10-07 | 2026-10-07 | 2026-10-07 | f37dd695 | v3.1.0 | WAL recycling: segments below the durable redo start removed | v3.0.0/workorder-bc-wal-recycling.md |
| AP | closed | 2026-10-02 | 2026-10-02 | 2026-10-02 | 2026-10-02 | 16ecf20a | v3.1.0 | fetch_id, rule 0' and the function catalog | v3.0.0/workorder-ap-function-catalog-fetch-id.md |
| AZ | closed | 2026-09-30 | 2026-09-30 | 2026-09-30 | 2026-09-30 | 8165bc25 | v3.1.0 | what AY carried: seven items, one letter | v3.0.0/workorder-az-ay-carry-forward.md |
| AY | closed | 2026-09-30 | 2026-09-29 | 2026-09-29 | 2026-09-30 | 9a0525db | v3.1.0 | the following letter: D9(a), D7's last gate, what AT and AX left | v3.0.0/workorder-ay-following-letter.md |
| AX | closed | 2026-09-29 | 2026-09-08 | 2026-09-08 | 2026-09-29 | e52ba9be | v3.1.0 | the instance in-flight publication | v3.0.0/workorder-ax-inflight-publication.md |
| AT | closed | 2026-09-28 | 2026-09-09 | 2026-09-09 | 2026-09-28 | 8627f0d0 | v3.1.0 | AR0 M3: Uniformity | v3.0.0/workorder-at-m3-uniformity.md |
| AT | closed | 2026-09-28 | 2026-09-26 | 2026-09-26 | 2026-09-28 | 8627f0d0 | v3.1.0 | AT close, an AT order: what AR0-5 left open | v3.0.0/workorder-at-close-ar0-5-left-open.md |
| AO | closed | 2026-09-09 | 2026-09-03 | 2026-09-04 | 2026-09-09 | df8cc5f1 | v3.1.0 | AR0 M2: the lock family | v3.0.0/workorder-ao-m2-lock-family.md |
| AW | closed | 2026-09-08 | 2026-09-06 | 2026-09-06 | 2026-09-08 | 0e1ed85c | v3.1.0 | M1 close and the read-view cutover | v3.0.0/workorder-aw-m1-close.md |
| AM | closed | 2026-09-08 | 2026-09-03 | 2026-09-03 | 2026-09-08 | 0e1ed85c | v3.1.0 | AR0 M1: the shared buffer pool, page latches, scalar page LSN | v3.0.0/workorder-am-m1-shared-pool.md |
| AM | closed | 2026-09-06 | 2026-09-06 | 2026-09-06 | 2026-09-06 | 15a57c2d | v3.1.0 | AM-S2-P, an AM order: the scan ring of c7c3a67 ported onto main | v3.0.0/workorder-am-s2-p-scan-ring-port.md |
