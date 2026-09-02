# CREATE PATTERN — user-declared patterns (spec, v1)

Status: **WITHDRAWN (operator). The code is removed; this file records the
design as it stood.**

A pattern is **a fingerprint-identified case tracked by statistics** — one
concept. The declaration path below duplicated it with a second model (a
name, a stored source text, a `sys.pattern_defs` row, an `origin` that
changed recording policy), and carrying two models of one thing was the
cost the operator declined to keep paying.

What holds today:

- There is no `CREATE PATTERN`, no `DROP PATTERN`, no `sys.pattern_defs`,
  and `SHOW PATTERNS` prints no name column. `sys.patterns`, the
  fingerprint, the waystone directory, the trail recorder and replay are
  untouched — declared and auto rows were always found by the same lookup
  (§7), so nothing that matches a statement to a pattern depended on the
  declaration.
- `SysPatternRow` keeps `flags` (`u16`, offset 38) and `origin` (`u8`,
  offset 40); the row is 41 bytes. Every row is `kOriginAuto` and nothing
  writes `kPatternPinned`. `SysCabinRow` carries the same `origin` concept.
- `TrailRecorder` has one threshold, `n = 2`, for every pattern.
- `TokenType::kNamedParam` (§3.1) still lexes and is accepted by no
  production: a `$name` parameter is refused everywhere.
- `kFingerprintVersion` did not move: `$name` folds to `ShapeTag::kValue`
  (§3.2), and no stored `pattern_id` or `arg_hash` changed.

Everything below describes the withdrawn design; none of it runs.

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
   traffic.
2. **Recording from the first execution.** The n=2 policy
   (`docs/spec/parser-v2.md` J5) infers "this pattern repeats" from a
   second sighting; a declaration *is* that evidence, so user patterns
   skip the probation.
3. **Survival across fingerprint version bumps.** The declaration stores
   its source text, so a version bump re-fingerprints and re-registers it
   at boot. Auto patterns cannot do this — they hold only a hash — and are
   retired instead.
4. **Pinning.** A pinned pattern is exempt from waystone retention.
5. **Typed parameters.** Declared types let CREATE catch implicit
   conversions in the body before any traffic runs (§6, check 6) —
   feedback auto-registration structurally cannot give, since it only
   ever sees statements that already executed.

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
  Unique across patterns (§6, check 9).
- **Parameter list**: one or more `$`-sigiled names, **each with a
  mandatory type annotation** (`$flag bool`). An untyped parameter is a
  parse error — inference from first use makes the declared contract
  depend on body order and gives the type checker (§6, check 6) nothing
  stable to check against. The type name must resolve in the engine's
  type set (check 3). The sigil is **mandatory in both the declaration
  list and the body.** A bare `a` in the body is always a column or
  alias; a `$a` is always a parameter. Without the sigil, a parameter
  named `a` and an alias `AS a` collide (identifiers are case-folded), and
  "value position" cannot disambiguate because join predicates put columns
  in value position too (`ON t.id = a.id`). The sigil removes the
  ambiguity at the token level, so no collision checks against aliases or
  column names are needed at all.
- **`WITH` before `OF`, body last.** The body is a complete statement; if
  options followed it, the parser would have to find where a SELECT ends.
  As a suffix after `OF`, the body runs to end of statement — no boundary
  problem. (Same trick as the ANALYZE prefix: wrap around an intact
  statement, never inside one.)
- `<body>`: **SELECT-class statements only** — anything `exec::Compile`
  maps to kSingleSelect / kJoinSelect. The body must contain at least one
  `$param` occurrence per declared parameter (§6, check 4).

An empty parameter list `()` is legal: such a pattern has exactly one
instance (the arg_hash of an empty argument stream).

---

## 3. Lexer and fingerprint changes

### 3.1 New token: `kNamedParam`

The lexer gains a token kind for `$` followed by an identifier
(`$` + `[A-Za-z_][A-Za-z0-9_]*`, compared case-insensitively like every
other identifier). Outside a CREATE PATTERN body it is a parse error.

**Named `kNamedParam`, not `kParam`**: `TokenType::kParam` already exists
and is `?`. They stay separate types because they disagree about the
grammar (`?` is refused everywhere, `$x` is accepted in a declared body)
while agreeing about the fingerprint (§3.2 folds both to `kValue`) — one
type for both would collapse a grammar distinction to buy a hash
distinction that does not exist. A bare `$` with no identifier after it
is a lexing error; there is no anonymous named parameter.

### 3.2 ShapeTag mapping — the load-bearing line

