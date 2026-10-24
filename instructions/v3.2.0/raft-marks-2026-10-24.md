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
