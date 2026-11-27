# EA's close: the kept parse's saving on a prepared statement, and its cost to a text one

**Finding.** The measurement compares EA's close on worktree `ea-prepared-statement-reuse` at `2c5c195f7`
(`v3.4.17-205-g2c5c195f7`, B) against the commit EA opened from, `481d101aa`
(`v3.4.17-202-g481d101aa`, A). Two independent runs, 24 and 48 rounds, agree on every sign below.

- **A prepared statement saves 0.47 to 0.55 us of server CPU on a point
  statement and 1.71 to 1.73 us on a three-relation join.** These are the
  paired B - A medians, each IQR clear of the in-run floor's:

  | shape | B - A | of A | p50 latency |
  |---|---|---|---|
  | point `SELECT` | -0.55 / -0.49 us | 8.1 - 9.0 % | 17.6 -> 17.0 us |
  | point `UPDATE` | -0.52 / -0.47 us | 7.4 - 8.1 % | 14.6 -> 14.1 us |
  | join | -1.73 / -1.71 us | 18.0 - 18.3 % | 21.0 -> 19.2 us |

  EA-S0 put the parse at 0.77, 0.76 and 1.93 us. The rest of each figure is
  still spent: the bind's AST copy and fill, and the substitution, which
  stays so that every form reading the text again still reads it.
- **A text statement is unchanged within the floor** (EA-R4):

  | shape | B - A, run 1 / run 2 |
  |---|---|
  | point `SELECT` | +0.04 / +0.07 us |
  | point `UPDATE` | -0.03 / +0.01 us |
  | join | +0.05 / +0.06 us |

  Each IQR overlaps the A2 - A floor's. **The point `SELECT`'s lean is
  positive in both runs and is not excluded at about 0.05 us (0.8 %).**
  Every instruction EA adds to the text path is a null test:
  - `C_PARSE`'s empty-name test;
  - a null `shared_ptr` set on the session at `C_EXECUTE` and reset at its end;
  - `KeptFor`'s null test at each of the statement's parse sites.

## 1. Stamp

