# CREATE PATTERN — user-declared patterns (spec, v1)

Status: **WITHDRAWN (operator, 2026-08-31). The code is removed; this file
records what was withdrawn, why, and what stays on disk.** The full design
text is in git history (this file before its 2026-10-13 compaction).

## 1. What was withdrawn, and why

`CREATE PATTERN <name> ($p <type>, ...) [WITH (pinned, expected_instances)]
OF <select>` declared a pattern ahead of traffic: a named `sys.pattern_defs`
row holding the statement's source text, recorded from the first execution
(`origin = kOriginUser`), exempt from waystone retention when pinned,
re-fingerprinted from its source on a fingerprint version bump, its
parameter types checked at CREATE. `DROP PATTERN` removed it.

A pattern is **a fingerprint-identified case** — one concept, named by its
`fetch_id` and nothing else. The declaration was a second model of it (a
name, a stored source text, a catalog relation in user tuple format, an
`origin` that changed recording policy), and carrying two models of one
thing was the cost the operator declined to keep paying.

**Since CF-S1b a pattern is tracked only when an operator opts it in**
(`waystone-concpets.md` §4a): `WAYSTONE ON <select>` sets one bit,
`kPatternWaystone`, on the pattern's own `sys.patterns` row. That bit is a
recording-policy bit, as the declaration's `origin` was, and it is
admissible where the declaration was not because it carries no second
identity: it sits on the fingerprint's own row, `fetch_id` stays the only
name, and the example's text is discarded once it is fingerprinted. So
`sys.patterns` is no longer "every shape that repeated" but **the opted
set**: one row per shape ever opted, with the bit saying which are opted
now.

## 2. What holds today

- There is no `CREATE PATTERN`, no `DROP PATTERN`, no `sys.pattern_defs`,
  and `SHOW PATTERNS` prints no name column. The withdrawal left
  `sys.patterns`, the fingerprint, the waystone directory, the trail
  recorder and replay untouched: declared and auto rows were always found
  by the same lookup, so nothing that matches a statement to a pattern
  depended on the declaration. Since CF-S1b there are no auto rows either:
  `WAYSTONE ON` is the only inserter of a row (`waystone-concpets.md` §4).
- `TrailRecorder` has one threshold, `kRecordThreshold = 2`, for every
  opted pattern (`include/kds/stats/trail_recorder.hpp`); renamed from
  `kAutoRecordThreshold` at CF-S1b, since no pattern is automatic.

### 3.1 `kNamedParam` still lexes

`TokenType::kNamedParam` (`$` + identifier, case-folded; distinct from
`kParam`, which is `?`) still lexes and is accepted by no production: a
`$name` parameter is refused everywhere.

### 3.2 It folds to `ShapeTag::kValue`

In the fingerprint's shape stream `kNamedParam` folds to `ShapeTag::kValue`,
the point where int literals, string literals and `?` binds already meet,
so `= $x`, `= 42` and `= ?` are one shape. Neither the parameter's name nor
its declared type entered the stream: live traffic carries no declaration,
and anything a declaration contributed would have made declared patterns
match nothing. `kFingerprintVersion` did not move when this landed: `$`
previously lexed as `kError`, so no stored id or `arg_hash` changed, and
making an unfingerprintable statement fingerprintable is a transition
`fingerprint.hpp`'s bump rule permits.

### 4.1 `SysPatternRow`'s `flags` and `origin` stay on disk, re-scoped

`SysPatternRow` keeps `flags` (`u16`, offset 38) and `origin` (`u8`,
offset 40); the row is 41 bytes (`include/kds/catalog/rows.hpp`,
`kOnDiskSize`), and `flags` precedes `origin` so both keep their
`offsetof` assert. **Since CF-S1b** bit 0 of `flags` is `kPatternWaystone`,
the opt-in `WAYSTONE ON` sets and `WAYSTONE OFF` clears, re-scoped from
`kPatternPinned`, which had no writer since this withdrawal; every other bit
is written 0. `origin` is a **reserved byte**, written 0 and read by
nothing: `kOriginAuto` and `kOriginUser` are deleted, because `WAYSTONE ON`
is the only creator of a row and provenance has no second value. Superblock
33 marks the change of meaning, so a version-32 binary refuses the volume
rather than reading an opted row as pinned. `SysCabinRow` keeps its own
`origin` (`kCabinOriginAuto` / `kCabinOriginUser`), which is unaffected.