In the fingerprint's shape stream, `kNamedParam` folds to
**`ShapeTag::kValue`** — the existing convergence point where int
literals, string literals, and `?` binds already meet. This single mapping
is what makes the feature work: the fingerprint of

```
... WHERE a.flag = $flag      (declared body)
... WHERE a.flag = 42          (live inline traffic)
... WHERE a.flag = ?           (live bound traffic)
```

is the **same pattern_id**. Without it, a declared pattern would never
match anything (an identifier hashes as `kIdent`, not `kValue`) and the
feature would be silently dead.

The parameter's *name* contributes nothing to the hash — names exist for
the declaration's readability only.

**Neither does the declared *type*.** A `$flag bool` and a `$flag int`
body hash identically; the type annotation exists for CREATE-time
checking (§6, check 6) and never enters the shape stream. Live traffic
carries no declaration, so anything the type contributed to `pattern_id`
would break the declared/live convergence that §3.2 exists to guarantee.

### 3.3 Arity and arg_hash

`arg_hash` remains a hash of the executed statement's literal/bind stream,
in statement order, type-tagged (kInt/kStr). The declaration does not
produce arg_hashes — instances still arise only from traffic. The
declaration's parameter list defines:

- **arity** = the number of `kValue` slots in the body. Each *occurrence*
  of a `$param` is one slot; a parameter used twice contributes two slots.
- The fingerprint machinery cannot enforce that two occurrences of `$flag`
  carry the *same value* at match time. A live statement
  `WHERE x = 1 AND y = 2` matches a body written
  `WHERE x = $f AND y = $f`. Repeated use is therefore allowed but is a
  readability device, not a constraint.
- A live statement whose literal type differs from the declared one
  (`= '1'` against a `$flag bool`) still matches the pattern (types are
  outside the shape hash, above) but hashes to a **different instance**
  than `= 1` would, and pays a conversion on every execution. CREATE-time
  checking cannot see traffic, and there is no runtime rejection of
  mistyped arguments.

---

## 4. Catalog changes

### 4.1 `SysPatternRow` — two additions

```
origin      u8    kOriginAuto = 0, kOriginUser = 1
flags       u16   bit 0: kPatternPinned
```

`origin` says who created the row; `kPatternPinned` says what retention
may do to its waystones. They are separate on purpose: an auto pattern
could be pinned by an operator without re-declaring it, and a user
pattern can be created unpinned.

Defaults: `CREATE PATTERN` writes `origin = kOriginUser` and
`pinned = on` unless the option says otherwise — declaring a pattern and
then letting retention silently evict it would defeat the declaration.
Auto registration writes `kOriginAuto`, unpinned.

### 4.2 New system relation: `sys.pattern_defs`

`SysPatternRow` is fixed-width and stays that way; names and source text
go to a sibling relation:

```
sys.pattern_defs
  id            int64    Keystone pk (invariant 11)
  pattern_id    uint64   (join key to sys.patterns; unique)
  param_count   int32    materialized arity (§3.3's value-slot count)
  name          varchar  (unique, case-folded)
  source_text   varchar  (the whole CREATE PATTERN statement, verbatim)
```

Storage rides the fixed-length rule as-is: tagged cells, var-heap spill
for text over the inline width. `SHOW PATTERNS` joins this relation to
print names instead of bare hex ids; auto patterns have no row here and
keep printing as hex.

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

**This is a catalog relation in user tuple format.** Every other one is a
fixed-offset typed row codec (`catalog/rows.hpp`). Storing arbitrary text
is what forces the exception: the fixed-length rule already answers
"where do long values go", and inventing a second answer for one catalog
row would be inventing a second var-heap protocol. The price is that its
rows cannot be read from `catalog/` — decoding them needs the row codec,
which sits above the catalog — so the readers live above it. Two rules
that module owns: **decode before descending** (`docs/spec/parser-v2.md`
I15 — the scan stages rows inside the walk and resolves spilled cells only
after every page span is released, which is why it cannot stop early on a
name match), and **deletion is physical** (`RetireSlot`, not `DeleteMark`
— catalog reads have no snapshot to filter a mark against, so a marked
row would still be found by name).

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
pattern gets.

---

## 5. Options

Recognized keys, all validated (§6, check 11); an unknown key is
`InvalidArgument`, not ignored.

| key                  | type    | default | effect                                   |
|----------------------|---------|---------|------------------------------------------|
| `pinned`             | on/off  | `on`    | sets/clears `kPatternPinned`             |
| `expected_instances` | integer | —       | initial `dir_depth` per §4.3             |

