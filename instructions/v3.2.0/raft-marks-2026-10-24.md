# The operator's words of 2026-10-24

## 1. CI pushed with the hook skipped

| | |
|---|---|
| **Word** | W4 (CI's session): *"push no ver"*, after CLA reported CI's every stage landed at `bcdadd10` and stopped at the push gate |
| **Mark** | **CI's branch pushed to `main` with `--no-verify`**: S0 `d40e92f9`, S1 `c31c00dd`, S2 `614864c4`, the close `aaec0ba8` and its review `c8aefb60`, with `origin/main` merged at `bcdadd10`. The full Debug suite ran 4335/4335 on `bcdadd10`'s build inputs; this record adds only `instructions/` |
| **Does not settle** | CI's close (the operator's word); a tag, a version |
| **Recorded at** | this file; the `raft-marks-2026-10-24.md` row in `instructions/v3.2.0/index.md` |

## 2. Version 3.4.5 tagged

| | |
|---|---|
| **Word** | *"tag 3.4.5"* |
| **Mark** | **`v3.4.5`**, an annotated tag on `main` at `0b9013c9`, the head of `origin/main` when the tag was taken (before §1's CI push landed). Its message states the expeditor test fixed (`f3e11ff3`), which empties `docs/inflight/bugs/`, CG's close measured, and the gaps that bound it. The full Debug suite ran 4321/4321 at `f3e11ff3`, whose build inputs the tagged commit shares, so the tag was pushed with `--no-verify`. CLAUDE.md's version line moved to `v3.4.5` |
| **Does not settle** | CG's close; CI's close; a later version |
| **Recorded at** | this file; the tag message; CLAUDE.md's Version Management; `instructions/v3.2.0/index.md` |

## 3. CG closed

| | |
|---|---|
| **Word** | W8 (CG's session): *"close cg"*, after CLA reported CG-S3 measured and pushed at `0b9013c9`, with the walking `UPDATE`'s slow mode unexplained and its confound check cut |
| **Mark** | **CG closed** (`workorder-cg-on-conflict-do-update.md`, header and §7 "CG closed 2026-10-24"): `ON CONFLICT DO UPDATE` built on the tuple key and on the one column of a unique index, every stage measured. The walking `UPDATE`'s slow mode at `6775aec1` stays in `known-gaps.md`, unexplained |
| **Does not settle** | The slow mode's cause; BL-Q10 and BL-Q11's marks; a push; a tag, a version |
| **Recorded at** | this file; the CG order's header and §7; the CG row in `instructions/v3.2.0/index.md`; CLAUDE.md's query row |

## 4. CI closed; its "tag 3.4.5" found the version already taken

| | |
|---|---|
| **Word** | W5 (CI's session): *"close ci, tag 3.4.5"* |
| **Mark** | **CI closed**: every stage landed and pushed (`fb36e416`), CI-S3 not built (CI-Q6 (a)), the close measured at `v3.4.3-13-g614864c4`. The gaps it leaves are `known-gaps.md`'s "The online index build (CI)". **No tag taken by CI's session**: `v3.4.5` already stood at `0b9013c9` (§2), taken and pushed by another session before §1's push landed, so it does not carry CI. A pushed tag is not moved; which version carries CI is the operator's word |
| **Does not settle** | the version that carries CI; a later milestone |
| **Recorded at** | this file; the CI order's header and §7; the CI row in `instructions/v3.2.0/index.md` |

## 5. Version 3.4.6 tagged

| | |
|---|---|
| **Word** | W6 (CI's session): *"tag 3.4.6"*, after CLA reported that `v3.4.5` stood at `0b9013c9`, before CI, and proposed (a) a new tag at the head of `main` over (b) moving `v3.4.5` |
| **Mark** | **`v3.4.6`**, an annotated tag on `main` at `ca49241a`, then the head of `origin/main`. It carries CI and CG's close. Its message states CI built and closed, CG closed, CI's premise and close measured, and the gaps that bound it. The full Debug suite ran 4335/4335 at `bcdadd10`, whose build inputs the tagged commit shares (everything after it is under `instructions/` and CLAUDE.md), so the tag was pushed with `--no-verify`. CLAUDE.md's version line moved to `v3.4.6`; `v3.4.5` is left where it stands |
| **Does not settle** | a later version |
| **Recorded at** | this file; the tag message; CLAUDE.md's Version Management; the CI order's header and §7; `instructions/v3.2.0/index.md` |

## 6. A1 closed

| | |
|---|---|
| **Word** | *"close A1"* |
| **Mark** | **A1 closed** - a relation-`X` build gives its core back between slices: `CREATE INDEX` and `REINDEX INDEX` since stage 2a (`37a60b10`), `CREATE ASSERTION` and `CREATE TRANSITION` since stage 2b (`cca7415b`). A1 kept no work order in the tree; its close was measured at `18ffb998` (`bench/v3.4.0/results-a1-close-v3.4.0-11-gf53e62db.md`): the yield costs nothing measurable on the statement path and bounds a same-core read behind a build to one slice, but a WAL segment roll during a large build still stalls that read, which stays in `docs/inflight/known-gaps.md` and `docs/spec/sched.md` §3 |
| **Does not settle** | The roll's stall; a push; a tag, a version |
| **Recorded at** | this file; the 2026-10-24 row of `instructions/v3.2.0/index.md` |
