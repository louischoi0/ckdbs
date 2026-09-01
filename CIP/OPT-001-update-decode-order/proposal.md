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

**A/B owed.** OPT-002's run is the instrument, and its finding governs
the shapes: `full-scan` cannot see a decode saving, so `update`,
`delete` and `analyze-scan` are what to run. Expect OPT-002's own
`update` number to *shrink* once this lands — that number was partly a
measure of this defect.
