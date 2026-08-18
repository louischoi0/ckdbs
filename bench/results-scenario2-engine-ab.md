# A freight booking's statements cost 2.9% more than they did 99 commits ago

**Thesis.** Between `c500d4a` (the engine `bench/results-scenario2-freight.md`
was last measured at, 2026-08-11) and `92c76dd` (`origin/main`, 2026-08-18),
the eight statements a freight booking issues went from **492.8 µs to 507.0
µs per booking, +2.9%** — and **all of the resolvable part of it is on the
four reads**, which moved +3.2% to +4.8% with *no overlap at all* between the
two engines' distributions across 11 runs each. The four writes moved +1.2%
to +2.1%, inside their overlap and therefore not resolvable. A midpoint build
at `2bd5030` splits the cost roughly in half: +1.2% by the midpoint, +1.6%
after it. **Throughput sees none of this** — the workload's TPS is set by one
fsync whose run-to-run drift is 8%, and the old and new medians (521.3 and
525.3) are indistinguishable.

This file is a two-ckdbs A/B on one axis, so it carries no PostgreSQL column:
a PostgreSQL baseline prices an engine against another engine, not an engine
against itself. `bench/results-scenario2-freight.md` is the current-state
document for this workload and is where the engine's absolute numbers live.

## 1. The run

| | |
|---|---|
| executed | **2026-08-18**: the 6 old/new pairs 02:31:35 → 02:38:46 UTC, the 5 old/mid/new rounds 02:43:50 → 02:49:00 UTC |
| **engines** | three, each measured from a **copy** of its binary taken before the first cell and never rewritten |
| | `c500d4a` — "fix: a transaction's assertion reservations are settled exactly once" era; the commit `results-scenario2-freight.md`'s superseded 2026-08-11 section was measured at. `sha256 85e91d26…` |
| | `2bd5030` — "fix: Location and TupleLocation carried spans that outlived their pins"; **51 commits** after `c500d4a`. `sha256 9b8efe70…` |
| | `92c76dd` — "feat: DROP TABLE is atomic inside a transaction (DT5)", `origin/main`; **48 commits** after `2bd5030`. `sha256 13907114…` |
| builds | all three Release (`-O3 -DNDEBUG`), gcc 13.3.0, from pristine `git archive` exports of their own commits. `92c76dd` and `2bd5030` link OpenSSL (`KDS_WITH_TLS=ON`, 3.0.13); `c500d4a` predates the option and has no TLS to build |
| driver | `tools/scenario2_freight.py`, **byte-identical at all three commits** (`git diff c500d4a..92c76dd -- tools/scenario2_freight.py` is empty), run from the `92c76dd` tree against every server |
| cells | `--organizations 2000 --ships 200 --operations 2000 --cargos 2000 --bookings 1500 --seed 1 --verify 25`; fresh server and fresh data file per cell; `cores = 1`, `durability = group`, everything else default |
| interleave | old/new pairs ×6, then old/mid/new rounds ×5 — **11 cells per endpoint engine, 5 for the midpoint**, alternating so device drift falls on both sides equally |
| ports | 15502 (`c500d4a`), 15503 (`2bd5030`), 15501 (`92c76dd`) — one server at a time |
| device / host | ext4 on `/dev/root`, data files under `$HOME/bench-s2-{ab,bisect}/`, **not tmpfs**. AMD EPYC 9V74, 2 vCPUs, Azure, kernel 6.17.0-1022-azure |
| machine quiet | every cell gates on `bench/wait_quiet.sh` and samples load for its own life; no compiler ran during the session |
| verify | every cell: 1,500 committed, 8,430 charge rows, 100 invariant checks, **0 failures**, 0 conflicts — on all three engines |

**The three binaries are not build-flag identical, and cannot be.** `c500d4a`
predates OpenSSL entirely, so it has no TLS channel, no SCRAM and no
authorization code compiled in; `2bd5030` and `92c76dd` are built
`KDS_WITH_TLS=ON`. That is not a confound introduced by the measurement — it
is part of what the commit range added — but it does mean the first leg
prices "the engine gained a TLS/auth-capable dispatcher" rather than "the same
dispatcher got slower", and the two cannot be separated from outside.

**One outlier is retained rather than discarded.** `ab-old-1`, the first cell
of the session, ran cold and is the worst cell on every phase (263 TPS,
3,222 µs commit, `freight-insert` 60.6 µs against a 42 µs median). It is left
in the pool and reported in the ranges below; the statistic is the median, so
it changes no conclusion. Its presence is why the old side's ranges are wider
than the new side's on two write rows.

**Why 2,000 cargos and not 100,000.** The eight statements cost the same at
every cargo count — `results-scenario2-freight.md` §9 measures 54.0 → 56.1 µs
for the pk lookup across a fiftyfold ladder — and the smaller load is what
buys 27 cells inside one quiet window instead of 6.

## 2. What moved

Per-statement mean, one row per statement, medians across cells:

