# instructions/v3.0.0 — the version's operator input

Opened 2026-09-02 on `worktree-v3.0.0-arch-revision` at `d15b5ac`
(`v2.7.0-134-gd15b5ac`), the first thing written under `instructions/`
since the tree was emptied at `7f0193b`.

**The version in one sentence:** v3.0.0 is the big-bang change — one WAL
stream for the instance, shared memory in place of shared-nothing,
ownership decomposed into affinity and allocation — and it is built
milestone by milestone from AR0's §8, each milestone under one work
order, the `cores = 1` path byte-identical throughout.

The version number is AR0 D15's *proposal*. The operator names versions
(`CLAUDE.md`, Version Management); until the word, `git describe --tags`
reads `v2.7.0-*` and every measurement carries that.

| document | what it is |
|---|---|
| `ar0-architecture-revision.md` | The governing draft, verbatim as the operator supplied it on 2026-09-02, with AR0-V appended: the source-read verification at `d15b5ac`, which is what the tree says wherever the body disagrees. D1–D16 pending; **M0 has the go-ahead** |
| `workorder-al-m0-single-wal.md` | **AR0 M0 — the single WAL stream.** The survey of every per-core-stream assumption at `d15b5ac` (AL-3), the rulings M0 needs (AL-4, with AL-R1 the append mechanism as the one that decides the shape), and the stages AL-S0..S9 with their cells and sizes (AL-5). AL-S0 is the hour the operator ordered |

**Read AR0-V before citing AR0's body.** Three of the body's numbers are
already attributed in the tree to causes other than the ones it names,
and two of its D-items name values the tree never shipped.
