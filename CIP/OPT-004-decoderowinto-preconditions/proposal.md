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

**A/B still owed.** The predicted win is low single digits and the
change cannot regress a result, so it was landed on the suite alone —
but "predicted small" is not "measured", and until an interleaved
server-CPU A/B says otherwise this entry carries *overhead not
measured*, not an implied pass. OPT-002's run is the instrument to copy,
and its finding applies here too: `full-scan` cannot see a decode
saving (render and wire are 96.9% of it), so `analyze-scan` and `update`
are the shapes to run.
