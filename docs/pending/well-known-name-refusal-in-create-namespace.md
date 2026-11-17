# Pending: `CREATE NAMESPACE` refuses a well-known object's stored name on one catalog only

Status: **PENDING** - the operator deferred it on 2026-11-17 with the other
logic defects of the audit fixed. Decides nothing, opens no stage, licenses
no code.
Raised: 2026-11-13 by the whole-engine audit's second run, as its findings
G39 (a logic defect) and G53 (dead code); verified 2026-11-17; deferred
2026-11-17.
Read at: `3c30fe67` (`v3.4.12-11-g3c30fe67`), on worktree
`cw-optimistic-page-read`, branch `worktree-audit-g27-g42-fixes`. Every
`path:line` below is at that commit.
Related: `docs/pending/whole-engine-audit-second-run-twelve-slices.md` (the
findings under `#### G39.` and `#### G53.`, their verdicts in §11.1 and
§11.2, and §12, which fixed the audit's other logic defects and left this
one); `docs/spec/namespace.md` NS7; `docs/spec/catalog.md` CT7;
`instructions/v3.2.0/raft-marks-2026-11-17.md` §9.

## 1. What prompted it

`[operator]` On 2026-11-17, after CLA reported thirteen of the audit's
fifteen surviving logic defects fixed and two waiting for a ruling: *"For
now put the two issues in pending, and push the work so far, no verify."*
(given in Korean; recorded in English). This is one of the two; the other
is `archiver-stalls-on-a-hole-among-leftover-segments.md`.

## 2. What the tree does

`[source-read]`

- `Catalog::CreateNamespace` makes two refusals of a name beyond "it is
  live". The first is unconditional: the spellings `sys` and `public`
  (`src/catalog/catalog.cpp:2261-2264`). The second asks the in-memory
  registry of well-known objects: a name `sys_objects_.GetByName` finds is
  `AlreadyExists` (`:2265-2272`). The registry holds 22 stored names -
  `namespaceSys`, `namespacePublic`, `type_int` and the other type names.
- The registry is filled by `InitWellKnownObjects`
  (`src/catalog/catalog.cpp:573`), which has one caller, `Bootstrap()`
  (`:686`), which runs on a fresh volume only
  (`src/bootstrap/bootstrap.cpp:124`). A mount of an existing volume builds
  a bare `Catalog` (`bootstrap.cpp:92`), and so does every core but the
  first (`src/server/core_runtime.cpp:228`).
- So `CREATE NAMESPACE namespacePublic` is refused on core 0 of the process
  that created the volume, and admitted on every other core and after every
  restart. One statement, two answers, by core and by process age.
- **`sys` and `public` are not affected**: their refusal does not ask the
  registry.

## 3. What the verification found

`[source-read]`, two readers told to refute G39 and one G53.

- **The refusal protects nothing.** No production code resolves a name
  through the registry: its only reader is this check
  (`sys_objects()` / `GetByName` have no other caller outside
  `tests/catalog_test.cpp`). `FindNamespaceOidByName` answers the SQL
  spellings and then page rows; the well-known objects have no page row. A
  user namespace named `namespacePublic` collides with nothing a statement
  can observe.
- **The documents disagree.** `docs/spec/namespace.md:187-189` (NS7) lists
  two refusals: a live name, and "the two reserved spellings `sys` and
  `public`". `include/kds/catalog/catalog.hpp:463` says a namespace is
  "name-unique across the registry and the page", and
  `tests/catalog_test.cpp:205` pins the refusal of `namespacePublic` - on a
  catalog that ran `Bootstrap()`, the one state in which it holds.
- **G53 is the other half of the same question.** It called the registry
  vestigial and proposed deleting it (about 110 lines). It was refuted as
  dead code - a production path reaches it and its answer is a
  client-visible refusal - and survives only as one of the two answers
  here.

## 4. The options

`[design]`

- **(a) Delete the check, and the registry with it.** NS7 as written: a
  well-known object's stored name is not a reserved spelling. Removes the
  check at `catalog.cpp:2265-2272`, `SysObjectRegistry` and its two files,
  `InitWellKnownObjects`, the member and its accessor, and the cells at
  `tests/catalog_test.cpp` that read the registry or pin the refusal;
  corrects the header sentence at `catalog.hpp:463` and the stale mentions
  the verifier listed (`well_known.hpp`, `catalog_cache.hpp`,
  `namespace.md:70-71` and `:202`). Every core and every mount then gives
  the answer all but one give today. A volume on which someone has already
  created a namespace of such a name is unchanged.
- **(b) Refuse on every catalog, and say so in NS7.** A `constexpr` list
  of the well-known stored names that every `Catalog` reads, in place of
  the registry's vector; NS7 gains the third refusal. Most of the
  registry's code still goes. A name an existing volume already holds as a
  namespace stays, and a second create of it is refused as live anyway.
  The verifier's note against the audit's own fix - calling
  `InitWellKnownObjects` from the constructor - is that a constructor must
  not fail (`docs/rules/rules.md`), and that one allocates.

Either is a small change with a low risk. They differ in one thing only:
whether a client-visible refusal that a test pins is removed, or one the
spec does not list is added.

## 5. What a ruling has to say

Whether a well-known object's stored name is refused by `CREATE NAMESPACE`
at all. No spec decides it: NS7 says no by omission, a header comment and
one cell say yes.
