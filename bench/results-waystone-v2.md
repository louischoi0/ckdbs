# Waystone v2 — recording and replay, measured

Workplan P14. First measured 2026-08-02 on the build that landed P08–P13
(recording, replay, the advisory-contract suite); **re-measured 2026-08-03**
after two parser changes that between them removed most of Waystone's
overhead — folding the fingerprint into the parse pass, then zero-copy
tokens.

**Headline: replay does what spec §7 predicted.** On a relation with no pk
index it is a 22–33× improvement. On a B+ tree relation it is a small
regression — now 3–6%, down from 13–15% before the fingerprint was folded
into the parse pass (2026-08-03).

The first measurement of this feature blamed the B+ tree loss on
`FingerprintOf` lexing the statement a second time before replay could even
be attempted. **That diagnosis was right and the fix confirmed it**: folding
the fingerprint into the parse removed most of the gap, and the btree join
went from +15.2% to +3.5%. What is left is the replay path's own cost — a
trail page read, an index build, and the recorder's write-side work.

---

## Method

Single connection over the newline protocol, per-request wall clock around
one send + one recv, so every number carries Python's own socket cost
(~200 µs floor — see `tools/bench_common.py`). Read the *differences*, not
the absolutes. 2000 rows per relation, one hot instance repeated 3000 times
after a 20-execution warm-up, p50 reported.

Two servers, identical but for `waystone_recording` / `waystone_replay`
(both `on` vs both `off`).

## Single-relation point lookup

| relation | waystone off | waystone on | change | 2nd lex | +2nd lex, +copying tokens |
|---|---|---|---|---|---|
| B+ tree | 221.2 µs | 236.1 µs | **+6.7%** | +5.9% | +13.1% |
| heap | 7910.8 µs | 235.7 µs | **−97.0% (34×)** | −96.9% | −96.8% |

## Two-relation join, one hot instance

| driving relation | waystone off | waystone on | change | 2nd lex | +2nd lex, +copying tokens |
|---|---|---|---|---|---|
| B+ tree | 294.1 µs | 302.3 µs | **+2.8%** | +3.5% | +15.2% |
| heap | 8019.9 µs | 303.6 µs | **−96.2% (26×)** | −95.9% | −95.5% |

The two right-hand columns are the same measurement on the two earlier
builds, kept because the deltas between them are what the parser changes
were worth. Almost all of the B+ tree regression was the second lex.

## What the parser changes were worth on their own

Zero-copy tokens are not a Waystone change — they make *every* statement
cheaper, whether or not a trail is involved. Measured on the **waystone-off**
baseline, which is pure parse-and-execute:

| statement | before zero-copy | after | change |
|---|---|---|---|
| btree join, short names | 319.8 µs | 294.1 µs | **−8.0%** |
| btree point, short names | 228.3 µs | 221.2 µs | −3.1% |
| btree join, long names (`join_benchmark.py`) | 318.8 µs | 299.5 µs | **−6.1%** |

The short-name rows are the interesting ones: those identifiers all fit the
small-string optimization, so **nothing was being allocated for them even
before**. The gain there is `Token` becoming trivially copyable — it is
returned by value from `Next()` and stored in the lookahead slot, and that
used to move a `std::string` every time. The allocation saving shows up in
the long-name row on top of it.

## Random-instance workload (`tools/join_benchmark.py`, 2000 rows, 3000 ops)

| phase | waystone off | waystone on | change |
|---|---|---|---|
| join-point, today | 299.5 µs | 324.8 µs | **+8.4%** |
| join-point, before both parser changes | 318.8 µs | 383.6 µs | +20.3% |

Included because it is the *unfavourable* shape and worth naming: 2000
distinct instances over 3000 executions means most instances are seen ~1.5
times, never reach `n = 2`, and never record. The workload pays the
fingerprint on every statement and replays almost nothing. This is not a
Waystone failure — it is Waystone correctly declining to record one-shot
work — but it is what a benchmark that picks random keys measures, and
anyone quoting `join_benchmark.py` should know that.

---

## What the numbers say

**Spec §7 called both halves.** It predicted "a full chain scan becomes one
read — **large**" for a heap relation, and "one descent becomes one read —
**modest, and honestly not the reason to build this**" for a B+ tree. Both
hold, and the second one is now close to literally true: the btree case has
gone from clearly negative to a few percent, which is about what "modest"
buys once the overhead is a few µs rather than a second lex.

**Why B+ tree replay still loses a little.** A descent on a 2000-row tree is
two or three page touches — genuinely cheap. Replay costs a trail page read,
an index build, and the recorder's write-side work on the way back out. There
is not much descent to save, so a few µs of overhead is the whole margin.
The remaining candidates, unmeasured: `TouchPattern` dirties the
`sys.patterns` page on every recorded execution, and the trail is re-read on
every execution even when it has not changed.

**Against spec §7's own bar** — "a pattern trail should match [a validated
point lookup] on the single-relation case and beat a btree descent chain on
the join case, or it has not earned its complexity" — the verdict is still
split, but much less starkly than the first measurement suggested: it clears
the bar decisively on heap relations and is **marginally behind on btree**,
by single-digit percent rather than 15%. Recorded rather than tuned away,
per P14.

**Both parser fixes are now in** (`docs/parser-v2.md`'s zero-copy tokens
landed 2026-08-03). Lexing allocates nothing: token text is a view into the
statement, and the fingerprint folds case as it hashes rather than into a
per-token buffer. What is left of Waystone's B+ tree overhead is its own —
a trail page read, an index build, and the recorder's write-side work.

---

## What to do with this

1. **Heap relations are where Waystone earns its keep**, and that is a real
   result rather than a consolation: it means a relation can get pk-keyed
   acceleration for *observed* patterns without carrying an index at all,
   which is spec §1's third consequence paying off.
2. **The btree numbers are now a fair Waystone verdict**, which the first
   measurement's were not — that one charged the subsystem for a parser cost
   since removed. If the remaining few percent is worth chasing, the two
   candidates named above (`TouchPattern`'s page dirty, re-reading an
   unchanged trail) are where to look, and both are measurable in isolation.
3. `waystone_replay = off` remains defensible for a deployment that is
   entirely B+ tree clustered, but the case is much weaker than it was: a
   few percent, not fifteen.

## Not measured

Multi-core (the server serves one connection at a time), trail replay under
concurrent writes (no transaction manager), and retention/eviction (P15, not
built). The correctness side is not benchmarked at all — it is
`tests/waystone_contract_test.cpp`, which compares five configurations byte
for byte.