| | |
|---|---|
| Executed | run 1: 2026-11-27 18:26:52 to 18:27:14 UTC (24 rounds); run 2: 18:27:42 to 18:28:23 UTC (48 rounds) |
| A | `481d101aa` (`v3.4.17-202-g481d101aa`), the commit EA opened from, exported with `git archive` to `/home/ukjin/ea-close/base-src` and built there |
| B | `worktree-ea-prepared-statement-reuse` at `2c5c195f7` (`v3.4.17-205-g2c5c195f7`), tree clean at the build |
| Build | `build-release` for each, `CMAKE_BUILD_TYPE=Release`, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, both built in this session at their commits; neither carries EA-S0's spike (no `parse_calls` string in either binary) |
| Binaries | copies in `/home/ukjin/ea-close/run/`: A `c537e112...fb1c`, B `0623df1d...2e977` (full hashes in `archive/.../binaries.sha256`); A2 is a second server from A's copy |
| Device | data files and WAL under `/home/ukjin/ea-close/run/work*`: `/dev/mapper/fedora-root`, xfs; not tmpfs |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn` (`tools/dk_close.py`'s `Server`) |
| Ports | 15891-15893 (run 1), 15895-15897 (run 2) |
| Pinning | the three servers on CPUs 2, 3 and 4 (P-cores), the mapping rotated every round so each arm has each CPU a third of the rounds; client on CPU 6 |
| Host | load 2.60 / 3.62 / 3.37 before run 1, 1.39 / 2.94 / 3.15 after run 2; no `cc1plus` in any cell (each cell's `host` in the JSON) |
| Driver | `tools/ea_close.py --rounds 24` and `--rounds 48`, `--k 2000 --n 10000`; the shapes and the data are EA-S0's (`tools/ea_s0_premise.py`) |
| Budget | under 2 minutes of runs and about 6 minutes of builds against the milestone's hour; nothing was cut |

## 2. Server CPU (us a statement, medians over rounds, run 2)

| shape | path | A | B | B - A paired median | B - A IQR | A2 - A IQR (floor) | A range | B range |
|---|---|---|---|---|---|---|---|---|
| point `SELECT` | text | 6.415 | 6.460 | +0.069 | -0.051 .. +0.134 | -0.098 .. +0.055 | 6.27 - 6.87 | 6.34 - 7.03 |
| point `SELECT` | prepared | 6.100 | 5.619 | -0.493 | -0.582 .. -0.424 | -0.101 .. +0.050 | 5.98 - 6.66 | 5.50 - 5.99 |
| point `UPDATE` | text | 6.683 | 6.680 | +0.006 | -0.060 .. +0.087 | -0.085 .. +0.017 | 6.59 - 10.22 | 6.58 - 9.81 |
| point `UPDATE` | prepared | 6.335 | 5.876 | -0.471 | -0.528 .. -0.416 | -0.058 .. +0.089 | 6.24 - 6.72 | 5.79 - 6.02 |
| join | text | 10.456 | 10.452 | +0.063 | -0.057 .. +0.158 | -0.228 .. -0.026 | 10.19 - 11.36 | 10.27 - 11.52 |
| join | prepared | 9.498 | 7.755 | -1.709 | -1.836 .. -1.598 | -0.222 .. -0.035 | 9.24 - 10.23 | 7.60 - 8.67 |

Run 1 (24 rounds), B - A paired medians:

| shape | text | prepared |
|---|---|---|
| point `SELECT` | +0.042 (IQR +0.020 .. +0.104; floor -0.052 .. +0.068) | -0.550 |
| point `UPDATE` | -0.032 | -0.521 |
| join | +0.047 | -1.728 |

## 3. Client latency (us, medians over run 2's 48 rounds of each block's percentile)

| shape | path | A p0 / p25 / p50 / p95 / p99 | B p0 / p25 / p50 / p95 / p99 |
|---|---|---|---|
| point `SELECT` | text | 17.4 / 18.0 / 18.1 / 19.1 / 20.1 | 17.5 / 17.9 / 18.1 / 19.1 / 20.2 |
| point `SELECT` | prepared | 16.8 / 17.3 / 17.5 / 18.4 / 19.5 | 16.3 / 16.8 / 16.9 / 17.9 / 18.8 |
| point `UPDATE` | text | 14.2 / 14.8 / 15.0 / 15.9 / 17.6 | 14.2 / 14.8 / 14.9 / 15.9 / 17.8 |
| point `UPDATE` | prepared | 13.8 / 14.4 / 14.5 / 15.5 / 17.4 | 13.3 / 13.9 / 14.1 / 15.0 / 16.9 |
| join | text | 21.4 / 22.0 / 22.2 / 23.4 / 24.9 | 21.4 / 22.0 / 22.2 / 23.4 / 24.9 |
| join | prepared | 20.1 / 20.7 / 20.9 / 22.0 / 23.6 | 18.3 / 18.9 / 19.1 / 20.3 / 21.6 |

No wait breakdown was taken: one connection a server on `relaxed`, nothing
parks. Run 1's tables are in `archive/.../close-run1.json`.

## 4. Against this engine's earlier numbers

| | EA-S0 at `481d101aa` | here | note |
|---|---|---|---|
| parse | 0.77 / 0.76 / 1.93 us | | point `SELECT` / point `UPDATE` / join |
| saving | | 0.49 - 0.55 / 0.47 - 0.52 / 1.71 - 1.73 us | the bind's copy, fill and substitution are left |
| A's prepared server CPU | 6.07 / 6.37 / 9.38 us | 6.10 / 6.34 / 9.50 us | same binary class, the spike absent here |

S0's prepared server CPU included the spike's clock reads.

## 5. What this does not measure

- Many connections, or `cores` above 1.
- A bind carrying a NULL, which runs the text path by design.
- The memory a kept parse holds.
- A statement past the kept parse's 64 KiB cap.
