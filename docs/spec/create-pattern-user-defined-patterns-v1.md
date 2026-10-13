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

A pattern is **a fingerprint-identified case tracked by statistics** — one
concept. The declaration was a second model of it (a name, a stored source
text, a catalog relation in user tuple format, an `origin` that changed
recording policy), and carrying two models of one thing was the cost the
operator declined to keep paying.

## 2. What holds today

- There is no `CREATE PATTERN`, no `DROP PATTERN`, no `sys.pattern_defs`,
  and `SHOW PATTERNS` prints no name column. `sys.patterns`, the
  fingerprint, the waystone directory, the trail recorder and replay are
  untouched: declared and auto rows were always found by the same lookup,
  so nothing that matches a statement to a pattern depended on the
  declaration.
- `TrailRecorder` has one threshold, `kAutoRecordThreshold = 2`, for every
  pattern (`include/kds/stats/trail_recorder.hpp`).

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

### 4.1 `SysPatternRow`'s `flags` and `origin` stay on disk

`SysPatternRow` keeps `flags` (`u16`, offset 38; bit 0 `kPatternPinned`)
and `origin` (`u8`, offset 40; `kOriginAuto = 0`, `kOriginUser = 1`); the
row is 41 bytes (`include/kds/catalog/rows.hpp`, `kOnDiskSize`). `flags`
precedes `origin` so both keep their `offsetof` assert. Every row is
`kOriginAuto`, nothing writes `kPatternPinned`, and nothing reads it —
retention is unbuilt. The two fields were separate because provenance and
retention policy moved independently. `SysCabinRow` carries the same
`origin` concept.
