# Cabin — measured on the business stress scenario

Workplan `CB01`-`CB11`. First measured 2026-08-03; **corrected and
re-measured 2026-08-04** after the first pass turned out to have been run on
tmpfs and reported as though it were a block device. See "Correction" below —
the first set of numbers overstated the feature and should not be quoted.

**Headline: the Cabin does exactly what it claims to the query it targets,
and that is worth between 0% and 27% of end-to-end throughput depending
entirely on two things the Cabin does not control** — how often the workload
repeats a value, and whether the scan it replaces is actually the bottleneck.
On the larger, disk-backed configuration it is a wash.

The scenario is `tools/stress_business.py --cabin`: a brokerage book where
one process executes trades (2 `INSERT trades` + 2 `UPDATE accounts` per
transaction) while a separate process runs periodic reporting over
`SELECT * FROM accounts WHERE user_id = <n>` — a non-pk equality on a foreign
key, which without a Cabin is a `FilterScan` of the whole relation per user
per period.

---

## Correction to the 2026-08-03 numbers

The first measurement put the data file in the session scratchpad, which is
`/tmp` — **tmpfs, i.e. RAM**. The tool's own docstring warns against exactly
this ("Do not measure this on tmpfs, where fsync is free and every durability
class looks identical") and the results file nonetheless said "EBS gp3". That
was wrong, and it mattered more than a footnote: `INSERT` is the one logged
statement, so with fsync free the WAL cost vanished and the reporting scan
looked like the dominant cost. It is not, on real storage.

| | tmpfs (wrong) | xfs on NVMe (right) |
|---|---|---|
| trade-insert p50 | 182 µs | 1,139 µs |
| share of run spent in trade-insert | ~20% | **82%** |

Everything below is on `/dev/nvme0n1p1` (xfs), `durability = group`,
Release build, 2 vCPU.

---

## Method

Two servers, two fresh data files, identical arguments apart from `--cabin`.

```
./build-release/kds_server <fresh>.db --port <p> --log-level error
python3 tools/stress_business.py --port <p> --users 10000 --assets 10000 \
    --txn-per-user 10 --seconds 1200 --profit-users 100 \
    --profit-interval 0.2 --profit-period-days 3 --verify 200 [--cabin]
```

10,000 users, 20,012 accounts, 10,000 assets. `--txn-per-user 10` makes this
a **fixed-work** run: both sides commit exactly 100,000 transactions and the
time each takes is the measurement. 59 reporting periods, 100 users sampled
per period, 5,900 scans total.

The Cabin is declared as a column policy (`user_id int64 CABIN`), so it is
created at `CREATE TABLE` and observes a value on its first selection (n=1).

`--verify 200` matched on both sides and no statement replied `ERR`. That is
the precondition for reading anything below.

## Results — 10,000 users, 100,000 transactions, on disk

| | no cabin | cabin | delta |
|---|---|---|---|
| **TPS** | 263.4 | 271.7 | **+3.2%** |
| wall clock for the same 100,000 txns | 379.7 s | 368.0 s | −3.1% |
| **profit-scan mean** | 10,713 µs | 8,571 µs | **−20.0%** |
| profit-scan p50 | 10,214 µs | 10,088 µs | −1.2% |
| txn p50 | 2,682 µs | 2,704 µs | +0.8% |
| account-update p50 | 121 µs | 123 µs | +1.7% |
| trade-insert p50 | 1,139 µs | 1,146 µs | +0.7% |

```
observed values   4,096   of 10,000 users   (== cabin_max_values)
entries           8,157
served / walked   1,413 / 4,487             23.9% of probes served
```

### Why mean moved 20% and p50 did not

Both are correct and they say different things. 23.9% of probes were served
from an entry set and became nearly free; the other 76% still walked the
relation *and* recorded while doing it. A quarter of the distribution moving
to ~0 pulls the mean down by roughly a quarter and leaves the median sitting
where it always was — in the walking majority. The Cabin is doing precisely
what it promises to the probes it can serve.

### Why that is worth only 3% end to end

`profit-scan` is 63.2 s of a 379.7 s run. `trade-insert` is **310.7 s** —
200,000 WAL-logged inserts at ~1.1 ms each. Cutting 20% off the smaller
number is 12.6 s, which is 3.3% of the run, which is the +3.2% measured. The
arithmetic closes.

**On real storage this workload is bound by the log, not by the scan.** A
structure that makes a non-pk read cheaper cannot move a number that is
dominated by fsync, and no amount of Cabin fixes that.

### Why the hit rate was 24%, and why the cap is not to blame

The reporter samples 100 users at random from 10,000, 59 times: 5,900 draws.
Expected distinct values from that is
`10000 × (1 − e^(−5900/10000)) ≈ 4,457`, so expected repeats ≈ **1,443**.
Measured hits: **1,413**.

So the hit rate is an arithmetic property of *random sampling*, not a
limitation of the structure. `observed values` did hit the 4,096
`cabin_max_values` default, but the cap clipped an expected 4,457 distinct
values to 4,096 — it cost on the order of 40 hits out of 1,443 and is not
what held the number down.

A Cabin is authoritative for *observed* values and pays off when a value is
probed **again**. A reporting job that samples uniformly at random from a
large population is close to the worst case for it. A job that sweeps all
users, or repeatedly reports on an active subset, is the shape it is for.

## Results — 2,000 users, 20 s, on tmpfs (the 2026-08-03 run)

Kept because it is a real measurement of a different configuration, and
because the contrast is the point. **Its TPS delta is a tmpfs artifact and
should not be quoted as a throughput result.**

| | no cabin | cabin | delta |
|---|---|---|---|
| TPS | 1,197.1 | 1,525.7 | +27.4% |
| profit-scan p50 | 1,960 µs | 190 µs | −90.3% |
| txn p95 | 4,031 µs | 960 µs | −76.2% |
| hit rate | | | 61.8% |

Two things differ and both matter. The hit rate was 61.8% because 100 users
were sampled from 2,000 rather than 10,000. And with fsync free, the scan
*was* the dominant cost, so removing it moved TPS — on a single-threaded
dispatcher an analytic scan is a write-path cost. Put the log back on a real
device and that stops being true.

## Scale of the scan itself

The served path is roughly flat in relation size; the walk it replaces is
linear. That ratio is the only number here that is a property of the Cabin
alone:

| accounts | profit-scan p50, walked | served |
|---|---|---|
| 618 | 530 µs | 158 µs |
| 4,015 | 1,960 µs | 190 µs |
| 20,012 | ~10,200 µs | (mean falls 20% at a 24% hit rate) |

## What the write hook cost

`account-update` p50 moved +1.7% and `trade-insert` +0.7% — at or below the
run-to-run noise on this machine. That is the "observed ⇒ complete" price,
and it is this small only because `docs/feat-cabin.md` §5's third row is
implemented: an UPDATE that does not change the key column **does not
append**. The first implementation appended on every write, which stays
correct under the superset rule but is unbounded — the two account updates
per trade would have added ~200,000 entries over this run until the per-value
cap un-observed the value and the Cabin stopped serving the relation it was
declared for.
`CabinContractTest.AnUpdateThatDoesNotTouchTheKeyColumnAppendsNothing` pins
it against exactly this workload's shape.

## What to take from this

1. **The feature works and is cheap.** −20% on the mean of the query it
   targets, at a write cost inside noise, with byte-identical answers.
2. **It is demand-driven, so it pays in proportion to repetition.** 24% hits
   from uniform random sampling; 62% when the population was 5× smaller.
   Neither is a defect.
3. **Judge it against the bottleneck.** On tmpfs the scan dominated and the
   Cabin was worth 27% of throughput; on disk the WAL dominates and it is
   worth 3%. The honest summary of the disk run is *a wash*.
4. `cabin_max_values = 4096` is a `[PROPOSED]` default that a 10,000-value
   column reaches. It did not bind here, but it would on a workload with a
   higher repeat rate, and §8's budget is still open.

## What this does not measure

- **A heap relation.** `accounts` is BTREE, so a failed location hint
  resolves through a descent; no hint failed in these runs.
- **Restart.** The entry sets are memory-resident by design (§9), so a
  restart returns every value to the scan path.
- **Concurrency beyond 2 vCPU**, or more than one trader process.
- **`DELETE`** — there is no such statement.
