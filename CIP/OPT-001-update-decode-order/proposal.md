<!-- One proposal, one directory. The register, the seven fields every
     entry carries and the standing constraints are in ../README.md. -->

# OPT-001 — UPDATE and DELETE decode every scanned row before testing the WHERE

**Hypothesis.** `apply()` in `src/server/command_dispatcher.cpp:8146`
decodes the whole row *twice* (`:8168` `DecodeRow` into a fresh
`std::vector<AstValue>`, `:8175` `DecodeRowInto` the frame slots),
resolves spills twice (`:8191`, `:8192`), and only then evaluates the
predicate (`:8196`). DELETE has the same shape with one decode
(`:9109` / `:9121` / `:9125`). Because `LocateByPk` returns `kScan` for
every heap relation (`:6765`), `UPDATE t SET c=x WHERE id=n` runs that
body on **every slot of the chain**: at `tools/benchmark.py`'s defaults
(heap, 2,000 rows) that is 2,000 full-row decodes of which 1,999 are
discarded. Reordering to *masked decode of the WHERE columns → predicate
→ full decode only on a match* should cut per-rejected-row work roughly
5-10x on a five-column row, and the **UPDATE phase's server CPU 2-4x**,
more on wide relations. This is the same defect AP01 measured at 75% of
the scan on the read path (`bench/results-scenario1-vs-pg.md`), left
standing on the write path.

**Measurement.** `tools/benchmark.py --clustered heap --update-ops 1000
--json`, plus server CPU from `/proc/<pid>/stat` fields 13-14,
interleaved A/B per `workplan-aggregate-perf.md`'s "How to measure here";
`tools/catalog_read_ab_benchmark.py` is the existing interleaved harness
to copy. There is no `ANALYZE` for UPDATE, so **server CPU is the
instrument**, not latency alone.

