<!-- One proposal, one directory. The register, the seven fields every
     entry carries and the standing constraints are in ../README.md. -->

# OPT-002 — Every decoded string costs a malloc+free the codec's own header says it should not

**Hypothesis.** `src/exec/row_codec.cpp:532` builds a fresh
`std::string`, fills it byte-at-a-time, and *move-assigns* it into the
slot; the `char(N)` arm (`:487-497`) and `ResolveSpills` (`:1062-1067`)
do the same. The move frees the slot's existing buffer and adopts a new
one, which **defeats the buffer reuse the file's own header claims at
`:430-433`**. For any value past libstdc++'s 15-byte SSO that is one
malloc + one free per string column per decoded row —
`tools/benchmark.py`'s `TEXT_LEN = 16` sits deliberately just past it.
Replacing both with `assign(ptr, n)` (one `memcpy`, capacity reused)
should remove ~40-80 ns per row per string column: **5-15% on scan
shapes that project text**, against the 0.05-0.12 us/row fitted in
`bench/results-scenario1-vs-pg.md` §6, and it compounds with OPT-001,
which decodes strings twice per rejected row.

**Measurement.** `tools/benchmark.py --json`, `full-scan` and `update`
phases, interleaved A/B in `build-release`. `ANALYZE SELECT * FROM t`
must show `examined=` and every other counter **unchanged** — this
change may not move a single counter, which is what makes an A/B the
only instrument.

**Reason.** It is fifteen lines against a cost paid per row per string
column on every shape that projects text, and the file already documents
the behaviour it fails to deliver.

**Pros / cons.** Pro: no design question, no counter change, cannot
regress a result. Con: none worth the name — the only thing at stake is
the `char(N)` arm's read-back-to-first-NUL contract
(`row_codec.cpp:182-197`), which the rewrite must keep with an explicit
`memchr` rather than inherit by accident.

**Consistency and sanity.** No hard invariant. Invariant 13's
fixed-length cell rule is untouched: this changes how bytes reach an
`AstValue`, never how many bytes a cell has. Proof: the types contract
suite (which compares configurations byte-for-byte) and the full suite.

**Implementation.** Branch `opt-002-string-slot-assign` at `55d2c0b`
(`v2.7.0-30-g55d2c0b`), cut from `dfe4c98` on `worktree-path-optimizer`
and pushed to
`https://github.com/louischoi0/ckdbs/tree/opt-002-string-slot-assign`.
One `AssignBytes` helper does a single `memcpy` into the slot's existing
capacity; the `char(N)` arm keeps its read-back-to-the-first-NUL
contract explicitly with `memchr` rather than inheriting it from a loop
that happened to stop there. Release suite **3088/3088 green** at that
commit, and the pre-push hook's own Debug suite green again on push.

**Measured — and the entry's own prediction was wrong about the shape.**
`bench/v2.7.0/results-opt002-string-slot-assign-v2.7.0-30-g55d2c0b.md`,
arm A `dfe4c98` against arm B `55d2c0b`, both built from a clean
`git archive` of their own commit, interleaved with one RNG stream
driving both arms:

| Shape (10,000 rows) | Delta | Verdict |
|---|---|---|
| `full-scan` | -0.86% / 0.00% / +0.43% over three independent setups | **no effect** — the prediction named this shape first and it is flat |
| `analyze-scan` (walk + decode, no render or wire) | +18.3% (@1,000: +27.0%) | the mechanism, isolated |
| `update` | +18.9% (@1,000: +13.8%), corroborated at +16.6% and +19.2% | where the win reaches a client |
| `point-select` (control) | flat | flat, as a control must be |

**What that costs the entry, stated rather than glossed:** the
hypothesis predicted 5-15% on text-projecting *scans* and got 0% there.
The reason is measurable rather than speculative — `ANALYZE` shows the
render, wire and client-parse portion `full-scan` adds is **96.9%** of
its latency at 10,000 rows, so a real decode saving cannot surface
through it. The mechanism was right, the instrument named in the entry
was the wrong one, and `analyze-scan` is what should have been named.
The largest end-to-end win landing on `update` is the second finding:
UPDATE decodes every scanned row *including its strings* before testing
the WHERE, which is OPT-001 — so OPT-002's win there is a measure of
OPT-001's defect, and it will shrink when OPT-001 lands. That is a
correct outcome, not a regression.

**Sanity.** No `ANALYZE` counter differed between arms at any size
(`examined=`, `pages=`, `opens=`, `pattern_id=`), and a byte-for-byte
row spot-check plus `COUNT(*)` matched — which is the check that
outranks the timing.
