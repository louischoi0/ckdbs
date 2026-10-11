# Pending: one "is this value a Keystone id" test for the named pk and the foreign key

Status: **PENDING** - the operator deferred it. It decides nothing and opens no stage.
Raised: 2026-10-11, in conversation with the operator.
Read at: `ac04b84e` on `worktree-fix-negative-fk-value`. The `[source-read]`
line numbers are at this commit.
Related: `docs/spec/heap-and-tuple.md` §4.1 (invariant 11, the Keystone id
space); `instructions/v3.0.0/workorder-bd-sorted-leaf-named-keys.md` (BD-R5,
the named-key refusal); `docs/spec/foreign-keys.md` §2 (an integer that is no
Keystone id references no parent).

## 1. What prompted it

`[design]` The `critics-developer` review of `e8d7d690` found it. That commit
fixed the negative-foreign-key-value bug, and the review found that the engine
now answers one question twice: is this integer `AstValue` a Keystone id in
`[kFirstRowId, kMaxKeystoneId]`? The fix left the second copy alone because
merging it changes BD-S3's named-key path, which is a different change from
the bug fix.

`[operator]` *"Record the consolidation of the named-pk check logic in a
document."*

## 2. What the tree does

`[source-read]` There are two definitions, and both are in
`src/server/command_dispatcher.cpp`.

- **The foreign-key check, `FkParentIdOf` (`:3606`).** A value is no id when
  any of these holds: `parser::IntLiteralWrapped(value)`, `int_val < kFirstRowId`,
  or `int_val > kMaxKeystoneId`. Otherwise its id is `int_val`.
  `ResolveForeignKeyParents` (`:3629`) and `CheckForeignKeyOnWrite` (`:3747`)
  both call it.
- **The named pk in `InsertOneRow` (`:5629-5666`).** The literal's spelling
  (`raw_int_text`, or `int_val` printed when the value was built rather than
  parsed) is read digit by digit:
  - a leading `-` is refused, except `-0`, which becomes `0`;
  - the loop accumulates up to `kMaxKeystoneId` and refuses past it;
  - a value below `kFirstRowId` is refused.

  The refusal is `OutOfRange "primary key X is outside the Keystone id space
  [1, 1099511627775] (byte N)"` (BD-R5 2.).

`[design]` As read, the two agree on every input:

| input | foreign key | named pk |
|---|---|---|
| a negative value | no id | no id |
| `0` and `-0` | no id | no id |
| `007` | 7 | 7 |
| above `2^40 - 1` | no id | no id |
| a literal past int64, including one whose `int_val` wraps into range, such as `2^64 + 5` | no id | no id |
| a built value (`raw_int_text` empty) | judged by `int_val` | judged by `int_val` |

Only the refusal differs (`FK_VIOLATION` against `OutOfRange`), and each caller
keeps its own.

## 3. The options

- **(a) One function, two callers.** Rename `FkParentIdOf` to `KeystoneIdOf`
  (or move it beside `kMaxKeystoneId`). The pk path then becomes
  `auto id = KeystoneIdOf(key); if (!id) return exhausted(); supplied_id = *id;`,
  so the digit loop and the `-0` special case go, about 12 lines. Its cost is
  that BD-S3's named-key cells (`OutOfRange` for 0, a negative number, `2^40`
  and a wrapped literal, each with its byte) must stay green and should be
  re-read against the table above. Nothing on disk or on the wire changes.
- **(b) Keep both.** Each is correct today. The risk is drift: a later change
  to the id space or to how a literal is spelled reaches one of the two and not
  the other. That is the kind of drift that left the foreign key's `int_val < 0`
  bail behind the pk check.

`[design]` CLA proposes (a), done as a small change of its own with BD-S3's
cells as its gate.
