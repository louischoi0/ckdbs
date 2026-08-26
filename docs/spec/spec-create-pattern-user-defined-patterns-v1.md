# CREATE PATTERN — user-declared patterns (spec, v1)

Status: **DECIDED — steps 1-4 of §8 built (2026-08-02).** The declaration
grammar, the catalog, the validation chain and the introspection surface
all run; step 5 (the recorder/retention hooks) waits on subsystems that do
not exist. Two things landed differently from the text below and the
sections say so where it matters: `sys.pattern_defs` gained a `param_count`
column and stores the **whole `CREATE PATTERN` statement** as its
`source_text` (§4.2), and the token for `$name` is `kNamedParam` because
`kParam` was already taken by `?` (§3.1).
Depends on: fingerprint (P01–P02, done), sys.patterns catalog (done),
waystone directory + page format (done), trail recorder/replayer (not built).
Related docs: waystone-concpets.md, parser blueprint, V0 fixed-width tuple rule.

---

## 1. Purpose and model

One mechanism, two entry points. A *user-declared pattern* is the same
object as an auto-registered one — the same `sys.patterns` row, the same
waystone directory, the same trail format, the same replay contract. What
declaration changes is **provenance and lifecycle policy**, never the trust
model: replay rules and validation are identical regardless of origin, for
the same reason the engine keeps one evaluator and one step-kind table.

What a declaration buys:

1. **Cold-start elimination.** An operator declares the known hot patterns
   at provisioning; the engine starts warm instead of learning from
   traffic. (This is the appliance story: patterns ship with the box.)
2. **Recording from the first execution.** The n=2 policy (J5) infers
   "this pattern repeats" from a second sighting; a declaration *is* that
   evidence, so user patterns skip the probation.
3. **Survival across fingerprint version bumps.** The declaration stores
   its source text, so a version bump re-fingerprints and re-registers it
   at boot. Auto patterns cannot do this — they hold only a hash — and are
   retired instead.
4. **Pinning.** A pinned pattern is exempt from waystone retention.
5. **Typed parameters.** Declared types let CREATE catch implicit
   conversions in the body before any traffic runs (§6, check 6) —
   feedback auto-registration structurally cannot give, since it only
   ever sees statements that already executed.

Auto-registration (when it lands) continues to widen coverage silently
underneath. Externally, the declared feature is the headline; autonomy is
implied, per the README positioning rule.

---

## 2. Syntax

```
CREATE PATTERN <name> ( $p1 <type> [, $p2 <type> ...] )
    [ WITH ( <option> = <value> [, ...] ) ]
    OF <body>

DROP PATTERN <name>
```

Example:

```sql
CREATE PATTERN acct_trades($flag bool, $name varchar)
  WITH (pinned = on, expected_instances = 100000)
  OF SELECT id, name
     FROM account AS a JOIN trade AS t ON t.id = a.id
     WHERE a.flag = $flag AND a.name = $name;
```

Grammar notes, in the order the parser meets them:

- `<name>`: ordinary identifier, ASCII-folded like every other identifier.
  Unique across patterns (§6, check 8).
- **Parameter list**: one or more `$`-sigiled names, **each with a
  mandatory type annotation** (`$flag bool`). An untyped parameter is a
  parse error — max-validation policy: inference from first use was
  considered and rejected, because it makes the declared contract depend
  on body order and gives the type checker (§6, check 6) nothing stable
  to check against. The type name must resolve in the engine's type set
  (check 3). The sigil is
  **mandatory in both the declaration list and the body.** A bare `a` in
  the body is always a column or alias; a `$a` is always a parameter.
  This is the whole reason for the sigil: without it, a parameter named
  `a` and an alias `AS a` collide (identifiers are case-folded), and
  "value position" cannot disambiguate because join predicates put columns
  in value position too (`ON t.id = a.id`). The sigil removes the
  ambiguity at the token level, so no collision checks against aliases or
  column names are needed at all.
- **`WITH` before `OF`, body last.** The body is a complete statement; if
  options followed it, the parser would have to find where a SELECT ends.
  As a suffix after `OF`, the body runs to end of statement — no boundary
  problem. (Same trick as the ANALYZE prefix: wrap around an intact
  statement, never inside one.)
