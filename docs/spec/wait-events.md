# Wait-event dictionary

What each wait event means, what its parameters hold, what usually makes it
long, and what to do about it. The registry is `include/kds/stats/wait_event.hpp`
(`kWaitEvents`); a name in `sys.wait_event_names` is the public contract and
the numeric id is not (CJ WD2). `tests/wait_event_doc_test.cpp` fails when this
file and the registry disagree on an event's name, class, kind, DB-time flag or
parameter names, so an event cannot be added, renamed or re-parameterised
without this page.

Each section's fields, in this order:

- **Class / Kind / In DB time** — the registry's row. `timed` events book a
  count, a total and a maximum; `counter` events book a count only. Only
  `idle.*` is outside DB time.
- **p1, p2, p3** — the parameter's registry name, then what the engine puts
  there. `(unused)` where the registry names none. A parameter the registry
  names but no site fills yet is written `not filled yet`.
- **Meaning**, **Usual causes**, **Remedies**.
- **Sites** — where the engine books it today, or that it has none.

Reading a wait: `sys.wait_stats` (cumulative, per core), `sys.wait_histogram`
(distribution), `sys.session_waits` and `sys.session_state` (per session),
`sys.ash` (samples) and `ANALYZE`'s `DB time` line (one statement).

### cpu.on_cpu

- **Class:** CPU
- **Kind:** timed
- **In DB time:** yes
- **p1:** (unused)
- **p2:** (unused)
- **p3:** (unused)
- **Meaning:** The task is running on its core. It is a state, kept beside the event table: `sys.wait_stats` carries it as a `cpu.on_cpu` row whose `total_us` is the core's ON_CPU time.
- **Usual causes:** Query execution, parsing, compile, index maintenance.
- **Remedies:** Look at `sys.statement_stats` and `ANALYZE` for the statement's examined rows; an index or a narrower predicate.
- **Sites:** Every poll of a task. The ASH sampler runs between polls, so `sys.ash` never shows ON_CPU (CJ-Q12); CPU shows as RUNNABLE there.

### commit.wal_flush

- **Class:** Commit
- **Kind:** timed
- **In DB time:** yes
- **p1:** target_lsn — the LSN the commit waits to see durable.
- **p2:** durability_class — the effective durability rung as a number: 1 `strict`, 2 `group`, 3 `relaxed`.
- **p3:** group_size — not filled yet: the WAL writer keeps no count of commits one sync covers.
- **Meaning:** A committing statement waits for its record to reach the durable point.
- **Usual causes:** A slow log device sync; many commits queued behind one sync; `strict` durability.
- **Remedies:** A faster log device; `SET DURABILITY group` where the application allows; fewer, larger transactions.
- **Sites:** The commit park in the dispatcher.

### commit.ceiling_wait

- **Class:** Commit
- **Kind:** timed
- **In DB time:** yes
- **p1:** target_lsn — the session's last acknowledged commit, which the snapshot ceiling must cover.
- **p2:** (unused)
- **p3:** (unused)
- **Meaning:** A statement, at its head and before its snapshot is minted, waits for the instance's snapshot ceiling to cover its own session's last acknowledged commit, so it sees its own row. It holds nothing while it waits.
- **Usual causes:** Another core's commit marker held below that commit, typically for a whole `strict` sync.
- **Remedies:** Look at `commit.wal_flush` on the other cores; the cause is usually there.
- **Sites:** The ceiling park at the head of the dispatcher's statement run.

### io.page_read

- **Class:** User I/O
- **Kind:** timed
- **In DB time:** yes
- **p1:** page_id — the page being read.
- **p2:** page_class — not filled yet.
- **p3:** core — not filled yet.
- **Meaning:** A foreground page fault read the page from the data file. The read is a blocking `pread` on the reactor, so the whole core stalls for its duration (CJ-Q8); the time is booked to the event, not to ON_CPU.
- **Usual causes:** A buffer pool smaller than the working set; a cold start; a scan over a cold relation.
- **Remedies:** Raise `buffer_pool_frames`; an index that touches fewer pages.
- **Sites:** The device page store's read.

