# Work order ES - four or more clients: the equal-client scenarios at `cores = 8`, and what serialises KDS when it has the cores

Status: **WRITTEN 2026-11-28, not opened** (`raft-marks-2026-11-28.md`
§25), on the operator's word *"write work milestones for the three causes
behind the current performance gap, and push"*; the third of the three EO
names. Written by CLA in a claude.ai session in the main checkout on
`main` at `c43dbf46` (`v3.4.17-332-gc43dbf46`), documents only, no
worktree. Nothing was built or run.

Tags: `[operator]`, `[source-read]`, `[measured]` (quoted), `[design]`,
as in DT's order.

Related: `bench/v3.5.1/results-eo-close-v3.5.1-63-g9543cf720.md` ("What
the numbers say", item 3; EO-Q6); `docs/spec/crosscore.md`;
`docs/spec/sched.md`; `server/load_placer.hpp`; EN
(`workorder-en-two-core-cells.md`: the lost kick and the hot-row loser),
which this order measures against and does not build.

## 0. What ES is

`[measured]` EO ran KDS at `cores = 1` (EO-Q6) while PostgreSQL's backends
spread over eight CPUs, and its own file says **"the table measures
EO-Q6's configuration, not KDS's concurrency"**. Scenario 1's join at 1,
2, 4 and 8 connections reads 1.92x, 1.10x, 0.57x, 0.29x; scenario 5's
mix 0.84x at one terminal and 0.39x at four (44,521 new orders a minute
against 112,294). EO recommends a `cores = 8` arm.

`[design]` So the third cause is first a measurement gap, and ES closes it
before it builds anything: **the multi-client cells at `cores = 8`** (S0).
What KDS then shows is one of: it scales and the gap was the
configuration (ES closes there); or something serialises it, and S1 names
what. The candidates already in the tree:

- the session placer re-deciding a session's core only at a transaction's
  head, so four terminals may share fewer cores than they could
  (`CLAUDE.md`, Cross-core execution);
- the one WAL stream, every core appending under core 0's latch, and a
  segment roll's sync under it (`wal.md` §6; DY);
- a hot row's first-updater-wins loss and the lost kick at a group commit's
  wake (EN's two causes; scenario 5's district row is exactly a hot row);
- the commit path EQ owns.

## 1. Rules

- **ES-R1** Every engine gets the same CPUs: KDS at `cores = 8` on the
  eight P-cores EO gave PostgreSQL's backends; the harness on the E-cores,
  as EO-Q6.
- **ES-R2** No delta against a Python driver's file and none against EO's
  `cores = 1` cells as an engine change: the `cores = 8` series is new.
- **ES-R3** Oracle's figures stay out of the tree (`bench/README.md`, "The
  Oracle control").
- **ES-R4** A remedy belongs to the order that owns its cause (EN, EQ, DY);
  ES builds only what no other order owns.

## 2. Items for the operator (ES-Q), CLA's proposals

| item | question | proposal |
|---|---|---|
| ES-Q0 | Open after EP; may run beside EQ and ER | **(a)** yes |
| ES-Q1 | The arm | **(a)** scenarios 0, 1 (connections 1 to 8) and 5 (terminals 1 to 8) at `cores = 1` and `cores = 8`, `group` and `strict`, with each core's busy share, `sys.wait_stats` per event and the placer's moves recorded |
| ES-Q2 | The bar for "it scales" | **(a)** none set in advance: S0 reports the curve and the waits that dominate it, and ES-Q3 is written from them |
| ES-Q3 | What S1 builds | written at S0, from what dominates |
| ES-Q4 | Order with EN | **(a)** S0 runs before EN's S1, so EN's price is measured on the same cells |

## 3. Stages

- **ES-S0 the arm** (ES-Q1): a results file of the curves and their
  waits.
- **ES-S1 the serialisation named**: the wait or latch that dominates at
  four or more clients, with its source and the order that owns it; ES-Q3
  written.
- **ES-S2 a remedy** only where no other order owns the cause.
- **ES-S3 the close**: the same cells again after whatever of EN, EQ and
  S2 has landed.

## 4. What ES does not do

- It does not change the placer, the WAL stream or the lock family on its
  own; those belong to the orders named above.
- It does not move the harness's defaults; `cores = 8` is an arm beside
  `cores = 1`.

## 5. Stage log (CLA, appended as stages land)

### Written 2026-11-28

In the main checkout on `main` at `c43dbf46`, documents only. Not opened;
no build, no run.
