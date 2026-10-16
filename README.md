<p align="center">
  <a href="https://kdbs.dev">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="assets/brand/kdbs-logo-dark.svg">
      <img src="assets/brand/kdbs-logo-light.svg" alt="KDBS" width="240">
    </picture>
  </a>
</p>

<p align="center"><strong>An OLTP database engine that keeps the invariants so the application does not have to.</strong></p>

<p align="center">
  <a href="https://kdbs.dev">Homepage</a> · <a href="manual/README.md">Manual</a> · <a href="manual/sql/sql.md">SQL reference</a> · <a href="docs/README.md">Design docs</a> · <a href="docs/inflight/known-gaps.md">Known gaps</a>
</p>

KDS is a userspace storage engine written in C++20 for online transaction processing: keyed reads and writes, short transactions, joins along key chains and durability the caller chooses. It narrows its feature surface to that workload and spends what it saves on two things most engines leave to the application — **the rules the data must obey** and **the physical shape the data should have**.

---

## Why KDS

Every application keeps a list of things it must never get wrong: no duplicate ids, no group over its limit, no state change that skips a step. Usually each rule is re-checked in every service that writes. KDS takes the position that these rules belong **in the schema, enforced by the engine**, so that nothing that writes to the database — a service, a script, an agent — can break them.

| The engine guarantees | So the application no longer has to |
|---|---|
| **Issue-once identity** — a row's tuple key is never reused for another committed row, unless an explicit `PURGE` frees it | guard against a stale id pointing at the wrong row |
| **Group bounds** — `CREATE ASSERTION` on per-group `COUNT(*)` / `SUM(col)` limits, checked in O(1) per write | count and compare before every write, and reconcile races |
| **Transition constraints** — `CREATE TRANSITION` declares a column's legal state changes | check `old → new` in every writer |
| **Referential integrity and unique indexes** | confirm a parent exists, or that a value is not taken |
| **Chosen durability** — `strict`, `group` or `relaxed` per transaction, with the loss window stated | guess what an acknowledgment meant |
| **A predictable plan** — a statement runs in the order it was written, with no cost-based reordering | tune around a planner that changes its mind |

Errors are built to be acted on: every refusal names the byte position of the offending token, and says whether the statement was simply wrong (`InvalidArgument`), understood but not built (`NotImplemented`), or outside what the architecture admits (`Unsupported`).

## Key ideas

- **The query is the plan.** Each statement compiles at parse time into a *step chain* executed in written order — no plan search, no plan flips with cardinality.
- **Keystone.** Every row carries a 48-bit tuple key — its identity for the life of the relation, never updated — so any structure can name a row by value instead of by address. Relations are clustered B+ trees on that key.
- **Waystone.** For a query shape that repeats, the engine records where the last execution found its rows and looks there first. It is advisory: a wrong or missing trail costs time, never a result.
- **Cabin.** A per-column lookup structure that covers only the values queries actually asked for — exact for those values, with no index-style cost for the rest. A secondary index is a Cabin that has observed everything.
- **Physical optimizer.** Runtime access statistics decide which Cabins exist: when switched on, the engine creates and drops them itself on columns declared `CABIN AUTO` (`SHOW CABIN_OPTIMIZER`).
- **Fixed-length tuples.** An `UPDATE` rewrites a row in place; long values spill to an immutable side store.
- **Thread-per-core.** One cooperative reactor per core over a shared buffer pool and one write-ahead log; recovery runs at mount, and the whole engine runs under deterministic crash simulation in tests.

Everything the engine learns is advisory by construction: delete all of it and every query returns the same rows, only slower.

## Quick start

### Try it with Docker

```sh
git clone https://github.com/louischoi0/ckdbs && cd ckdbs
docker compose run --rm cli
```

This builds the image, starts the server and opens a SQL prompt. To run the server alone, use `docker compose up -d kds`; it listens on `127.0.0.1:15432` and keeps its data in the `kds-data` volume.

### Build from source

Requirements: CMake ≥ 3.20, a C++20 compiler and OpenSSL 3.

```sh
scripts/build.sh                     # build into ./build
scripts/test.sh                      # build and run the test suite
./build/kds_server --config kds.conf.sample   # data file kds.db, port 15432
python3 tools/ckdbs_cli.py           # interactive SQL client
```

```sql
CREATE TABLE accounts (id int64, owner varchar, balance decimal(10,2));
INSERT INTO accounts (owner, balance) VALUES ('alice', 100.00);   -- id is issued
SELECT * FROM accounts WHERE id = 1;
```

Configuration, operation and backup are in the [server manual](manual/server/server.md).

## Documentation

| For | Read |
|---|---|
| Writing SQL | [SQL reference](manual/sql/sql.md) |
| Running and configuring the server | [Server manual](manual/server/server.md), [Backup and restore](manual/server/backup-restore.md) |
| Writing a client (KWP/1 wire protocol) | [Client manual](manual/client/client.md) |
| The self-managing optimizer | [Physical optimizer manual](manual/physical-optimizer/physical-optimizer.md) |
| How the engine works inside | [Design specifications](docs/spec/), starting with [`heap-and-tuple.md`](docs/spec/heap-and-tuple.md) |
| What is missing, and what a restart loses | [Known gaps](docs/inflight/known-gaps.md) |
| Ideas not yet built | [Concept notes](docs/conceptnotes/) |

## What KDS is not

Not a data warehouse, not a general-purpose SQL database and not a dialect-compatibility layer. If the workload is analytical scans over wide history, use a column store; if it is high-rate transactional access to living data with rules the data must obey, KDS is built for that.

## Status

Under active development. Read the [known gaps](docs/inflight/known-gaps.md) before relying on any behaviour; the [manual](manual/README.md) is verified against the code.

## Terminology

The 48-bit row identity is called the **tuple key**, never the *primary key*: it is a row's identity, not a business key.