- `<body>`: v1 accepts **SELECT-class statements only** — anything
  `exec::Compile` maps to kSingleSelect / kJoinSelect. INSERT/UPDATE
  patterns are deferred (§9). The body must contain at least one `$param`
  occurrence per declared parameter (§6, check 4).

An empty parameter list `()` is legal: such a pattern has exactly one
instance (the arg_hash of an empty argument stream).

---

## 3. Lexer and fingerprint changes

### 3.1 New token: `kNamedParam`

The lexer gains a token kind for `$` followed by an identifier
(`$` + `[A-Za-z_][A-Za-z0-9_]*`, compared case-insensitively like every
other identifier). Outside a CREATE PATTERN body it is a parse error in
v1 — the token is *reserved* for the extended protocol's named binds
(D4), which this deliberately aligns with, but nothing wires it yet.

**Named `kNamedParam`, not `kParam`**: `TokenType::kParam` already exists
and is `?`. They stay separate types because they disagree about the
grammar (`?` is refused everywhere, `$x` is accepted in a declared body)
while agreeing about the fingerprint (§3.2 folds both to `kValue`) — one
type for both would collapse a grammar distinction to buy a hash
distinction that does not exist. A bare `$` with no identifier after it
is still a lexing error; there is no anonymous named parameter.

### 3.2 ShapeTag mapping — the load-bearing line

In the fingerprint's shape stream, `kParam` folds to **`ShapeTag::kValue`**
— the existing convergence point where int literals, string literals, and
`?` binds already meet. This single mapping is what makes the feature
work: the fingerprint of

```
... WHERE a.flag = $flag      (declared body)
... WHERE a.flag = 42          (live inline traffic)
... WHERE a.flag = ?           (live bound traffic)
```

is the **same pattern_id**. Without it, a declared pattern would never
match anything (an identifier hashes as `kIdent`, not `kValue`) and the
feature would be silently dead.

The parameter's *name* contributes nothing to the hash — names exist for
the declaration's readability and for future named binds only.

**Neither does the declared *type*.** A `$flag bool` and a `$flag int`
body hash identically; the type annotation exists for CREATE-time
checking (§6, check 6) and never enters the shape stream. This is
deliberate: live traffic carries no declaration, so anything the type
contributed to `pattern_id` would break the declared/live convergence
that §3.2 exists to guarantee.

### 3.3 Arity and arg_hash

`arg_hash` remains what it is today: a hash of the executed statement's
literal/bind stream, in statement order, type-tagged (kInt/kStr). The
declaration does not produce arg_hashes — instances still arise only from
traffic. The declaration's parameter list defines:

- **arity** = the number of `kValue` slots in the body. Each *occurrence*
  of a `$param` is one slot; a parameter used twice contributes two slots.
- A documented consequence: the fingerprint machinery cannot enforce that
  two occurrences of `$flag` carry the *same value* at match time. A live
  statement `WHERE x = 1 AND y = 2` matches a body written
  `WHERE x = $f AND y = $f`. Repeated use is therefore allowed but is a
  readability device, not a constraint. (Recorded in the doc so nobody
  later mistakes it for a bug.)
- Declared types interact with the arg stream's *type tags* (kInt/kStr)
  in one way worth naming: a live statement whose literal type differs
  from the declared one (`= '1'` against a `$flag bool`) still matches
  the pattern (types are outside the shape hash, above) but hashes to a
  **different instance** than `= 1` would, and pays a conversion on every
  execution. CREATE-time checking cannot see traffic; this is exactly
  the blind spot ANALYZE's pattern display is positioned to surface
  later. Runtime rejection of mistyped arguments is *not* part of v1
  (§9).

---

## 4. Catalog changes

No backward compatibility is owed (pre-release); the bootstrap format
changes in place, existing data directories are re-initialized.

### 4.1 `SysPatternRow` — two additions

```
origin      u8    kOriginAuto = 0, kOriginUser = 1
flags       u16   bit 0: kPatternPinned      (existing reserved field)
```

`origin` says who created the row; `kPatternPinned` says what retention
may do to its waystones. They are separate on purpose: an auto pattern
could later be pinned by an operator without re-declaring it, and a user
pattern can be created unpinned.

