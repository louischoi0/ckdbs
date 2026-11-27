# Work order EA - a prepared statement parsed once: the parse kept across executions, and the compile's reuse put before the operator again

Status: **OPENED 2026-11-27** on worktree `ea-prepared-statement-reuse`
(branch `worktree-ea-prepared-statement-reuse`) from `481d101aa`
(`v3.4.17-202-g481d101aa`), on the operator's word of 2026-11-27, a standing
go-ahead scheduled four hours ahead with "mark as proposed" and the push at
the close (`raft-marks-2026-11-27.md` §11). **EA-Q0..Q5 are marked (a) on
that word, each as CLA proposed it** (§4); EA-Q0 (a) is the word BN-Q13's
standing waited for, and EA-Q3 (a) leaves CX-Q37 (a) standing. **EA-S0 done**
(`bench/v3.4.17/results-ea-s0-premise-v3.4.17-202-g481d101aa.md`): the parse
is 12.2 - 20.9 % of a prepared statement's server CPU on every shape, so
EA-Q1's bar is not met and §2.1-§2.3 are built. Written 2026-11-26 on worktree
`known-gaps-milestone-orders` (branch `worktree-known-gaps-milestone-orders`)
from `8274c951` (`v3.4.17-148-g8274c951e`) on the operator's word of
2026-11-26 (`raft-marks-2026-11-26.md` §3). A `[source-read]` claim with a
`path:line` in §0-§2 was read at `8274c951`; a claim taken from
`known-gaps.md` carries that entry's own verification commit and is not
re-verified here.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/spec/protocol.md` (PARSE / BIND / EXECUTE);
`docs/spec/parser-v2.md` I12; `docs/inflight/known-gaps.md`, "The read
and update paths (BN)", "Captured plans (CX)";
`instructions/v3.0.0/workorder-bn-opt-read-update-path.md` (BN-R1
`[quiet-wrong]`, BN-Q13);
`instructions/v3.2.0/workorder-cx-managed-plans.md` (CX-Q10, CX-Q37);
`include/kds/parser/fingerprint.hpp` (the bump rule).

## 0. What EA is

`[source-read]` A prepared statement is substituted into its text and
parsed again at every execution (`known-gaps.md:1481`). `[design]` EA
keeps the **parse** of a prepared statement and binds its values into
it, so an execution lexes and parses nothing and a value never passes
through the string-literal grammar. Whether the **compiled chain** is
also kept is the question CX-S6 answered "no" on a measurement; EA puts
it before the operator once more with the parse's cost removed from
under it, and builds it only on a mark.

## 1. The tree as read

`[source-read]` from `docs/inflight/known-gaps.md` at `8274c951`:

- **The parse** (`:1481`, BN-R1 not built, BN-Q13): parse and compile
  are 1.1-4.5 us of a point statement's server CPU after BN, 5-15 % of
  it (`[measured]`
  `bench/v3.0.0/archive/bn-close-v2.7.0-787-g8679367e/partsB.md`). "A
  client that prepares is the only one a cached parse would help; no
  driver in the tree prepares." **A text parameter containing a single
  quote stays unbindable at `C_BIND` for the same reason.**
- **Two compiles** (`:2752`, verified at `29401d0f`): a block run in an
  order other than its written one compiles twice - 5.3 us against
  2.5 us for a three-relation join, in-process - and there is no plan
  cache (CX-Q37 (a)). In the server `exec::Compile` is 5.7 % and 6.3 %
  of a two-variant plan's client p50, 2.8-4.1 % for every plain shape
  (`[measured]`
  `bench/v3.4.12/results-cx-s6-compile-share-v3.4.12-112-g9712f5f0.md`
  §8). "Skipping the first compile is not built and is not free: the
  written order's refusal is the statement's."
- **A second lex** (`:2741`): a multi-variant plan lexes the statement
  again at every compile to read its arguments
  (`src/exec/admin_plans.cpp:47`, `parser::ArgumentsOf`).
- **A shape whose relation never gets a prior is searched at every
  compile, with no memo** (`:2909`).

`[source-read]` CX-Q37's adopted reasoning
(`workorder-cx-managed-plans.md:589`): "a chain cached without its
literals and bound again per statement is the one structure in this
order that could answer wrongly if a literal lands in the wrong slot."
`[source-read]` The string-literal grammar has no escape, deliberately
(`src/parser/lexer.cpp:120`); EA does not change it.

## 2. The mechanism `[design]`

1. **The kept parse.** `C_PARSE` stores the statement's AST with each
   parameter a typed hole that records its byte position. `C_BIND` fills
   the holes with values - never text - and `C_EXECUTE` hands the bound
   AST to the compile. The fetch id and the fingerprint are computed
   once at the parse, from the text, exactly as today.
2. **A quote in a parameter** binds, because the value is never lexed.
3. **Plan arguments.** A multi-variant plan's rule reads the bound AST's
   values where a kept parse exists, so the second lex is not made
   (`:2741`); a text statement lexes as today.
4. **Validity.** A kept parse holds no catalog state: names are resolved
   at every compile, as today, under the relation `IS` the bind
   declares. A kept parse therefore cannot go stale, and that is why it
   is the first stage.
5. **The compile's reuse** (only on EA-Q3's mark): a chain kept per
   statement handle, per session, keyed by the schema version word, the
   plan and statistics generations and the session's namespace, with
   every literal a slot the bind writes. It is CX-Q37's option (c)
   narrowed to prepared statements.

## 3. Rules

- **EA-R1. A prepared statement's answer is its text statement's**
  `[quiet-wrong]`: for every shape, the bound AST equals the AST the
  substituted text parses to, value for value and type for type. A
  differential cell runs the golden corpus both ways.
- **EA-R2. `kFingerprintVersion` does not move**, and a prepared
  statement's fetch id is its text's; the golden corpus pins it.
- **EA-R3. A refusal carries the byte of the text the client sent**: a
  hole's byte is the parameter marker's.
- **EA-R4. A statement that is not prepared executes no new
  instruction.**
- **EA-R5. A kept chain, if built, is never run under a catalog, plan or
  statistics generation other than the one it was compiled under**
  `[quiet-wrong]`, and the relation `IS` - not the word - stays what
  keeps a stale parse from executing. A literal's slot is typed, and a
  bind of another type recompiles.
- **EA-R6. 64 statements and 64 portals a session stay the bounds**; a
  kept parse's memory is counted against a stated per-statement cap.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (EA-Q), CLA's proposals

| | item | options | CLA proposes | mark |
|---|---|---|---|---|
| EA-Q0 | Is EA opened? BN-Q13 left BN-R1 unbuilt | (a) yes, for the kept parse; (b) no | (a) | (a), the operator's "mark as proposed" (2026-11-27) |
| EA-Q1 | Does EA-S0 gate? | (a) S0 measures a prepared point `SELECT`, `UPDATE` and a three-relation join through a client that prepares, parse and compile apart; under 3 % of server CPU saved by the parse on every shape, EA builds §2.2 only (the quote) and stops. (b) built regardless | (a) | (a), the same word; the bar read at S0: not met (§7) |
| EA-Q2 | A client that prepares | (a) `tools/kwp.py` gains a prepare path for the measurement and the cells; (b) a C++ test client only | (a) | (a), the same word; `tools/kwp.py` already had it (BN-S1's `prepare` and `execute_prepared`), so nothing was added |
| EA-Q3 | The compiled chain (CX-Q37) | (a) stays unbuilt, CX-Q37 (a) standing; S0 reports what it would save with the parse already removed. (b) built for prepared statements, as §2.5; (c) built for every shape | (a) | (a), the same word; S0's figure in §7; EA-S3 not run |
| EA-Q4 | The double compile (`:2752`) | (a) outside EA: the written order's refusal is the statement's; (b) inside | (a) | (a), the same word |
| EA-Q5 | `C_DESCRIBE` of an unexecuted statement | (a) stays refused; (b) answered from the kept parse where it needs no catalog read | (a) | (a), the same word |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **EA-S0** | The premise: lex, parse, fingerprint and compile timed apart on a prepared and a text path, in `build-release`, server CPU, interleaved. The census of every place a parameter's text is substituted | a results file; EA-Q1's bar read; EA-Q3's figure |
| **EA-S1** | The kept parse, typed holes, the bind by value; the quote | EA-R1's differential cell; EA-R2; a cell per storable type and NULL |
| **EA-S2** | Plan arguments read from the bound AST | cells: a multi-variant plan chooses the same variant by either path |
| **EA-S3** | EA-Q3 as marked, if marked beyond (a) | its own design reviewed before code; EA-R5's cells |
| **EA-S4** | The close: overhead A/B on a text statement (EA-R4) and the saving on a prepared one, `protocol.md`, `client-manual.md`, `known-gaps.md` | the operator's word on landing |

## 6. What EA does not do

- No escape in the string-literal grammar, no `$name` parameter
  (withdrawn with `CREATE PATTERN`).
- No cross-session statement cache and no plan cache for text
  statements.
- No change to which plan a statement gets.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-26

On `known-gaps-milestone-orders` from `8274c951`, on the operator's word.
No stage is opened and no code is written. **Not reviewed**: the
`critics-developer` review was started and stopped before it reported,
so no line reference or reading in this order has a second reader behind
it.

### Opened 2026-11-27

On worktree `ea-prepared-statement-reuse` from `481d101aa`, on the operator's
word (`raft-marks-2026-11-27.md` §11); EA-Q0..Q5 marked (a) on it (§4).

### EA-S0 - done 2026-11-27

`bench/v3.4.17/results-ea-s0-premise-v3.4.17-202-g481d101aa.md`, measured on
`ea-prepared-statement-reuse` at `481d101aa` with a spike that times the parts
inside the server (archived beside the results, never in the tree), driver
`tools/ea_s0_premise.py`.

- **EA-Q1's bar**: the parse is 12.2 - 12.9 % of a prepared point `SELECT`'s
  server CPU (0.77 of 6.07 us), 11.9 - 12.0 % of a point `UPDATE`'s (0.76 of
  6.37 us) and 19.2 - 20.9 % of a three-relation join's (1.93 of 9.38 us),
  every round. Not under 3 % on any shape: §2.1-§2.3 are built.
- **EA-Q3's figure**: with the parse removed, `exec::Compile` is 8.1 - 8.2 %
  of the point `SELECT`'s server CPU, nothing of the `UPDATE`'s (a write by
  its key compiles no chain) and 18.9 - 22.1 % of the join's. EA-Q3 (a)
  stands; the figure is the operator's to weigh against CX-Q37.
- **The census** (the results file's §6): a value is substituted in one place
  (`kwp_session.cpp`, `Substitute`); each statement kind's own parse is one
  call (`HandleSelect`, `ParseKeepingShape`); `WithReturning`, the query
  layer, a multi-variant plan's arguments, a SET value's scalar subquery and
  a re-run after a wait read the text again.
- **Outside EA, found on the way**: a text statement over KWP lexes its text
  twice (`C_PARSE`'s `FingerprintOf`), 3.4 - 8.1 % of its server CPU;
  EA-R4 leaves it, and it is in `known-gaps.md`.
