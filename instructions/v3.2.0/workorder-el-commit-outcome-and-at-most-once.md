# Work order EL - a retry that cannot run twice: CN-5's token proved first as a row the client writes, and the engine told the truth where it does not know an outcome

Status: **WRITTEN 2026-11-28, not opened.** Written on worktree
`service-txn-orders` (branch `worktree-service-txn-orders`) from
`72580aa04` (`v3.5.1-6-g72580aa04`) on the operator's word of 2026-11-28
(`raft-marks-2026-11-28.md` §6, §7). **A proof of concept**: EL-S0 proves
or refutes, with cells and no engine change, that the answers CN-5 asks of
a token are reachable today; what the engine itself gains is gated on that
result. No stage is started and EL-S0 waits for the operator's word and
for the marks on §4's items. **CN-5 is not taken up by this writing**: the
note is marked when EL is opened (EL-Q0). Every `[source-read]` claim with
a `path:line` was read by CLA at `72580aa04` in the session that wrote
this order.

Tags: `[operator]`, `[source-read]`, `[measured]`, `[design]`,
`[quiet-wrong]`, as in DT's order.

Related: `docs/conceptnotes/cn5-commit-outcome-and-idempotency.md` (CN-5,
written against `7f35688`); `docs/spec/txn.md` §6; `docs/spec/protocol.md`
§7, §10, §11; `docs/spec/client-manual.md`; `docs/spec/index.md` §10.2;
`docs/spec/heap-and-tuple.md` §4.1; `docs/spec/parser-v2.md` I20;
`tests/bs/history.hpp`; EI, EJ and EK, written the same day.

## 0. What EL is

`[operator]` The operator's premise of 2026-11-28 and the word that asked
for this order are EI's §0. C8 is the survey's eighth constraint: **a
write runs to completion whatever happens to its connection, nothing in
the engine produces `UNKNOWN_OUTCOME`, and the engine issues tuple keys -
so a client that retries after a timeout can insert a second row.**

`[design]` A service calls another over a network that loses replies, and
its only safe reaction to a lost reply is to send the request again. That
makes "this work, at most once, and tell me what happened the first time"
the one guarantee every writer in such a system asks of its database on
every request. CN-5 designed an engine mechanism for it in September -
a client token claimed on arrival and carried by the commit record - with
ten decisions open.

`[source-read]` **Since CN-5 was written the engine has gained the parts
its mechanism was to supply** (§1.2): a unique index whose check waits for
an undecided writer of the same value, a named tuple key that waits
likewise, `ON CONFLICT`, a bound on a transaction's life, and a cancel.
EL's hypothesis is that **a token row the client writes in the
transaction that does the work already gives CN-5's answers**, and that
what is left for the engine is narrower than the note: the row's
retention, the round trips, and the two places the engine reports an
outcome it does not know.

## 1. The tree as read `[source-read]`

### 1.1 Where an outcome is unknown today

- **To the client, after a drop.** "A connection that drops mid-statement
  may find the statement applied. That is the contract, not an edge
  case": a write already running runs to completion
  (`docs/spec/client-manual.md:322-326`). `UNKNOWN_OUTCOME` "means the
  statement may have run, and must not be retried", and "nothing in this
  engine produces it"; "this engine issues tuple keys, so a retry would
  insert a second row" (`docs/spec/client-manual.md:315-321`).
- **The code has no producer.** `kUnknownOutcome` is declared, mapped and
  decoded (`include/kds/base/status.hpp:123`, `:233-234`;
  `src/server/command_dispatcher.cpp:292`); its comment says its only
  producer, statement shipping, retired at AT-S6
  (`include/kds/base/status.hpp:104-107`).
- **To the engine, in one place it says so.** "The truthful code for a
  strict commit whose sync failed is `UnknownOutcome` rather than the
  device's own error, and it is not what the engine returns": a crash
  inside the unwind can replay as a winner a transaction whose client was
  told it had failed (`docs/spec/txn.md:1391-1402`).
- **The retry bit is the contract**: "`retryable` is authoritative client
  guidance ... financial client libraries build retry loops on this bit"
  (`docs/spec/protocol.md:155`). A reply that never arrived carries no
  bit.

### 1.2 CN-5 §2 re-read at `72580aa04`