Defaults: `CREATE PATTERN` writes `origin = kOriginUser` and
`pinned = on` unless the option says otherwise — declaring a pattern and
then letting retention silently evict it would defeat the declaration.
Auto registration (when wired) writes `kOriginAuto`, unpinned.

### 4.2 New system relation: `sys.pattern_defs`

`SysPatternRow` is fixed-width and stays that way; names and source text
go to a sibling relation:

```
sys.pattern_defs
  id            int64    Keystone pk (invariant 11 — see below)
  pattern_id    uint64   (join key to sys.patterns; unique)
  param_count   int32    materialized arity (§3.3's value-slot count)
  name          varchar  (unique, case-folded)
  source_text   varchar  (the whole CREATE PATTERN statement, verbatim)
```

Storage rides the V0 rule as-is: tagged cells, var-heap spill for text
over the inline width. Nothing new is invented for it. `SHOW PATTERNS`
joins this relation to print names instead of bare hex ids; auto patterns
have no row here and keep printing as hex.

Four things the built version settles that the sketch above left open.

**A Keystone pk was added.** Every relation's first column is its
system-generated pk (invariant 11), and this is a relation stored in
ordinary user tuple format, so it has one. Not a design change — a
consequence the three-column list had simply not spelled out.

**`source_text` is the whole statement, not the body.** §7 re-registers a
declared pattern from this text after a fingerprint version bump, and
that has to restore the declared *types* and the `WITH` options too —
neither of which is recoverable from the body alone. It is also why there
is no sibling relation for the parameters: the canon already carries
them, and a second copy is a second thing that can drift. What gets
*fingerprinted* is still the body alone (§3.2); the parser keeps both
slices.

**`param_count` is stored, not derived.** Recomputing it means
re-fingerprinting `source_text`, and the two could then disagree for a
row an older build wrote.

**This is the first catalog relation in user tuple format.** Every other
one is a fixed-offset typed row codec (`catalog/rows.hpp`), and
`exec/catalog_view.hpp` says why that is deliberate. Storing arbitrary
text is what forces the exception: the fixed-length rule already answers
"where do long values go", and inventing a second answer for one catalog
row would be inventing a second var-heap protocol. The price is that its
rows cannot be read from `catalog/` — decoding them needs the row codec,
which sits above the catalog — so the readers live in
`stats/pattern_defs.hpp`. Two rules that module owns: **decode before
descending** (I15's R1 — the scan stages rows inside the walk and
resolves spilled cells only after every page span is released, which is
why it cannot stop early on a name match), and **deletion is physical**
(`RetireSlot`, not `DeleteMark` — catalog reads have no snapshot to
filter a mark against, so a marked row would still be found by name).

### 4.3 `dir_depth` at creation

`expected_instances = N` maps to the directory depth at creation time:

```
dir_depth = clamp( ceil( log2048(N) ), 1, 6 )
```

| expected_instances        | dir_depth |
|---------------------------|-----------|
| ≤ 2,048                   | 1         |
| ≤ ~4.19 M   (2048²)       | 2         |
| ≤ ~8.6 G    (2048³)       | 3         |
| … up to 2048⁶             | 4–6       |

Rationale: directory growth is a cache flush (deepening strands 2047/2048
of existing mappings), so pre-sizing is the *mitigation*, not a
convenience. The option deliberately exposes an instance count, not a
depth — the operator should not need to know the 2048 fanout, and
"hash_table_size" would wrongly suggest arbitrary granularity when the
real knob is an integer in [1, 6].

Default when the option is absent: `dir_depth = 1`, same as an auto
pattern would get.

---

## 5. Options

Recognized keys, all validated (§6, check 10); an unknown key is
`InvalidArgument`, not ignored — max-validation policy.

| key                  | type    | default | effect                                   |
|----------------------|---------|---------|------------------------------------------|
| `pinned`             | on/off  | `on`    | sets/clears `kPatternPinned`             |
| `expected_instances` | integer | —       | initial `dir_depth` per §4.3             |

`pinned` interacts with the (future) waystone page budget: pinned
patterns are excluded from the global eviction budget — either exempted
outright or charged to a separate pinned budget; that choice belongs to
the retention spec (P15) and is intentionally not made here.

