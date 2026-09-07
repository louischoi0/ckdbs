# `MaterializeIndexDefinition`'s owner refusal is keyed on a hook only a test installs

**Found** 2026-09-07 by the `critics-developer` pass on AW-S1b
(`af86026` on `worktree-aw-m1-close`). **Not fixed** — the fix is a decision,
not a patch. The gate's predicate did not change; what changed is which side
of it production is on.

## The gate

`src/catalog/catalog.cpp:3352`:

```cpp
if (seed == AnchorSeed::kHere && access.value()->owner_core != core_id_ && on_publish_) {
    return Status::NotImplemented("… may not seed its anchor (workplan-peer-writer.md PW1c-6)");
}
```

`CreateIndex` writes a *relation* page since PW2-3's anchor seed, so this
core writing a peer-owned relation's anchor would make it the second writer
of a page whose handoff had already granted the peer — PL §9 rule 5's mount
refusal. The third conjunct is deliberate and its reason is stated at the
site: **an installed publisher is what makes ownership a handoff fact.**
The P4e harness builds indexed fixtures on rotated relations through a
hook-less catalog, where nothing was ever granted away and core 0 is still
the only writer, so refusing there would refuse a legitimate build.

## What inverted

`Catalog::SetRelationPublishHook` had exactly one production installer:
`Expeditor::Serve`'s CC7 publish. AW-S1b deleted it with the grants. The
only caller left in the tree is `tests/core_runtime_test.cpp:619`.

So the predicate that used to read *"on in production, off in the harness"*
now reads *"off in production, on in one test"*. The refusal fires nowhere
a user can reach.

## Why it was not simply re-keyed

Three options, and none is obviously right:

1. **Re-key on ownership alone** (`seed == kHere && owner_core != core_id_`).
   This refuses the P4e harness — `EveryShippableShapeAnswersExactlyWhat`
   `LocalExecutionAnswers` builds an index on a rotated relation through a
   core-1 dispatcher — which is precisely the case the third conjunct was
   added to admit.
2. **Delete the gate.** What it guarded is a page already handed off, and
   nothing hands pages off any more; the dispatcher's PW1c-6 refusal still
   covers the statement path with its byte position. But the catalog is
   "the door every non-DDL caller comes through" by this file's own
   doctrine, and deleting the door because the last known intruder left is
   the shape of decision this tree does not make silently.
3. **Keep it and retire the hook**, making the refusal unreachable by
   construction rather than by accident, and say so.

The predicate the gate really wants is *"has any core other than this one
been able to write this relation's pages"*, and under one frame table that
question has no page-level answer at all — it is the routing decision, which
this layer does not see.

## What is true meanwhile

The gate is inert in production. Nothing is unsound: core 0 running
`CREATE INDEX` on a peer-owned relation is refused at the dispatcher
(PW1c-6), and a peer running it ships the DDL to core 0 (CC13). What is
lost is the catalog-level defence for a non-dispatcher caller, which is a
defence against a caller that does not exist yet.

## Owner

Whoever takes ownership routing next — AT (`ar0-5-amendment-uniformity.md`)
if it lands before anything else touches `CreateIndex`. Related:
`docs/spec/crosscore.md` CC7's owner-builds exception, which is the ruling
that says core 0 keeps the catalog half and the owner runs the page half.