| statement | old median | old range | mid median | new median | new range | new − old | distributions overlap? |
|---|---:|---:|---:|---:|---:|---:|---|
| cargo-lookup | 49.8 | 49.4–51.0 | 51.0 | 52.2 | 51.8–55.4 | **+2.4 µs (+4.8%)** | **none** |
| credit-lookup | 43.5 | 43.0–44.5 | 44.2 | 45.5 | 44.8–49.8 | **+2.0 µs (+4.6%)** | **none** |
| capacity-read | 41.9 | 39.4–42.9 | 42.7 | 43.9 | 43.2–47.0 | **+2.0 µs (+4.8%)** | **none** |
| recipe-read | 49.9 | 48.9–50.6 | 49.9 | 51.5 | 50.9–55.0 | **+1.6 µs (+3.2%)** | **none** |
| freight-insert | 42.0 | 40.8–60.6 | 41.9 | 42.5 | 41.6–45.9 | +0.5 µs (+1.2%) | yes |
| charge-insert | 34.4 | 30.8–40.2 | 34.7 | 35.0 | 34.4–38.1 | +0.6 µs (+1.7%) | yes |
| operation-update | 39.6 | 38.3–40.0 | 40.1 | 40.1 | 39.4–44.2 | +0.5 µs (+1.3%) | yes |
| org-update | 38.2 | 34.8–39.0 | 38.8 | 39.0 | 38.4–71.6 | +0.8 µs (+2.1%) | yes |

*(µs, client-measured round trip, `n = 11 / 5 / 11` cells)*

| per booking | old | mid | new |
|---|---:|---:|---:|
| the eight statements, summed | **492.8** | 498.9 | **507.0** |
| against `c500d4a` | — | **+1.2%** | **+2.9%** |
| against `2bd5030` | — | — | **+1.6%** |
| TPS (median) | 521.3 | 530.7 | 525.3 |
| commit mean (median) | 1,324.5 | — | 1,283.8 |

Three things follow.

**The reads carry it, and the signal is categorical rather than statistical.**
On all four read statements the slowest of eleven old cells is faster than the
fastest of eleven new cells. That is not a margin argument — there is no
threshold to choose, because the two sets do not touch. On all four write
statements the sets overlap and the difference is inside the run-to-run
spread, so the honest reading is that the writes moved by an amount this
measurement cannot resolve.

**It is about +2 µs per read, not +5%.** The four reads gained +1.6 to
+2.4 µs each while costing 42–50 µs; the writes gained +0.5 to +0.8 µs while
costing 34–42 µs. A roughly constant per-statement addition, biggest on the
statements that do the most page work, is what a fixed per-statement or
per-page-access cost looks like — not what a proportional slowdown of the
executor's inner loop would look like.

**Throughput cannot see it, which is why this file exists.** The old and new
TPS medians differ by 0.8%, in the *opposite* direction from the statement
cost, because a booking is 507 µs of statements behind a 1,284 µs fsync whose
own drift is eight times larger than the effect. Anyone re-running the
scenario matrix and reading its TPS column would conclude nothing changed.

## 3. Where in the range

The midpoint says the cost is not one commit and not one subsystem: **+1.2%
of the +2.9% is already present at `2bd5030`, and the remaining +1.6% arrives
after it.** What each half contains, as candidates and not as attributions —
this measurement has three points on a 99-commit line and cannot name a
commit:

**`c500d4a..2bd5030`, 51 commits.** The `PageRef` migration and armed
eviction (MG01–MG06): every `PageStore` accessor now returns a pinned handle,
so every page touch takes a pin and a release it did not take before. This is
the read path by construction. Also here: direct TLS, SCRAM and
statement-class authorization (a per-statement role check at the dispatcher,
even with `auth = off`), `owner_oid` in the common page header, general
`ORDER BY`, key mode, and recovery at mount.

**`2bd5030..92c76dd`, 48 commits.** The executor's coroutine conversion and
the whole cross-core pipeline series (P4d-2 through P4d-4c and P4e), which
includes `ea30544` — "awaits live at the page boundary" — putting a
suspension point exactly where a read statement crosses pages. And the
transactional DDL series (DT1–DT7), of which `7a2e5fe` ("catalog reads answer
what the reader's view can see"), `b2c5dd9` ("statement resolution answers
what the session can see") and `9bdf5df` ("every route into a relation agrees
on whether it exists") all add per-statement catalog resolution work in front
of every statement, read and write alike.

**This does not contradict `bench/results-p4d-executor.md`.** That document
measured `2bd5030..95946c4` — the first 8 commits of this second half — and
found the fixed per-statement component "did not survive" the terminal split,
at −0.6 to +0.1 µs on its own workload. The leg measured here is six times
longer than that range and ends 40 commits later, and its workload is a
different one. The two results are consistent with the second half's +1.6%
having arrived in the 40 commits that document did not cover.

## 4. What this does not answer

- **Which commit.** Three points on a line of 99. Naming one needs a real
  bisect: each step is a Release build, about 25 minutes on this box, so a
  7-step bisect over either half is an afternoon and would want the read
  statements alone as its signal.
- **Whether +2 µs per read is worth anything.** It is 0.1% of a booking on
  this workload, invisible behind the fsync. On a read-only workload with no
  durability point in it — `scenario1_backtest.py`'s QPS matrix — the same
  +2 µs would be a 4–5% result. That measurement has not been made and this
  file should not be read as having made it.
- **Whether the writes moved.** They are +1.2% to +2.1% with overlapping
  distributions across 11 cells a side. More cells would resolve it; this run
  did not.
- **Anything at scale.** All 27 cells are one connection, 1,500 bookings,
  2,000 cargos, `cores = 1`. Cross-core, concurrency and the 100,000-cargo
  matrix are `results-scenario2-freight.md`'s, measured at `92c76dd` only.