---

## 6. Validation at CREATE — the full list

Declarative registration's payoff is early feedback; every check below
runs at CREATE time, in this order, first failure wins. Errors are
`InvalidArgument` with a message naming the check; the one warning is
carried in the success response (the one-line protocol has no side
channel).

1. **Body parses.** Full parse of the `OF` suffix with `kParam` accepted
   in value positions.
2. **Every `$ident` in the body is declared.** An undeclared `$x` is an
   error, not an implicit parameter.
3. **Parameter list is well-formed.** Names valid, unique after folding;
   every parameter carries a type annotation, and each type name
   resolves in the engine's type set (`sys.types`) — an unknown type is
   an error, not a deferred lookup. The empty list `()` is legal.
4. **Every declared parameter is used at least once.** An unused
   parameter silently changes nothing today but would desynchronize the
   declared arity from the body's value-slot count — reject.
5. **Body compiles.** `exec::Compile` must return a StepChain against the
   current catalog; unknown relations/columns and Unsupported shapes (J2)
   fail here with the compiler's own message.
6. **Implicit-conversion analysis → warning or error.** With the chain
   compiled, every `$param` occurrence has a *context type*: the catalog
   type of the column on the other side of its predicate (the lhs column
   of an equality, the subject column of `IN`/`BETWEEN`). Each
   occurrence is checked against the parameter's declared type:
   - **exact match** → clean.
   - **coercible mismatch** → **warning** in the success response, one
     line per offending occurrence (`$flag bool vs account.flag int at
     step 0: implicit conversion on every execution`). The declaration
     succeeds — a conversion is a per-execution cost and a likely
     mistake, not an invalid pattern.
   - **incoercible mismatch** → **error**; the comparison could never
     evaluate, so the pattern could never match its own intent.
   The coercibility matrix is the engine's expression-typing rule, not
   this spec's. **Ratified 2026-08-02** at the v1 baseline that was
   proposed here: numeric↔numeric and bool↔int coerce (warn),
   string↔numeric does not (error). Implemented in
   `src/exec/pattern_ddl.cpp` as three *families* — numeric, bool, text —
   rather than a pairwise table, because the rule is a statement about
   families and a table would have to answer `int8` vs `uint64` and
   `varchar` vs `char` separately, each entry a chance to disagree with
   the sentence above. Concretely: identical type → clean; text on
   exactly one side → error; anything else → warn. A parameter used in
   several predicates is checked at every occurrence — the declared type
   is the single contract all of them must satisfy.
7. **Statement class is patternable.** v1: kSingleSelect / kJoinSelect
   only.
8. **Replayability check → warning, not error.** If the chain contains no
   kLookup/kProbe step (scan-only), the pattern is legal but its trail
   can never replay — only (future) prefetch. The success response says
   so: declaring it is allowed, being surprised later is not.
9. **Name is unique** across `sys.pattern_defs` (case-folded).
10. **pattern_id reconciliation.** Compute the fingerprint of the body
   (with `$params` as kValue) and look it up:
   - not present → fresh row, `origin = kOriginUser`.
   - present with `origin = kOriginAuto` → **adopt**: upgrade the row in
     place (origin, pinned per options), attach the `pattern_defs` row.
     The existing `waystone_root` and any recorded trails are *kept* —
     adoption must not throw away a warm cache. `dir_depth` is not
     changed by adoption (regrowing would flush; if the operator wants a
     deeper directory they can DROP and re-CREATE).
   - present with `origin = kOriginUser` → error: duplicate declaration
     (possibly under a different name — the message includes the existing
     name).
11. **Options validated**: known keys only; `pinned` ∈ {on, off};
    `expected_instances` ∈ [1, 2048⁶].
12. **Fingerprint version stamped** on the row, as with any registration.

On success the response returns the `pattern_id` (hex) and the effective
`dir_depth` — the id is what ANALYZE will print for matching statements,
which makes "I declared it, why doesn't traffic match" debuggable by
direct comparison.

---

## 7. Runtime semantics

- **Matching costs nothing extra.** Live traffic computes its pattern_id
  exactly as before; declared and auto rows are found by the same lookup.
  There is no "declared pattern matcher" — §3.2 already made the hashes
  converge.
