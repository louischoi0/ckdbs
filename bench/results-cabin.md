# Cabin — measured on the business stress scenario

Workplan `CB01`-`CB11`, measured 2026-08-03 on the build that landed them.

**Headline: the reporting scan gets 10× faster and the write workload gets
27% faster with it.** The second number is the interesting one and is not a
mistake — see "Why TPS went *up*" below.

The scenario is `tools/stress_business.py --cabin`: a brokerage book where
one process executes trades (2 `INSERT trades` + 2 `UPDATE accounts` per
transaction) while a separate process runs periodic reporting over
`SELECT * FROM accounts WHERE user_id = <n>`. That query is a non-pk
equality on a foreign key — a `FilterScan`, a walk of the whole `accounts`
relation, per user, per period — and it is exactly the shape a Cabin exists
for.

---

## Method

Two servers, two fresh data files, identical arguments apart from `--cabin`.
Release build, `durability = group`, EBS gp3, one trader process and one
reporting process, each on its own connection to a server that dispatches
every client on one thread.

```
./build-release/kds_server sb.db --port <p> --log-level error
python3 tools/stress_business.py --port <p> --users 2000 --assets 500 \
    --seconds 20 --profit-users 100 --profit-interval 0.3 \
    --profit-period-days 3 --verify 100 [--cabin]
```

2,000 users, 4,015 accounts, 500 assets. The Cabin is declared as a **column
policy** — `user_id int64 CABIN` — so it is created at `CREATE TABLE` and
observes a value on its **first** selection (n=1, the rule a declared
pattern already gets). At n=2 a reporter sampling 100 users out of 2,000
would spend most of a 20-second run counting first sightings.

`--verify 100` reads accounts back and compares them against the driver's
in-memory balances. **It matched in both runs**, which is the precondition
for reading any of the numbers below: a Cabin chooses where to look, never
what is visible.

---

## Results

| | no cabin | cabin | delta |
|---|---|---|---|
| **TPS** (committed transactions/s) | 1,197.1 | 1,525.7 | **+27.4%** |
| committed transactions | 23,942 | 30,514 | +27.4% |
| **profit-scan** p50 | 1,960 µs | 190 µs | **−90.3%** |
| profit-scan p95 | 2,871 µs | 2,409 µs | −16.1% |
| profit-scan p99 | 3,728 µs | 3,091 µs | −17.1% |
| **txn** p50 | 525 µs | 526 µs | +0.2% |
| txn p95 | 4,031 µs | 960 µs | **−76.2%** |
| txn p99 | 5,739 µs | 4,098 µs | −28.6% |
| account-update p50 | 114 µs | 114 µs | −0.1% |
| account-update p95 | 204 µs | 174 µs | −14.6% |
| trade-insert p50 | 128 µs | 130 µs | +1.6% |

Cabin state at the end of the run:

```
observed values   1,833   of 2,000 users
entries           3,691   ~24 bytes each  (~89 KB)
served / walked   2,967 / 1,833           61.8% of probes served
```

**`misses` equals `observed` exactly** — 1,833 each. That is the n=1 policy
being visible rather than asserted: every value walked the relation exactly
once, recorded, and was served on every probe after. The 61.8% "hit rate" is
therefore a statement about the *sampling*, not about the Cabin: with 100
users drawn at random per period out of 2,000, a third of the probes in a
20-second run are first sightings. A longer run converges toward 100%.

---

## Why TPS went *up*

A Cabin makes the reader cheaper and the writer more expensive, so the naive
expectation is that TPS falls. It rose 27%, and the reason is the thing this
scenario was built to expose: **the server dispatches every client on one
thread** (`docs/sched.md`). A 1,960 µs `FilterScan` is 1,960 µs during which
no trade can be executed. Cutting it to 190 µs hands that time back to the
traders.

The `txn` percentiles say it more precisely than the mean does. p50 is
unchanged — 525 µs against 526 µs — because a transaction that does not
collide with a reporting scan costs exactly what it always did. p95 collapses
from 4,031 µs to 960 µs: the tail *was* the scan, and it is now short enough
to stop dominating.

So the honest reading is not "a Cabin makes writes faster". It is: **on a
single-threaded dispatcher, an analytic scan is a write-path cost, and this
is what removing it is worth.** On a multi-core server with the relation
owned by another core, the reader's improvement would stay and this
particular TPS gain would not.

## What the write hook actually cost

`account-update` p50 moved by −0.1%: **unmeasurable**. That is the
"observed ⇒ complete" price, and it is this small for one specific reason
worth recording, because the first implementation got it wrong.

The trader's `UPDATE accounts SET balance = ..., asset_qty = ...` never
touches `user_id`, and `docs/feat-cabin.md` §5's third row says an UPDATE
that does not change the key column does nothing. The first version appended
regardless. That is still *correct* — the entry set stays a superset and the
read dedupes — but it is unbounded: two account updates per trade would have
grown observed sets by 60,000 entries over this run, until the per-value cap
un-observed them and the Cabin stopped serving the relation it was declared
for. Correct and useless is still a defect. The comparison now happens in the
hook (`CommandDispatcher::NoteCabinWrite`), and
`CabinContractTest.AnUpdateThatDoesNotTouchTheKeyColumnAppendsNothing` pins
it against exactly this workload's shape.

What remains is one in-memory directory probe per cabined column per write,
which is what the −0.1% is.

## Scale

Two sizes were measured. The Cabin's win grows with the relation, which is
what a structure replacing a full walk should do:

| accounts | profit-scan p50, no cabin | with cabin | speedup |
|---|---|---|---|
| 618 | 530 µs | 158 µs | 3.4× |
| 4,015 | 1,960 µs | 190 µs | **10.3×** |

The served path is roughly flat — 158 µs against 190 µs for 6.5× the
relation — because it reads the matching rows and nothing else. The
`FilterScan` it replaces is linear in the relation, so the ratio is a
function of how much bigger the relation is than the answer.

---

## What this does not measure

- **Memory.** 3,691 entries is ~89 KB and nothing here pushes the caps.
  `cabin_max_values` / `cabin_max_entries_per_value` are `[PROPOSED]` and
  the budget (§8) is open; a workload with high-cardinality values is where
  that decision needs data.
- **A heap relation.** `accounts` is BTREE here, so a failed location hint
  resolves through a descent. On a heap relation the step abandons the Cabin
  and walks (there is no pk descent), which is a different cost that no hint
  ever failed in this run to exercise.
- **Restart.** The entry sets are memory-resident by design (§9), so a
  restart returns every value to the scan path and the first period after it
  pays the walk again. Nothing here measures the rebuild.
- **`DELETE`.** There is no such statement, so the one maintenance case that
  is specified and unexercised stays unexercised.
