# instructions/v3.2.0 — the version's operator input

Opened 2026-10-14 on `worktree-cn4-transition-refresh` at `b0bc959c`
(`v3.1.0-31-gb0bc959c`). **The operator named the version v3.2.0 on
2026-10-14** for the work this directory holds. The annotated tag is a
separate act and has not been taken. Until it is, `git describe --tags`
reads `v3.1.0-*`.

| document | what it is |
|---|---|
| `workorder-bv-transition-constraints.md` | **BV — Transition**: a column's allowed state changes (initial states, `old => new` edges, deletable states) as a row-local, stateless constraint. Takes up CN-4. **Built 2026-10-14, every stage landed** on `worktree-cn4-transition-refresh` (S1 `3a68dd25`, S2+S3 `3eb5a1eb`, S4 the close); the owning spec is `docs/spec/transition.md`. Opened on the operator's standing go-ahead (`raft-marks-2026-10-14.md` §4); BV-Q1 the operator's mark, every other item adopted as CLA proposed. Not merged to `main`, not tagged |
| `workorder-bw-instant-add-drop-column.md` | **BW - instant `ADD COLUMN` / `DROP COLUMN` over reserved 16-byte slots.** **Written and opened 2026-10-14** on `worktree-bw-instant-add-drop-column` from `750496b1` (`v3.2.0-12-g750496b1`), on the operator's words W0-W3 (`raft-marks-2026-10-14.md` §5-§7): D-a..D-f marked (a) by the operator; BW-Q1..Q7 adopted as CLA proposed on the standing go-ahead (W3), BW-Q3 `[quiet-wrong]`; every measurement held (W2). `CREATE TABLE ... SPARE n` fixes the row size with `n` slots; `ADD` takes a slot, `DROP` flags a column, neither touches a tuple; both transactional under the relation `X`. Stages: S1 format and layout, S2 DROP, S3 ADD, S4 the text, S5 the close. Not part of AR0 §8's chain |
| `raft-marks-2026-10-14.md` | The operator's words of 2026-10-14: CN-4's re-read and opening, BV written, BV-Q1 marked (a), BV to its close on the standing go-ahead; BW written, D-a..D-f marked (a), BW's measurement held, BW-Q1..Q7 adopted on the standing go-ahead |