### io.var_heap_read

- **Class:** User I/O
- **Kind:** timed
- **In DB time:** yes
- **p1:** page_id
- **p2:** rel_oid — the owning relation, from the page header.
- **p3:** (unused)
- **Meaning:** A page fault whose page turned out to be a var-heap page (a spilled value). Booked as `io.page_read` until the header is read, then renamed.
- **Usual causes:** Wide values read through a cold pool.
- **Remedies:** As `io.page_read`; keep hot values inline with a larger `varchar(N)` width.
- **Sites:** The device page store's read, by reclassification.

### io.scan_ring_read

- **Class:** User I/O
- **Kind:** timed
- **In DB time:** yes
- **p1:** page_id
- **p2:** rel_oid — not filled yet.
- **p3:** (unused)
- **Meaning:** A page fault through the scan ring (`FetchHeat::kRing`), the small cyclic frame set that keeps a bulk read from displacing the working set. Its one user is the Cabin controller's build walk; a `SELECT` walk faults as `io.page_read`.
- **Usual causes:** A Cabin build over a relation that is not resident.
- **Remedies:** Usually none: the ring is what keeps the build off the working set. `SET CABIN_OPTIMIZER` stops the builds.
- **Sites:** The device page store's read.

### sysio.writeback

- **Class:** System I/O
- **Kind:** timed
- **In DB time:** yes
- **p1:** page_id
- **p2:** bytes
- **p3:** (unused)
- **Meaning:** A background page writeback.
- **Usual causes:** A device that cannot keep up with the dirty rate.
- **Remedies:** A faster data device.
- **Sites:** None yet: registered, never booked.

### sysio.checkpoint

- **Class:** System I/O
- **Kind:** timed
- **In DB time:** yes
- **p1:** lsn — not filled yet.
- **p2:** bytes — not filled yet.
- **p3:** (unused)
- **Meaning:** A checkpoint step parked on the instance's data-file sync. Booked to the checkpoint task, which is a background task with no session.
- **Usual causes:** A large dirty set; a slow data device sync.
- **Remedies:** `checkpoint_interval_ms`; a faster device.
- **Sites:** The checkpointer.

### sysio.wal_write

- **Class:** System I/O
- **Kind:** timed
- **In DB time:** yes
- **p1:** lsn — the LSN the checkpointer waits to see durable.
- **p2:** bytes — not filled yet.
- **p3:** (unused)
- **Meaning:** The checkpointer parked until the log is durable up to a page's LSN, before it may write that page back (write-ahead). It issues no log write itself; it asks for a sync and waits.
- **Usual causes:** A slow log device sync; pages dirtied by records not yet synced.
- **Remedies:** A faster log device.
- **Sites:** The checkpointer.

### xcore.step_response

- **Class:** CrossCore
- **Kind:** timed
- **In DB time:** yes
- **p1:** target_core
- **p2:** step_id
- **p3:** (unused)
- **Meaning:** A statement waits for a step running on another core.
- **Usual causes:** A busy peer core.
- **Remedies:** None today.
- **Sites:** None: every statement runs on its session's core, so no remote step exists. Registered so the name is stable.

### xcore.credit

- **Class:** CrossCore
- **Kind:** timed
- **In DB time:** yes
- **p1:** target_core
- **p2:** credits_left
- **p3:** (unused)
- **Meaning:** A cross-core stream waits for flow-control credit.
- **Usual causes:** A slow consumer.
- **Remedies:** None today.
- **Sites:** None, as `xcore.step_response`.

### sched.run_queue

- **Class:** Scheduler
- **Kind:** timed
- **In DB time:** yes
- **p1:** scheduling_group — not filled yet.
- **p2:** queue_length — not filled yet.
- **p3:** (unused)
- **Meaning:** A runnable task waited for its turn on the core. Also the RUNNABLE state of the ASH sampler.
- **Usual causes:** More runnable tasks than the core can poll; a long step ahead in the queue.
- **Remedies:** More cores; `SET PRIORITY` to place analytic work on batch cores.
- **Sites:** The poll hook: the time from becoming runnable to the next poll.