---

## 6. Validation at CREATE — the full list

Declarative registration's payoff is early feedback; every check below
runs at CREATE time, in this order, first failure wins. Errors are
`InvalidArgument` with a message naming the check; the one warning is
carried in the success response.

1. **Body parses.** Full parse of the `OF` suffix with `kNamedParam`
   accepted in value positions.
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
   current catalog; unknown relations/columns and refused shapes
   (`docs/spec/parser-v2.md` J2) fail here with the compiler's own
   message.
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
   Coercibility is decided by three *families* — numeric, bool, text —
   rather than a pairwise table: identical type → clean; text on exactly
   one side → error; anything else → warn. A parameter used in several
   predicates is checked at every occurrence — the declared type is the
   single contract all of them must satisfy.
7. **Statement class is patternable.** kSingleSelect / kJoinSelect only.
8. **Replayability check → warning, not error.** If the chain contains no
   kLookup/kProbe step (scan-only), the pattern is legal but its trail
   can never replay. The success response says so: declaring it is
   allowed, being surprised later is not. So that a declared point lookup
   is not itself a scan, a `$param` in pk-equality position compiles to
   `kLookup`.
9. **Name is unique** across `sys.pattern_defs` (case-folded).
10. **pattern_id reconciliation.** Compute the fingerprint of the body
   (with `$params` as kValue) and look it up:
   - not present → fresh row, `origin = kOriginUser`.
   - present with `origin = kOriginAuto` → **adopt**: upgrade the row in
     place (origin, pinned per options), attach the `pattern_defs` row.
     The existing `waystone_root` and any recorded trails are *kept* —
     adoption must not throw away a warm cache. `dir_depth` is not
     changed by adoption (regrowing would flush; a deeper directory takes
     a DROP and re-CREATE).
   - present with `origin = kOriginUser` → error: duplicate declaration
     (possibly under a different name — the message includes the existing
     name).
11. **Options validated**: known keys only; `pinned` ∈ {on, off};
    `expected_instances` ∈ [1, 2048⁶].
12. **Fingerprint version stamped** on the row, as with any registration.

On success the response returns the `pattern_id` (hex) and the effective
`dir_depth` — the id is what ANALYZE prints for matching statements,
which makes "I declared it, why doesn't traffic match" debuggable by
direct comparison.

---

## 7. Runtime semantics

- **Matching costs nothing extra.** Live traffic computes its pattern_id
  exactly as before; declared and auto rows are found by the same lookup.
  There is no "declared pattern matcher" — §3.2 already made the hashes
  converge.
- **Trail recording**: `origin = kOriginUser` ⇒ record from the first
  execution of an instance. `kOriginAuto` ⇒ n=2 unchanged.
- **Retention** skips `kPatternPinned` rows' waystones. Invariant 8 still
  holds for pinned patterns — pinning is a policy promise, not a
  correctness requirement, and a manual purge remains legal.
- **Fingerprint version bump**: at boot, rows whose stamped version is
  stale split by origin — auto rows retire; user rows are
  re-fingerprinted from `source_text`, get their `pattern_id` updated in
  both relations, and keep name/origin/pinned. Their waystone tree is
  discarded (the old trails hang off the old id; invariant 8 makes that
  free) and rebuilt by traffic — from first execution, since they are
  user rows.
- **DROP PATTERN name**: removes both rows; the waystone tree under
  `waystone_root` is freed (or handed to retention for lazy reclamation —
  both are invariant-8-safe). If auto registration later re-learns the
  same shape, it reappears as a nameless auto row — DROP deletes the
  declaration, not the shape.

---

## 8. Implementation order

The build order is history. Three facts of the build that the design
record needs:

- `kNamedParam` folding to `ShapeTag::kValue` moved no stored
  `pattern_id`: `$` previously lexed as `kError`, so a statement containing
  one had no fingerprint at all, and making a previously
  unfingerprintable statement fingerprintable is the transition
  `fingerprint.hpp`'s bump rule permits without a version move.
- `flags` (u16, offset 38) precedes `origin` (u8, offset 40) so both keep
  their `offsetof` assert; the row grew 38 → 41 bytes.
- A `$param` in pk-equality position compiles to `kLookup` (§6, check 8).

---

## 9. Out of scope / open

What the design left out is unrecorded here beyond the refusals stated
above: the body is SELECT-class only (§2), types are checked at CREATE and
never at execution (§3.3), and `$name` binds reach no production.
