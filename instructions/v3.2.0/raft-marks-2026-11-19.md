# Raft marks, 2026-11-19

The operator's words of 2026-11-19 that open a new matter, and what CLA
took under them.

## 1. v3.4.13 named and tagged

| | |
|---|---|
| **Word** | *"tag 3.4.13"* - after CLA's report that CX landed on `main` at `8dc37b80`, with CY's change restored and the full Debug suite 0 failed of 4732 there (`raft-marks-2026-11-18.md` §49) |
| **Mark** | **The version of record is `v3.4.13`**, the operator's own word: an annotated tag on `main` at `8dc37b80`, pushed. Its message states what is built since `v3.4.12` (CX, CY, DF-S1, the audit's G27-G30, G33-G38, G40-G42), the superblock's move from 35 to 37 (a `v3.4.12` volume does not mount), the suite, the overhead files, and the gaps that bound it - CX's close regression first, unattributed |
| **Does not settle** | the regression's bisect; CX-Q36 (b), CX-Q37 (b)/(c); anything after `8dc37b80` |
| **Recorded at** | this file; `CLAUDE.md`'s version of record; this file's row in `instructions/v3.2.0/index.md` |

## 2. CZ pushed with the hook skipped, C1 alone; the merged tree not suite-run

| | |
|---|---|
| **Context** | CZ closed on 2026-11-18 with C1 built, on worktree `cz-checkpoint-roll-wait` at `f8ffccea`, not merged and not pushed (`raft-marks-2026-11-18.md` §50). A session rooted outside the repository reported that state on 2026-11-19, with `origin/main` 12 commits ahead of the branch's base `9712f5f0` (CX, CY, `v3.4.13`), and then C1 against C2 from CZ-S1 and CZ-S1b |
| **Word** | *"Push CZ, C1 only, no verify."* and, while the merged tree's build was running, *"Just push; I am ending the session right away."* (given in Korean; recorded in English) |
| **Mark** | None. **CZ-S3 and CZ-S4 go to `main` with `--no-verify`, C1 alone**: `origin/main` at `11f72ce9` merged into the work branch at `44de9abc` - conflicts in `instructions/v3.2.0/index.md` and `raft-marks-2026-11-18.md` only, each resolved by keeping both sides, CZ's section renumbered from §47 to §50 there; `src/server/expeditor.cpp`, `tests/CMakeLists.txt`, `CLAUDE.md`, `known-gaps.md`, `sched.md` and `wait-events.md` merged by git with no conflict. C2 is not in it: `src/wal/file_log_device.cpp` is `origin/main`'s, and the spike branch `worktree-cz-s1-spike` is not merged. **The hook was skipped. The full suite was not executed on the merged tree**: it ran 0 failed of 4727 at `74861601`'s build inputs, before CX and CY reached `main`; the Debug build of `44de9abc` was at about half when the second word came, and no test of it had run. **No `critics-developer` review of the merge was run.** The overhead was measured at CZ's close on `74861601` against `9712f5f0`, not on the merged tree |
| **Does not settle** | whether the merged tree builds and passes - the next session owes the full suite at `44de9abc`'s build inputs before anything is built on it; whether BL's bar (v) met in the probe's cell closes BL-R7's remainder; the spike branch's deletion; DG-Q0..Q10 - DG stays unopened, and C2 with it; a tag, a version |
| **Recorded at** | this file; the CZ order's header and §7; the CZ row and this file's row in `instructions/v3.2.0/index.md` |
