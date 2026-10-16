<p align="center">
  <a href="https://kdbs.dev">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="assets/brand/kdbs-logo-dark.svg">
      <img src="assets/brand/kdbs-logo-light.svg" alt="KDBS" width="240">
    </picture>
  </a>
</p>

<h3 align="center">An OLTP database engine that keeps the invariants<br>so the application does not have to.</h3>

<p align="center">
  <a href="https://kdbs.dev"><b>Homepage</b></a> ·
  <a href="#quick-start"><b>Quick start</b></a> ·
  <a href="manual/README.md"><b>Manual</b></a> ·
  <a href="manual/sql/sql.md"><b>SQL reference</b></a> ·
  <a href="docs/README.md"><b>Design docs</b></a>
</p>

<p align="center">
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-E8380D">
  <img alt="Linux" src="https://img.shields.io/badge/platform-Linux-14213D">
  <img alt="Docker" src="https://img.shields.io/badge/docker-compose-14213D">
  <img alt="Status" src="https://img.shields.io/badge/status-active%20development-14213D">
</p>

---

**KDBS** is a userspace database engine written in C++20 for online transaction processing: keyed reads and writes, short transactions, joins along key chains, and durability the caller chooses. It keeps its feature surface narrow and spends what that saves on two things most engines leave to the application — **the rules the data must obey** and **the physical shape the data should have**.

## Why KDBS

Every application keeps a list of things it must never get wrong: no duplicate ids, no group over its limit, no state change that skips a step. Usually each rule is re-checked in every service that writes. KDBS puts these rules **in the schema, enforced by the engine**, so nothing that writes to the database — a service, a script, an agent — can break them.

```sql
-- No flight is sold past 180 seats, whoever writes and on whatever core.
CREATE TABLE bookings (id int64, flight_no int64, passenger varchar);
CREATE ASSERTION seat_cap ON bookings GROUP BY (flight_no) CHECK COUNT(*) <= 180;

-- An order is finalized only after it was checked.
CREATE TABLE orders (id int64, status varchar(16));
CREATE TRANSITION order_status ON orders(status)
  ON INSERT ('NEW')
  ON UPDATE ('NEW' => 'CHECKED', 'CHECKED' => 'FINALIZED');

INSERT INTO orders (status) VALUES ('NEW');
UPDATE orders SET status = 'FINALIZED' WHERE status = 'NEW';
-- ERR TRANSITION_VIOLATION retryable=0 transition 'order_status' on column 'status':
--     'NEW' => 'FINALIZED' is not a declared edge, row id 1 (byte 27)
```

| The engine guarantees | So the application no longer has to |
|---|---|
| **Group bounds** — `CREATE ASSERTION` on per-group `COUNT(*)` / `SUM(col)` limits, checked in O(1) per write | count and compare before every write, and reconcile races |
| **Transition constraints** — `CREATE TRANSITION` declares a column's legal state changes | check `old → new` in every writer |
| **Issue-once identity** — a row's tuple key is never reused for another committed row, unless an explicit `PURGE` frees it | guard against a stale id pointing at the wrong row |
| **Referential integrity and unique indexes** | confirm a parent exists, or that a value is not taken |
| **Chosen durability** — `strict`, `group` or `relaxed` per transaction, with the loss window stated | guess what an acknowledgment meant |
| **A predictable plan** — a statement runs in the order it was written, with no cost-based reordering | tune around a planner that changes its mind |

Errors are built to be acted on: every refusal names the byte position of the offending token and says whether the statement was simply wrong (`InvalidArgument`), understood but not built (`NotImplemented`), or outside what the architecture admits (`Unsupported`).

## Key ideas

| | |
|---|---|
| **The query is the plan** | Each statement compiles at parse time into a *step chain* executed in written order — no plan search, no plan flips with cardinality. |
| **Keystone** | Every row carries a 48-bit tuple key — its identity for the life of the relation, never updated — so any structure can name a row by value instead of by address. Relations are clustered B+ trees on that key. |
| **Waystone** | For a query shape that repeats, the engine records where the last execution found its rows and looks there first. A wrong or missing trail costs time, never a result. |
| **Cabin** | A per-column lookup structure covering only the values queries actually asked for — exact for those values, with no index-style write cost for the rest. A secondary index is a Cabin that has observed everything. |
| **Physical optimizer** | Runtime access statistics decide which Cabins exist: when switched on, the engine creates and drops them itself on columns declared `CABIN AUTO`. |
| **Fixed-length tuples** | An `UPDATE` rewrites a row in place; long values spill to an immutable side store. |
| **Thread-per-core** | One cooperative reactor per core over a shared buffer pool and one write-ahead log. Recovery runs at mount, and the whole engine runs under deterministic crash simulation in tests. |

Everything the engine learns is advisory by construction: delete all of it and every query returns the same rows, only slower.

## Quick start

### Try it with Docker

```sh
git clone https://github.com/louischoi0/ckdbs && cd ckdbs
docker compose run --rm cli
```

This builds the image, starts the server and opens a SQL prompt. To run the server alone, use `docker compose up -d kds`; it listens on `127.0.0.1:15432` and keeps its data in the `kds-data` volume.

### Build from source

Requirements: Linux, CMake ≥ 3.20, a C++20 compiler and OpenSSL 3.

```sh
scripts/build.sh                                # build into ./build
scripts/test.sh                                 # build and run the test suite
./build/kds_server --config kds.conf.sample     # data file kds.db, port 15432
python3 tools/ckdbs_cli.py                      # interactive SQL client
```

### First statements

```sql
CREATE TABLE accounts (id int64, owner varchar, balance decimal(10,2));
INSERT INTO accounts (owner, balance) VALUES ('alice', 100.00);   -- the tuple key is issued
SELECT * FROM accounts WHERE owner = 'alice';
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

## What KDBS is not

Not a data warehouse, not a general-purpose SQL database and not a dialect-compatibility layer. If the workload is analytical scans over wide history, use a column store; if it is high-rate transactional access to living data with rules the data must obey, KDBS is built for that.

## Status

Under active development. Read the [known gaps](docs/inflight/known-gaps.md) before relying on any behaviour; the [manual](manual/README.md) is verified against the code. The engine's binaries, settings and source still carry the `kds` prefix (`kds_server`, `kds.conf`).

**Terminology.** The 48-bit row identity is called the **tuple key**, never the *primary key*: it is a row's identity, not a business key.

---

<p align="center">
  <a href="https://kdbs.dev">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="assets/brand/kdbs-symbol-dark.svg">
      <img src="assets/brand/kdbs-symbol-light.svg" alt="KDBS" width="28">
    </picture>
  </a><br>
  <sub><a href="https://kdbs.dev">kdbs.dev</a></sub>
</p>