### res.eviction_retry

- **Class:** Resource
- **Kind:** timed
- **In DB time:** yes
- **p1:** retry_count
- **p2:** pool_use
- **p3:** (unused)
- **Meaning:** A statement retried a buffer pool reservation after eviction.
- **Usual causes:** A pool at its ceiling with every frame pinned or dirty.
- **Remedies:** Raise `buffer_pool_frames`.
- **Sites:** None yet: registered, never booked.

### net.send_backpressure

- **Class:** Network
- **Kind:** timed
- **In DB time:** yes
- **p1:** session — not filled yet.
- **p2:** unsent_bytes — not filled yet.
- **p3:** (unused)
- **Meaning:** A streaming `SELECT` parked because the client stopped reading its sealed batches.
- **Usual causes:** A slow or stalled client.
- **Remedies:** Fix the client; `max_rows` to bound delivery.
- **Sites:** The select walk's slice boundary.

### app.lock_wait

- **Class:** Application
- **Kind:** timed
- **In DB time:** yes
- **p1:** holding_trx — the transaction holding what the statement waits for.
- **p2:** (unused)
- **p3:** (unused)
- **Meaning:** A statement waits for another transaction's lock: a row writer's decide, a relation lock, an index drain.
- **Usual causes:** Long transactions; hot rows; DDL against a busy relation.
- **Remedies:** Shorter transactions; find the holder in `p1` and `sys.lock_waits`.
- **Sites:** The dispatcher's write-block, relation-lock and index-drain waits.

### app.horizon_wait

- **Class:** Application
- **Kind:** timed
- **In DB time:** yes
- **p1:** waited_trx — the undecided writer, or the deleter every reader must pass, that the `PURGE` waits out; 0 when it waits for older statements' epochs.
- **p2:** (unused)
- **p3:** (unused)
- **Meaning:** A `PURGE` waits for a writer's decide or for the read horizon to pass a deleter it would retire; a `PURGE DROPPED COLUMNS` waits for every statement older than the drop to end. Polled, bounded by the purge's 1 s horizon wait.
- **Usual causes:** A long-lived reader or open transaction; an undecided writer of a row in the window.
- **Remedies:** End the old transaction; retry the `PURGE`.
- **Sites:** The `PURGE` wait in the dispatcher.

### app.lock_conflict

- **Class:** Application
- **Kind:** counter
- **In DB time:** yes
- **p1:** rel_oid — not filled yet.
- **p2:** pk — not filled yet.
- **p3:** holding_trx — not filled yet.
- **Meaning:** A lock ask was refused. A count only: the time of any wait that follows is `app.lock_wait`.
- **Usual causes:** Contention on a row or relation.
- **Remedies:** As `app.lock_wait`.
- **Sites:** The two refusal exits of the lock table.

### app.assertion_reservation

- **Class:** Application
- **Kind:** counter
- **In DB time:** yes
- **p1:** assertion_id — not filled yet.
- **p2:** group_key_hash — not filled yet.
- **p3:** (unused)
- **Meaning:** A write met another transaction's reservation in an assertion's group. A count only.
- **Usual causes:** Concurrent writers in one assertion group.
- **Remedies:** Spread writes across groups; shorten transactions.
- **Sites:** The assertion check.

### idle.client

- **Class:** Idle
- **Kind:** timed
- **In DB time:** no
- **p1:** (unused)
- **p2:** (unused)
- **p3:** (unused)
- **Meaning:** A session between statements, waiting on its client. Outside DB time and never sampled (CJ WD6).
- **Usual causes:** Normal operation.
- **Remedies:** None.
- **Sites:** Published when a session is created and, by the poll hook, when its statement ends; no scope books time to it.

### idle.background

- **Class:** Idle
- **Kind:** timed
- **In DB time:** no
- **p1:** (unused)
- **p2:** (unused)
- **p3:** (unused)
- **Meaning:** A background task waiting for work. Outside DB time. Today only the server's host loop books it, parked until a connection is handed to its core or the server stops.
- **Usual causes:** Normal operation.
- **Remedies:** None.
- **Sites:** The server's host loop.