**Reason.** A point UPDATE by pk on a heap relation is the shape the
engine exists for, and it currently pays a full decode per row of the
relation. The masked decode this needs already exists — AP01 built it
for SELECT; `step_compiler.cpp:1303` simply hands UPDATE
`Step::kAllColumns`, whose comment ("they need every column of a
matching row anyway") is true of a matching row and false of a rejected
one.

**Pros / cons.** Pro: the largest measured win on the list, with the
machinery already built and proven on the read path. Con: it is the
entry with the most ordering to preserve — MVCC classify → masked decode
→ predicate → conflict check → full decode → FK → write — and a wrong
mask yields a *stale slot value and a wrong row*, not a crash. That is
the cost: a real bug class in exchange for the biggest win.

**Consistency and sanity.** No numbered hard invariant. It touches R1 in
`row_codec.hpp` (spills resolved with no page span live) and the
pre-SET image the `previous` copy must carry (`:8232-8236`).
`CompileWhere` must emit a real mask via `FilterColumnsOf`
(`step_compiler.cpp:1318`) rather than `kAllColumns`. Proof: the full
suite, the MVCC and FK contract tests, and a `sim/` cell whose oracle
would see any wrong-row write.

**Implementation.** Branch `opt-001-update-decode-order` at `c4799b8`
and `e156b3d` (`v2.7.0-36-ge156b3d`), cut from the merged working branch
and pushed to
`https://github.com/louischoi0/ckdbs/tree/opt-001-update-decode-order`.
`CompileWhere` emits the residual's real mask behind two structural
gates — a step carrying a sub-chain is unmaskable, and a relation wider
than 64 columns cannot be named by a `uint64_t`; the row's bytes are
copied out of the page once and decoded from the copy; UPDATE's second
full decode is gone and DELETE completes the frame with the complement
before the assertion enforcer reads it.

**Sanity, three ways.** Release suite **3091/3091 green**;
`scripts/sim.sh` **266 runs, 0 failures** at `c4799b8` — every committed
seed plus 8 fresh ones across the crash and I/O-fault modes, which is
the check aimed at this change's actual failure mode, a wrong row
written with no crash; and a `critics-developer` review of the diff that
found **no correctness bug**, having checked the mask against what
`EvaluateConjuncts` reads, both sides of a column-to-column compare,
`BETWEEN`'s lowered pair, `IS NULL` as a `CompareOp` rather than a kind,
and the 64-column boundary in all three places.

**What the review changed** (`e156b3d`): the comment justifying the
payload copy was **wrong on its facts** — a store does not move a pinned
frame, so the copy is R1 *discipline* on a path with no
`PageSpanGuard`, not a dangling-pointer fix, and the comment now says
so. The decode-then-resolve rule, written at four sites, is now one
`exec::DecodeAndResolve` — the fourth site is DELETE's complement, where
drift hands back the previous row's value instead of failing. And three
tests, because nothing asserted this mask at all: it names the WHERE's
column and only it, it gives the mask up to a sub-chain, and it gives it
up past 64 columns.

**Declined from the review, with the reason:** reusing `payload_copy` as
the undo before-image. Its premise holds today, but the before-image is
what an abort restores, and the re-read is the only thing keeping it
independent of how long the row body was suspended — wrong there is
silent corruption, against a saving of one page-store lookup on matched
rows only.

**Measured** —
`results-opt001-update-decode-order-v2.7.0-38-gea1d9d0.md`, beside this
file, with its driver and raw output under `archive/`. Arm A `004da62`
(`v2.7.0-34-g004da62`, OPT-002 and OPT-004 present, OPT-001 absent)
against arm B `ea1d9d0` (`v2.7.0-38-gea1d9d0`), both built from a clean
`git archive` of their own commit, interleaved, one RNG stream driving
both arms. Server CPU is the instrument, because there is no `ANALYZE`
for an UPDATE.

| Shape | 200 rows | 1,000 | 10,000 |
|---|---|---|---|
| `update` (point UPDATE by pk) | +35.0% | +99.5% | **+159.7%** |
| `wide` (25-column relation) | +178.0% | +436.4% | **+672.4%** |
| `delete` | +29.4% (in floor) | +50.0% (in floor) | **+33.0%** |
| `select` (flat control) | +7.6% | -1.8% | -10.9% |
| `subchain` (gated control) | +5.1% | +8.2% | +8.0% |

**The hypothesis held, and the shape of it is the evidence.** The
prediction was 2-4x on UPDATE's server CPU, more on wider relations. The
win *grows monotonically with relation size* — 1.35x, 2.00x, 2.60x —
which is what the mechanism requires: OPT-001 removes work from rejected
rows, and the rejected-row count is exactly what N scales. A flat curve
would have falsified it. On a 25-column relation it reaches **7.72x**,
because the mask's benefit is the columns *not* decoded.

**One control moved, and the brief that called it a control was wrong.**
`subchain` was specified as "must show no change", since a sub-chain
predicate is gated to `kAllColumns`. It gained 5-8%, clearing its floor
at the two larger sizes — and the reason is in the diff rather than in
the measurement: OPT-001 does **two** things, and only one of them is
gated. The masking is gated (the new test proves the gate holds); the
*decode-count* halving is not — `DecodeRow` + `DecodeRowInto` became one
`DecodeAndResolve`, and a sub-chain UPDATE still gets that. The
prediction of no change was wrong about this change's own shape;
`select`, which never reaches the altered functions, is the honest flat
control.

**Left open rather than explained away:** `select` at 10,000 rows shows
-10.9%, clearing its own floor, with no code-level mechanism — the read
path does not reach anything OPT-001 touched. It is recorded in the
results file as unexplained.

**Correctness beside the timing:** both arms' tables were sha256-matched
after the update phase — `t_main`, `t_wide` and `t_del`, core range and
count, at all three sizes, plus verbatim spot rows. Zero mismatches,
zero error replies across the run. That is the check that outranks every
number above, because this change's failure mode is writing the *wrong*
row, not crashing.

**And it settles OPT-002's inflated reading.** Arm A here reproduces
OPT-002's own earlier `update` number at 10,000 rows to within 0.6%
(1999.2 us against 1987.7 us, from an independent session), and arm B
drops ~60% below it. OPT-002's +18.87% on UPDATE was measuring this
defect: with the mask in place, `WHERE id = ?` never decodes the string
column of a rejected row at all. The CIP entry predicted that number
would shrink once OPT-001 landed, and it did.
