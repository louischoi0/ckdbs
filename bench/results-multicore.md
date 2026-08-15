# Multi-core isolation benchmark — cores=1 vs cores=2, 4 isolated relations

Measured 2026-08-10 at commit `494c4ee`, `build-release/kds_server`
(Release), on a 2-core EC2 host (`nproc` = 2, so `cores = 2` is this
machine's ceiling — `cores` is refused above the reported core count).
Driver: `tools/multicore_benchmark.py --rows 2000`.

## The question, and the honest answer up front

Does giving KDS more cores speed up a workload of **four non-interfering
relations**, each driven by its own connection? **Not yet, and the engine
says why**: relations are placed on the *creating* core, DDL runs on
core 0, and the cross-core step pipeline is unbuilt — so core 0 serves
every statement whatever `cores` says, and peers come up alive and idle
(`docs/crosscore.md` P4/P6, `docs/workplan-crosscore.md`). The measured
ratio is parity within noise. **This file is the baseline the pipeline
will be measured against**; the harness already does everything a real
demonstration needs — isolated relations, per-relation connections, and
per-relation `owner_core` printed from `DESCRIBE`.

## Setup

- 4 relations `bench0..bench3`, `(id int64, owner varchar, balance int64)
  BTREE`, created fresh per configuration (`cores` is superblock-pinned,
  so each configuration bootstraps its own data file).
- One client thread + connection per relation, all four running
  concurrently past a start barrier; no statement ever touches two
  relations.
- Per relation: 2,000 INSERTs, 2,000 point-SELECTs by pk, 2,000 UPDATEs
  by pk, 1,000 DELETEs (the odd half), one final scan —
  28,004 statements per configuration.
- Defaults otherwise: `durability = group`, single-connection-per-thread
  text protocol, so every latency carries the Python client's socket cost
  (the floor this harness resolves; sub-10 µs differences are noise).

## Results (run 1)

**Placement — the load-bearing fact**: every relation reports
`owner_core=0` in *both* configurations.

| | cores=1 | cores=2 |
|---|---|---|
| wall clock | 11.09 s | 10.56 s |
| aggregate throughput | 2,524 stmt/s | 2,651 stmt/s |
| errors | 0 | 0 |

Per-phase latency, aggregated across the four relations:

| phase | n | cores=1 p50 / p99 (µs) | cores=2 p50 / p99 (µs) |
|---|---|---|---|
| insert | 8,000 | 2,031 / 4,618 | 1,959 / 4,150 |
| point-select | 8,000 | 160 / 1,512 | 185 / 2,010 |
| update | 8,000 | 2,048 / 3,951 | 2,052 / 5,402 |
| delete | 4,000 | 2,021 / 4,775 | 2,020 / 4,210 |

**Throughput ratio cores=2 / cores=1: 1.050x (run 1), 1.129x (repeat).**

## Reading it

- **Parity is the result.** A 5-13% run-to-run spread with p50s equal to
  within a few percent in both directions is noise on a shared 2-vCPU
  host, not scaling: the write phases' p50 (~2 ms) is the `group`-fsync
  path, identical in both configurations because every statement runs on
  core 0 either way. What `cores = 2` buys today is a second WAL stream
  that this workload never writes to.
- **The four connections already interleave** — the server serves clients
  concurrently but cooperatively on one thread — so the point-select p50
  of ~160-185 µs is four threads sharing one reactor, not one thread's
  latency.
- **What would make this benchmark move**: the cross-core pipeline
  (statements executing on the owning core) plus relation placement that
  spreads the four relations across cores — the P6 blocker (relation
  ownership vs page ownership) owns both. When that lands, this exact
  driver re-run is the demonstration: same tables, same isolation, and
  `owner_core` will say `0,1,0,1` instead of `0,0,0,0`.

## The re-run, 2026-08-15 (workplan P4e) — and why it still cannot move

The pipeline landed (P4d complete, `docs/workplan-crosscore.md`): a
two-step join executes across cores. The re-run this file asked for was
attempted at commit `3854242` with the driver extended as promised —
`tools/multicore_benchmark.py` now takes `--placement`, so the "`owner_core`
will say `0,1,0,1`" prediction above is testable rather than asserted.

**It says `0,1,0,1`. The workload still cannot run, and the reason is not
the pipeline.**

```
== multi-core: cores=2, placement=rotate ==
   placement: bench0 owner_core=1  bench1 owner_core=1
   NOT RUN - the relations cannot be written from this connection:
     ERR this transaction's writes are bound to core 0 and relation 'bench0'
     is owned by core 1; a transaction may write on one core only until
     two-phase commit exists
```

**A peer-owned relation has no writer.** Cross-core writes are refused
(crosscore.md CC3), DML statement shipping is unbuilt, and core 0 alone
carries a listener — so no sequence of client statements can populate the
relations this benchmark would then read across cores. The driver probes
with one INSERT and reports this rather than producing an error storm
from four threads; that probe is the deliverable, because it turns a
claim about the engine into something reproducible in 10 seconds.

So the prediction in "What would make this benchmark move" was
incomplete. It named the pipeline and placement; it did not name **a
writer for peer-owned relations**, which is the binding constraint now
that the other two exist. The ratio above stays this file's baseline, and
`placement = creating` remains the only configuration this driver can
run — at which every relation is on core 0 and parity is the correct
answer whatever the pipeline can do.

**The pipeline was measured instead where it can be reached**, in
process, against local execution over the same rows:
`bench/results-crosscore-pipeline.md`. Headline: a shipped statement
costs 2.52 µs plus **0.626 µs per forwarded row**, against 0.417 µs per
row for the same join run locally.

## Deviations from the house results format

No PostgreSQL comparison and no p0/p25/wait breakdown: the comparison
this file exists for is KDS against its own future, and the per-phase
p50/p99 above is what the driver records today.