| CN-5 §2 or §7, at `7f35688` | at `72580aa04` |
|---|---|
| "There is no cancellation in this engine", `C_CANCEL` has no handler | `C_CANCEL` is answered where a listener has an identity source, and ends a walking `SELECT` or a walked write at its pause (`docs/spec/protocol.md:138`); a write by one key still runs to completion |
| "There is no idle-session timeout"; the lifetime ceiling "is not enforced today", so a dead socket holds a token indefinitely | `idle_session_timeout_s` closes an idle session where set (`docs/spec/protocol.md:140`), and an explicit transaction is rolled back between 1x and 1.25x `txn_lifetime_ceiling_ms` whatever its socket does (`docs/spec/txn.md:20-39`) |
| The lock family's units are "relation, range, slice and tuple" | the slice unit is deleted and `kIndexKey` names one value of one unique index (`docs/spec/txn.md:830-837`) |
| A named tuple key already in use "is refused as a duplicate" | and one meeting an **undecided** insert of the same key waits for its decide, placed if that rolls back (`docs/spec/heap-and-tuple.md:212`) |
| (not in the note) | a unique index's check finds a row whose latest writer is undecided **busy**, and "a statement that has written nothing waits for the holder's decide and runs again" (`docs/spec/index.md:1042-1044`, `:1062-1064`) |
| (not in the note) | `ON CONFLICT ... DO NOTHING` and `DO UPDATE` arbitrate by the tuple key or a unique index's columns (`docs/spec/parser-v2.md:259-260`; `CLAUDE.md`, Primary keys) |
| The read view cannot answer "did trx N commit?" | unchanged, as far as this session read |

## 2. The mechanism `[design]`

### 2.1 The recipe S0 proves or refutes

One relation the client owns, `tokens(token <unique>, state, result)`,
and three statements:

- **The work.** `BEGIN`; `INSERT INTO tokens` the request's token;
  the request's writes; `COMMIT`. The token row commits with the work or
  not at all (one transaction, `txn.md` §6).
- **The resend**, same token. Its `INSERT` of the token meets the unique
  check: the first attempt **committed** - a duplicate, refused, and the
  client reads the stored result; **undecided** - busy, the statement has
  written nothing, so it waits for the decide and runs again (§1.2);
  **rolled back** - no row, the resend runs. That is CN-5 §3's claim
  table for an arrival, row by row.
- **The lookup that fences**, CN-5's burn: an autocommit `INSERT` of the
  token with state `burned`. Committed first attempt - refused, the
  answer is `COMMITTED`; undecided - waits; absent - **the burn commits,
  and the late original's own token insert is then the duplicate**, so
  `NOT_COMMITTED` is final.

**What S0 must show, and what would refute it**: zero double
applications and zero lost ones over histories in which the reply to
`COMMIT` is dropped, the original is still in flight when the resend or
the burn arrives, and the instance crashes at every op - on one core and
two. A resend refused `TxnConflict` at the fault net, where CN-5 asks it
to wait, is a finding and not a refutation; a resend that runs beside a
committed original is a refutation.

### 2.2 What the recipe does not give, as read before S0

1. **Autocommit.** A single statement has no second statement to carry
   its token, so the client pays `BEGIN` and `COMMIT` round trips it did
   not pay before. S0 measures them.
2. **Retention.** A token row is a row: it lives until the client deletes
   it, and a deleted row's key stays bound until a `PURGE`
   (`heap-and-tuple.md` §4.1c). CN-5's "expired is never mistaken for
   never seen" is the client's to keep, by the timestamp in its token.
3. **The fingerprint.** Nothing compares the resend's request with the
   original's unless the client stores one in the row.
4. **The engine's own unknowns** (§1.1's third item): no client recipe
   makes a reply truthful.

### 2.3 What the engine would gain, each behind S0

- **E1 - the truthful code.** A `strict` commit whose sync failed answers
  `UNKNOWN_OUTCOME`, as `txn.md` §6 says it should; the code gets the one
  producer that is true.
- **E2 - CN-5's token in the engine**: the claim map, the token on
  `TXN_COMMIT`, the lookup frame. Opened only if S0 shows the recipe
  wrong, or its cost (item 1) or its gaps (2, 3) above what the operator
  accepts.
- **E3 - the recipe as a documented contract**: a section of the manual,
  with the cells of S0 as the contract's tests.

## 3. Rules

- **EL-R1. A token names at most one committed run of its work**
  `[quiet-wrong]`: a resend that runs beside a committed original is
  money moved twice.
- **EL-R2. `NOT_COMMITTED` is final** `[quiet-wrong]`: an answer that the
  work did not commit, after which the original commits, is the same
  defect from the other side. A lookup that cannot fence answers "still
  in flight".
