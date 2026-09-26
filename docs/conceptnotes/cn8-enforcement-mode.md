# CN-8 — Concept Note: Enforcement Mode — `ENFORCE | OBSERVE` on every declared invariant, the assertion arm first

Status: CONCEPT, not a work order, not a CIP. Nothing here opens a stage.
**No work starts on this note until the operator rules on §8.** Ordering
relative to CN-1 to CN-6 is not fixed. CN-7 is reserved for escrow
operations and compensable sagas (discussed 2026-09-26, not written); this
note does not depend on it.
Author: CLA, 2026-09-26, against `1f9592a`
Origin: the operator's proposal of 2026-09-26, in conversation, of a "weak
constraint" — an assertion that lets the write through and writes a
warning to the server log for every update that crosses the bound. The
operator's statement is the one `[operator]` claim in §0; the naming,
the count unit and the transition rule are CLA's proposals (§8), none
ratified.
Claim tags: `[operator]` for the operator's statement; `[source-read]`
with `path:line` at `1f9592a`; everything else `[design]`. Nothing is
`[measured]`.
Relation to `docs/spec/protocol.md`: this note reserves no frame, bit or
code. It names an existing frame (`S_NOTICE`) as a candidate carrier and
leaves the decision to that spec (O5).

---

## 0. The concept

`[operator]` *"I want to add a weak-constraint concept. For example, if an
assertion is set weak, updates and inserts proceed as they are, but a
change that crosses the condition keeps producing a warning in the server
log."*

`[design]` Restated in this tree's terms: **an invariant's enforcement is
a mode of the invariant, not a property of it.** The invariant is
declared once; whether a violating admission is *refused* or *recorded*
is a setting on the declaration, with two values:

| mode | the check | the violating write | what is recorded |
|---|---|---|---|
| `ENFORCE` (default, today's behaviour) | runs | refused, statement fails, transaction poisoned | counters (already) |
| `OBSERVE` | runs, identically | **admitted** | a violation event per would-be refusal, plus counters |

Everything up to the last step is the same in both modes. What
`OBSERVE` removes is the refusal and nothing else — not the structure,
not the check, not the cost.

The name "weak constraint" is not used below. §8 O1 says why.

## 1. What other systems ship

`[design]`, from CLA's training knowledge; to be re-checked against
vendor documentation before a work order cites it.

| system | mechanism | what it is | what it is not |
|---|---|---|---|
| Oracle | constraint states `ENABLE VALIDATE` / `ENABLE NOVALIDATE` / `DISABLE VALIDATE` / `DISABLE NOVALIDATE`, plus `RELY` | existing rows exempt, or the check off entirely, or "trust it, don't check" for the optimizer | a check that runs and admits |
| PostgreSQL | `NOT VALID` on FK and CHECK; `VALIDATE CONSTRAINT` later | new writes enforced, existing rows skipped until validated | same |
| SQL Server | `WITH NOCHECK`; untrusted constraints | same shape as `NOT VALID` | same |
| Snowflake | declared constraints, informational only | never checked | not checked at all |
| dbt / Great Expectations | test `severity: warn` | the closest analogue: the test runs, a failure is reported, the pipeline continues | a batch test over a snapshot, not a write-time admission |
| triggers with `RAISE WARNING` / `SIGNAL SQLSTATE '01…'` | user code | can be made to do this | not a declared mode; the invariant is in procedural code |

Two lessons carry into §3. **No surveyed engine has the mode as such.**
Every declared-but-not-enforced state above either skips rows or skips
the check; none runs the check on every write and admits the violator
with a record. The closest precedent is a test framework's warn
severity, which is exactly the "declare, observe, then enforce"
deployment path and is why this note exists. And **`NOT VALID` is a
different thing** and stays different (§3): it is *enforced* for new
writes and *unchecked* for old rows; `OBSERVE` is *checked* for all and
*refused* for none.

`[design]` **Is it worth it.** The value is the deployment path. An
invariant declared against live data may already be violated somewhere,
and its violation rate is unknown until it is measured; switching it on
hard against a running service stops the service. "Declare it, watch
what it would refuse, then promote it" is the only way an invariant gets
into a system that is already running, and it matters more, not less,
when the party declaring the invariant is an agent: an agent-proposed
invariant in `OBSERVE` and a human promotion to `ENFORCE` is a safe
division of authority. The cost is one: a system left in `OBSERVE`
forever is a system that believes an invariant it does not have (§7).

## 2. What this tree has

`[source-read]` unless marked.

- **The assertion check is one refusal at one step.** §6.2's protocol is:
  compute the group key; admission check — would the aggregate with every
  held contribution exceed the bound; *if yes ⇒ fail the statement with
  `AssertionViolation`; nothing was mutated or held*; if no ⇒ hold,
  reserve, write (`docs/spec/assertion.md:352-374`). `OBSERVE` replaces
  the "if yes" branch with "record, then continue as if no"; every other
  step is unchanged.
- **The refusal poisons the transaction** like every other write failure
  (`assertion.md:180-200`, AS9): in an explicit transaction the session
  enters failed-txn; in autocommit the statement unwinds fully. `OBSERVE`
  has no refusal, so it has no poisoning; §5 states what an admitted
  violation does to the transaction (nothing).
- **The counters exist and the surface names them.** `SHOW ASSERTIONS`
  reports assertions with `enforcing=` and live counters — checks,
  violations, reserved, aborted (`manual/sql/sql.md:850`); production
  counters per assertion are admission checks, violations, reservations
  rolled back by abort, hint-heal events (`assertion.md:667-680`).
  **`enforcing=` already means something else**: it reports `0` for the
  one assertion whose directory could not be rebuilt at mount
  (`sql.md:362-364`), i.e. whether the structure is *live*, not which
  mode it is in. §3 keeps that field and adds a `mode=` beside it (O4).
- **The catalog has the bits.** `sys.assertions.flags` is `u32`,
  *reserved (deferrable/not-valid bits)* (`assertion.md:657`). The mode
  is one bit in that word; no row shape changes.
- **`NOT VALID` is a reserved token** that parses and returns
  `Unsupported`, beside `DEFERRABLE` (`assertion.md:91-92`). The grammar
  has a slot for a post-predicate modifier; `OBSERVE` takes the slot
  beside `NOT VALID`, not instead of it (§3).
- **`CREATE ASSERTION` refuses on an existing violation.** AS7: the build
  scans the relation and any existing violation fails the CREATE
  (`assertion.md:61`); the manual says the same (`sql.md:356-357`). An
  `OBSERVE` create must not fail there — that is half of its purpose —
  so §3 changes what the scan does in that mode.
- **The protocol has the frame.** `S_NOTICE` is a server → client frame
  for *non-fatal server messages* (`docs/spec/protocol.md:69`), and the
  error frame's own text says a warning is an `S_NOTICE` *precisely so a
  non-fatal remark is not an error with a softer adjective*
  (`protocol.md:140`). A grep of `src/` and `include/kds/wire/` at
  `1f9592a` finds the frame named only in `error_registry.hpp:60`'s
  comment — **defined, and CLA found no emitter**. O5 is whether this
  note's warning becomes its first.
- **The server log has a level.** `LogLevel::kWarn = 3` and a `Logger`
  with a `min_level` (`include/kds/base/log.hpp:48-52`, `:142-145`).
  The operator's "server log warning" has a destination.
- **Foreign keys and transition constraints.** `FkViolation` is a status
  code of the same class as `AssertionViolation`
  (`include/kds/base/status.hpp:87`, `:195`); CN-4's transition refusal
  is a concept. Both are refusal-at-one-step shapes and take the mode
  the same way (§6); this note builds only the assertion arm.

## 3. The mechanism (assertion arm)

`[design]`

- **Declaration.** `CREATE ASSERTION name CHECK (...) [ENFORCE | OBSERVE]`;
  omitted means `ENFORCE`, so every existing declaration keeps its
  meaning and its bytes. The mode is a bit in `sys.assertions.flags`
  (§2) and part of `source_text`.
- **Admission under `OBSERVE`.** §6.2 step 2 computes the same answer.
  On "would exceed": increment the group's `would_refuse` counter, stamp
  `last_would_refuse_lsn`, emit the record (§4), and **fall through to
  the "no" branch** — hold, reserve, apply. The Bound Cabin's aggregate
  therefore goes *past* the bound, which it never does today, and stays
  an exact running aggregate; nothing in the Cabin's invariants assumes
  the bound holds (they assume the aggregate is right, which it is).
- **What is counted (O2).** One violation per **would-be refusal**: an
  admission that `ENFORCE` would have refused. Not one per write into a
  group that is already over — that turns a breached group into a line
  per statement and answers a question nobody asked. The would-be
  refusal count is the number the operator needs before promotion: *how
  many statements would fail if this were switched on now*.
- **Create under `OBSERVE`.** AS7's scan runs unchanged and builds the
  Bound Cabin; a group found over its bound does not fail the create —
  it is counted as `initial_over` on that group, and the create's
  success line reports how many groups were over. The CREATE is still
  refused retryably for an unsettled relation (that rule is about the
  scan's soundness, not the bound).
- **`OBSERVE → ENFORCE` (O3).** `ALTER ASSERTION name ENFORCE` is
  refused while any group is over its bound, naming the count and the
  first group key, exactly as a fresh `CREATE ... ENFORCE` would refuse on
  the same data. The operator repairs the data or lowers the bound and
  retries. There is no state in which `mode=ENFORCE` and a group is over;
  that is the invariant §7's first row protects. `ENFORCE → OBSERVE` is
  always admitted. Both are catalog writes under the same latch and
  plan-cache invalidation as `DROP ASSERTION` (§8.1a's cutover applies
  unchanged).
- **Surface.** `SHOW ASSERTIONS` gains `mode=` beside the existing
  `enforcing=` (which keeps its meaning: structure live), a per-assertion
  `would_refuse=` total and `groups_over=`; a per-group view is O6.
  `ANALYZE`'s `Assertion` line gains `would_refuse=`.

The sketch:

```sql
CREATE ASSERTION purchase_limit
  CHECK (COUNT(*) <= 5 GROUP BY user_id, product_id) ON purchases
  OBSERVE;
-- ... traffic; SHOW ASSERTIONS reports mode=OBSERVE would_refuse=17 groups_over=3
ALTER ASSERTION purchase_limit ENFORCE;
-- refused: 3 groups over bound 5; first (user_id=41, product_id=7) at 8
```

## 4. Where the record goes

`[design]` Three destinations, in order of usefulness, and the log is
the last of them.

1. **The counter, in the Bound Cabin's group header.** `would_refuse` and
   `last_would_refuse_lsn` per group, `initial_over` from the create. This
   is the durable, queryable record; it survives what the log survives
   (the Bound Cabin is logged) and is what `SHOW ASSERTIONS` reads.
2. **The client, on the statement that violated.** A notice on the reply
   naming the assertion, the group key, the aggregate and the bound — the
   same text `ENFORCE`'s error carries, in a frame that is not an error.
   For an agent this is the useful one: it learns *this write would be
   refused under enforcement* at the moment it makes the write. `S_NOTICE`
   is the frame the protocol already reserves for this (§2); whether it
   is used is O5.
3. **The server log, at `kWarn`.** The operator's stated destination.
   One line at the first would-be refusal of a group; thereafter a
   throttled summary per assertion (count since last line), not a line
   per event, because a breached hot group at OLTP rate is a log at OLTP
   rate. The throttle interval is a constant with no measured value and
   is marked `[provisional]` on the day it is set (Guideline 2); its first
   consumer is the operator reading the log, which is a confirmed one.

## 5. Transaction semantics

`[design]` An admitted violation is an ordinary write. It reserves, it
commits or aborts with its transaction, it is visible under the ordinary
view. Nothing about the transaction is poisoned, marked or delayed. The
counter increment rides the reservation's WAL record (§7 of the
assertion spec already logs the group-header delta; the counter is two
more words in that record) so that a would-be refusal counted by a
transaction that then aborts is *not* uncounted — an abort is a fact
about the transaction, not about whether the write would have been
refused. `would_refuse` counts admissions, not commits, and the surface
says so.

## 6. Generalisation — the same dial on every declared invariant

`[design]` The assertion arm is first because its check is one branch
at one step (§2). The mode is not assertion's; it belongs to the
declaration of any invariant whose enforcement is a refusal at one
step:

| invariant | refusal today or in its note | under `OBSERVE` |
|---|---|---|
| Group-bound assertion | `AssertionViolation` | this note |
| Foreign key (`FkViolation`) | child write refused, parent delete refused | admitted; a dangling reference counted per would-be refusal — which is also CN-2's *loose FK* in another spelling, and the two notes should be read together before either opens |
| Transition constraint (CN-4) | an undeclared `old => new` refused | admitted and counted; the count is the list of edges the declaration is missing |
| Unique index (IX11, not built) | would be a refusal | admitted, duplicates counted |

The README's "Invariants live in the engine" table gets one sentence:
*guaranteed* means `ENFORCE`. A row in `OBSERVE` is measured, not
guaranteed.

## 7. Known consequences

`[design]`

- **`OBSERVE` costs what `ENFORCE` costs.** The Bound Cabin, its pinned
  pages, its WAL records and the O(1) admission check are all still
  paid. The mode saves one refusal. Any reading of "weak" as "cheap" is
  wrong and the manual must say so.
- **The bound stops bounding the aggregate.** Under `OBSERVE` a group's
  running aggregate can exceed the declared bound without limit. Every
  consumer of the group header that assumed `aggregate ≤ bound` must be
  read before the arm is built; CLA found none by the spec, and the
  source read is the stage's first item.
- **The failure mode is belief, not correctness.** No query result
  changes in either mode; the risk is an operator, or a README reader,
  taking an `OBSERVE` assertion for a guarantee. Mitigations are
  structural: `mode=` is the first field after the name in
  `SHOW ASSERTIONS`, the create's success line states the mode, and
  `ALTER ... ENFORCE` cannot succeed over a breach.
- **Two vocabularies must not merge.** `NOT VALID` (reserved, AS7) means
  *old rows unchecked, new writes enforced*; `OBSERVE` means *all checked,
  none refused*. A later `NOT VALID` is a third state orthogonal to the
  mode and this note does not decide it.
- **Hot-group log volume** is the one operational hazard and §4's
  throttle is its answer; the counter, not the log, is the record.

## 8. Operator decisions this note leaves open

Each carries CLA's proposal, none ratified.

| # | question | CLA's proposal |
|---|---|---|
| O1 | Name | **`ENFORCE \| OBSERVE`**, not "weak"/"soft": the check, the structure and the cost are identical, only the refusal differs, and a word that suggests weaker consistency would be false. Two values now, room for a third |
| O2 | Count unit | **One per would-be refusal** (an admission `ENFORCE` would have refused), not per write into a breached group |
| O3 | `OBSERVE → ENFORCE` with groups over | **Refuse the `ALTER`**, naming the count and the first group, as `CREATE ... ENFORCE` refuses today. `ENFORCE NOVALIDATE` (keep the breach, refuse further growth) is a separate spelling for a separate day |
| O4 | `enforcing=` on `SHOW ASSERTIONS` | **Keep it with today's meaning** (structure live) and add `mode=` beside it; renaming a shipped field costs more than the ambiguity |
| O5 | The client notice | **Emit an `S_NOTICE`** on the violating statement; it is the frame the protocol says a warning is, and this would be its first emitter. `protocol.md` owns the payload |
| O6 | Per-group violation view | **Deferred.** `SHOW ASSERTIONS` totals in v1; a `SHOW ASSERTION name GROUPS` listing the over groups when a consumer asks |
| O7 | Log throttle | **One line at a group's first would-be refusal, then one summary line per assertion per interval**; the interval `[provisional]` until read against a log at OLTP rate |
| O8 | Which arms after assertion | **FK next**, read together with CN-2; transition with CN-4 when it opens; unique when it exists |
| O9 | Measurement gate | **None before the assertion arm** — the check path is unchanged and the counter is two words on a record already emitted. The arm's row states the cost as "not measured" until an AS-E cell includes an `OBSERVE` assertion under a write mix |
