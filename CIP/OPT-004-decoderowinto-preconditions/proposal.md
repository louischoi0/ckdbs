<!-- One proposal, one directory. The register, the seven fields every
     entry carries and the standing constraints are in ../README.md. -->

# OPT-004 — `DecodeRowInto` still pays the Status-constructing preconditions AP02 removed

**Hypothesis.** `row_codec.cpp:979-980` calls `CheckKeystoneColumn` (an
out-of-line, cross-TU call returning a `Status` that carries a
`std::string` by value, `src/catalog/schema.cpp:79`) and
`CheckLayoutMatches` **per decoded row**. The sibling
`DecodeColumnsInto` was already fixed by AP02 via `CheckDecodeInputs`
(`:868-900`), whose comment states the rule — *the predicate first, the
Status only on failure* — and `DecodeRowInto` never got it. Hot callers:
UPDATE `:8175`, DELETE `:9109`, and every step compiled `kAllColumns`
including every step of a chain carrying a sub-chain
(`step_compiler.cpp:1565-1567`). AP02 measured 19-35% on this class;
here expect **low single digits**, but on ~5 lines that cannot regress.

**Measurement.** The same interleaved server-CPU A/B in
`build-release`; `tools/aggregate_benchmark.py` for a
sub-chain-carrying shape.

**Reason.** It is the identical fix already ratified and measured
elsewhere in the same file, left on one function.

**Pros / cons.** Pro: identical checks, identical messages, no
behavioural surface. Con: none — the win is small, and the entry says so
rather than inflating it.

**Consistency and sanity.** No hard invariant. `CheckDecodeInputs`
already performs exactly these checks, so the refusal text a client sees
must be byte-identical; the decode-error tests are the proof.

**Implementation.** Branch `opt-004-decoderowinto-preconditions` at
`b05925f` (`v2.7.0-31-gb05925f`), cut from `55d2c0b`, pushed to
`https://github.com/louischoi0/ckdbs/tree/opt-004-decoderowinto-preconditions`.
`DecodeRowInto`'s four preconditions now go through `CheckDecodeInputs`,
the same function its sibling uses, so the second statement of them is
gone along with the per-row `Status`. Release suite **3088/3088 green**
at that commit; the pre-push hook's Debug suite green again on push.

**Measured — and the honest answer is "inside the noise floor".**
`results-opt004-decoderowinto-preconditions-v2.7.0-38-gea1d9d0.md`,
beside this file, driver and raw output under `archive/`. Two arm pairs
were run, because they answer different questions:

- **Pair 1**, the change in isolation on the tree it was written
  against: `55d2c0b` against `b05925f`.
- **Pair 2**, what it is worth *today*: `ea1d9d0` with `DecodeRowInto`'s
  preconditions hand-reverted, against `ea1d9d0` unmodified. OPT-001 has
  since cut how often `DecodeRowInto` runs at all, so pair 1 alone would
  overstate what remains.

**No pair, at any of three sizes, shows a reproducible,
correctly-directioned, size-scaling effect** in any shape where
`DecodeRowInto` runs per row. The best-supported positive signal is pair
1's `update` at +0.77% to +0.86% — the bottom edge of "low single
digits", and not a number worth calling a win. The prediction was low
single digits and it is at or below what this harness can resolve.

**The two-pair design is what kept this honest, and that is the finding
worth keeping.** Pair 1's `analyze-scan` at 10,000 rows shows a large,
well-resolved **-9.06%** against a 256-tick floor — reported alone, that
reads as a real regression. Pair 2 measures the *byte-identical*
`CheckDecodeInputs` code in a differently-linked binary and shows -0.33%,
inside floor. The non-reproduction is the evidence: **for a change this
small, inter-binary code-layout noise is comparable to the effect being
measured**, and a within-run floor understates the true floor. The same
class of artifact was named in OPT-001's results file for its own
unexplained outlier.

Likewise pair 2's `update` shows a consistent, floor-clearing
-8.8%/-6.6%/-10.5% — and it is *not* attributable to this change:
under OPT-001's mask, `DecodeRowInto` runs at most once per statement in
that shape whatever N is, so a per-row change cannot produce a delta
that large and that insensitive to size.

**Correctness:** 12 of 12 byte-identical checks across both pairs and
all sizes (table hash, `COUNT(*)`, full `ANALYZE` reply hash). Pair 2's
hand-reverted tree also passed the full suite **3091/3091**, including
the test that exercises the reverted refusal path — confirming the
revert was behaviourally inert, which is what makes the comparison a
measurement of cost rather than of behaviour.

**What this entry concludes.** The change stays: the checks and their
messages are identical by construction, it deletes a second statement of
four preconditions that were free to drift apart, and it cost nothing to
land. But it buys **no measurable performance** at any size this harness
can resolve, and the entry says so rather than quoting AP02's 19-35% as
though it had been reproduced here. A defensible change with an
unmeasurable win is a different thing from a win, and conflating them is
how a register stops being worth reading.

**One caveat carried rather than hidden:** pair 1's `rows=10000` redo had
an unrelated build (worktree `xf`) appear for part of its window. It was
kept, with the reasoning stated in the results file — its CPU pass ran
*faster* than the run discarded for contamination, and its floors are
tight — but it is not a pristine cell.