- **EL-R3. The token and the work are one transaction's**: neither is
  durable without the other. No stage stores a token outside the
  transaction it names, in memory or in a second transaction.
- **EL-R4. `UNKNOWN_OUTCOME` is produced only where the engine does not
  know, and never where it does**: a reply that says "may have run" for
  work that plainly did not is the lie the fault net's comment already
  refuses (`src/server/command_dispatcher.cpp:621-625`), and it is never
  `retryable = 1`.
- **EL-R5. Nothing waits on a token longer than the lock family lets a
  writer wait**: the fault net and the lifetime ceiling bound it, as they
  bound a row.
- **EL-R6. No frame, capability bit, status code or record field is
  added before EL-Q2 is marked**, and CN-5's ten items are the
  operator's, not this order's.
- The standing rules, as DT's order lists them.

## 4. Items for the operator (EL-Q), CLA's proposals

| | item | options | CLA proposes |
|---|---|---|---|
| EL-Q0 | Is EL opened, and how far? | (a) S0 and S1, S2 behind EL-Q2; CN-5 marked as taken up; (b) S0 alone; (c) not opened | (a) |
| EL-Q1 | Where S0's cells live | (a) the simulation harness for the crash and drop histories, with the history checker's indeterminate outcome as the oracle's input, and the two-core rig for the in-flight waits; (b) the rig alone | (a) |
| EL-Q2 | When does the engine's token (E2) open? | (a) only if S0 refutes the recipe, or measures its added round trips above the larger spread of an autocommit write at `group` **and** the operator names that cost unacceptable; otherwise EL closes with E1 and E3. (b) regardless: CN-5's mechanism is the target and the recipe its interim | (a) |
| EL-Q3 | The truthful code (E1) | (a) built in S1: a `strict` commit whose sync failed answers `UNKNOWN_OUTCOME`; (b) left, and `known-gaps.md` carries `txn.md` §6's paragraph by name | (a) |
| EL-Q4 | The token's column in the recipe | (a) the client's choice, and S0 runs two: a `char(16)` primary key (a hashed key, one descent) and a named `int64` tuple key (48 bits, CN-5's "TigerBeetle's shape"); (b) one fixed form | (a) |
| EL-Q5 | A resend that meets the fault net | (a) left as the lock family's refusal, `TxnConflict` retryable, and documented: the resend is sent again; (b) a longer wait for a token than for a row | (a) |
| EL-Q6 | The client library | (a) `tools/`' reference client gains nothing in EL: the recipe is three statements; (b) a helper that wraps a call in the recipe | (a) |

## 5. Stages

| stage | what | ends in |
|---|---|---|
| **EL-S0** | The proof: §2.1's three statements as cells - the reply to `COMMIT` dropped, the resend and the burn each arriving before, during and after the original's decide, a crash at every op - on both token forms of EL-Q4; EL-R1 and EL-R2 as the oracle. Beside it, measured in `build-release` inside the stage budget: an autocommit write against the same write inside the recipe, one client, `group` | the recipe proved or refuted, `[measured]`; §2.2's list confirmed or corrected; EL-Q2 read |
| **EL-S1** | E1 (EL-Q3 (a)): the failed-sync commit's code, with a cell under an injected sync failure; the stale comments CN-5 §2 names, corrected | the suite; `txn.md` §6, `client-manual.md` |
| **EL-S2** | (gated on EL-Q2) E2: CN-5's mechanism, its ten items marked first | its own stage plan, written when it opens |
| **EL-S3** | The close: the manual's section (E3) with S0's cells named as its tests; `known-gaps.md`; CN-5's header says what EL took and what it left; the overhead measurement if S1 or S2 changed a statement's path | the operator's word on landing |

## 6. What EL does not do

- No retry inside the server, and no retry policy for a client.
- No lookup by transaction id (CN-5 O6).
- Nothing across instances: a token names work on this instance. A saga
  step's at-most-once is EL's; the saga is the client's (EI).
- No token on a `relaxed` commit's promise beyond what `relaxed` gives:
  the recipe's row is lost with its work inside the loss window, and the
  resend then runs it once (CN-5 §5).
- No change to the K1 rule for a deleted token's key.

## 7. Stage log (CLA, appended as stages land)

### Written 2026-11-28

On `service-txn-orders` from `72580aa04`, on the operator's word. No stage
is opened and no code is written. §2.1's recipe is `[design]`: it is read
off `index.md` §10.2 and `heap-and-tuple.md` §4.1 and has not been run.