- **Trail recording**: `origin = kOriginUser` ⇒ record from the first
  execution of an instance. `kOriginAuto` ⇒ n=2 (J5) unchanged.
- **Retention** (when built): skips `kPatternPinned` rows' waystones.
  Invariant 8 still holds for pinned patterns — pinning is a policy
  promise, not a correctness requirement, and a manual purge remains
  legal.
- **Fingerprint version bump**: at boot, rows whose stamped version is
  stale split by origin — auto rows retire (today's behavior); user rows
  are re-fingerprinted from `source_text`, get their `pattern_id`
  updated in both relations, and keep name/origin/pinned. Their waystone
  tree is discarded (the old trails hang off the old id; invariant 8
  makes that free) and rebuilt by traffic — from first execution, since
  they are user rows.
- **DROP PATTERN name**: removes both rows; the waystone tree under
  `waystone_root` is freed (or handed to retention for lazy reclamation —
  implementation's choice, both are invariant-8-safe). If auto
  registration later re-learns the same shape, it reappears as a nameless
  auto row — DROP deletes the declaration, not the shape.

---

## 8. Implementation order

1. ~~Lexer `$param` token + fingerprint `kParam → kValue` fold~~ —
   **done.** `TokenType::kNamedParam` (§3.1) folds to `ShapeTag::kValue`.
   **`kFingerprintVersion` stayed at 1**: `$` used to lex as `kError`, so
   a statement containing one had no fingerprint at all, and making a
   previously-unfingerprintable statement fingerprintable is exactly the
   transition `fingerprint.hpp`'s bump rule permits without one — the
   same argument V04's dot token used. No stored `pattern_id` moved, and
   the corpus's existing golden hashes are the witness.
2. ~~Catalog: `SysPatternRow` origin/flags + bootstrap bump;
   `sys.pattern_defs`~~ — **done.** `flags` (u16, offset 38) precedes
   `origin` (u8, offset 40) so both keep their `offsetof` assert; the row
   grew 38 → 41 bytes. `kSuperBlockVersion` 5 → 6, so every pre-existing
   data file stops mounting — twice over, since bootstrap also gained a
   seventh relation on a fixed page id.
3. ~~`CREATE PATTERN` / `DROP PATTERN` dispatch + §6 validation chain~~ —
   **done** (`include/kds/exec/pattern_ddl.hpp`).
4. ~~`SHOW PATTERNS` join with names; CREATE response with pattern_id~~ —
   **done**, and `ANALYZE` now prints the statement's `pattern_id` too,
   which is what actually closes the loop below.
5. Hooks consumed later by the trail recorder (first-execution recording
   for user origin) and retention (pinned) when those land. **Not built**
   — `PatternAccess` carries `origin` and `flags` so the recorder can
   read them off the cached entry without a page read per execution.

Steps 1–4 are useful before any trail recording exists: declaration,
introspection, and the ANALYZE pattern-id display already close the
"did my declaration match" loop. Verified end to end — a declaration
prints `pattern_id=0x4dc97b591917e01`, and `ANALYZE` of the matching
inline statement prints the same number.

One thing built beyond the letter of the list, because §6 check 8 needed
it: a `$param` in pk-equality position compiles to `kLookup`. Without
that the compiler's `kInt`-only pk test would make every declared point
lookup a `kScan`, and check 8 would warn "can never replay" about
precisely the shape declaring a pattern exists to make replayable.

---

## 9. Out of scope / open

- Trail recorder and replayer themselves (waystone workplan).
- Retention / page budget and the pinned-budget question (P15).
- Directory collision policy — still `[OPEN]` in the waystone spec;
  nothing here constrains it.
- INSERT / UPDATE / DELETE patterns (fingerprint already covers them;
  gated only by validation check 7).
- Runtime enforcement of declared parameter types against live argument
  type tags (rejecting or warning on a mistyped instance at execution
  time) — v1 checks types at CREATE only; the runtime side is ANALYZE
  territory first, enforcement later if ever.
- Priming / warm execution ("run this instance now with these sample
  args") — a separate verb over a declared pattern, not part of CREATE.
- `$named` binds in the extended wire protocol (D4) — the token is
  reserved to stay compatible; wiring is future work.
