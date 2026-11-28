# EM-S0: what the statement front end's repeated work costs the server

**Finding.** On worktree `em-front-end-reuse` at `b46ac5140`
(`v3.5.1-69-gb46ac5140`, the EM-S0 spike applied), the work EM's candidates
would remove clears EM-Q1 (a)'s 3 % bar on a shape for three of them:

- **C4, the uncached namespace scan.** `FindNamespaceOidByName` costs
  0.415 µs of a qualified `ns1.t2`'s 7.08 µs (5.9 %). With one view in the
  instance it scans twice, 0.77 µs (10.2 %); with the view and 100 more
  relations, 2.76 µs (29.1 %). The cell is 0.48, 0.90 and 2.96 µs dearer
  than the same `t2` under `USE ns1`.
- **C1, the second lex of an unnamed text statement.** Net of a zero-hole
  kept parse's bind and copy, it is 0.08 to 0.12 µs on the four point shapes
  (1.3 % to 1.8 %), under the bar, and 0.68 µs on the three-relation join
  (6.4 %), over it.
- **C3, a multi-variant plan's lex.** `ArgumentsOf` costs 0.66 µs of 11.63 µs
  (5.7 %) on the join under a two-variant plan.

**C2, the parked write's re-parse, does not.** A walked `UPDATE` of 60,000
rows parses 6 times, about 3.3 µs a parse, against about 20 ms of server CPU
a slice (0.02 %).

C1 as the order first wrote it (EM-Q8 (a) through today's `KeptParse::Of`)
would be slower than the text path it replaces. It lexes the text three
times where today's text path lexes twice: net of the parse it moves and
of the lex it removes, it adds 0.33 to 0.42 µs a point statement and
0.76 µs on the join (§3's `named0` note). C1 is therefore admitted only
with the zero-hole path the order's §8 now states.

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-28, 11:33:58 to 11:34:14 UTC (one server, one run) |
| Branch / commit | `worktree-em-front-end-reuse` at `b46ac5140`, `git describe --tags` = `v3.5.1-69-gb46ac5140`, plus the EM-S0 spike (§2), applied to a detached copy of the tree; no EM code in the build |
| Build | `build-release`, `CMAKE_BUILD_TYPE=Release`, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, built in this session at this commit with the spike applied |
| Binary | a copy at `/home/ukjin/em-s0-run/kds_server`, `sha256` `dee5a37ec2a14e6965327134fce817d71336dd1a7277895bce4be549cc504ab5` (`archive/.../binary.sha256`) |
| Device | data file and WAL under `/home/ukjin/em-s0-run/work`: `/dev/mapper/fedora-root`, xfs (`df -T`); not tmpfs |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, every other key default (`tools/dk_close.py`'s `Server`) |
| Port | 15883 |
| Pinning | server on CPU 2 (a P-core), client on CPU 5 (a P-core) |
| Host | load 0.85 / 2.16 / 1.68 before, 1.28 / 2.18 / 1.70 after; no `cc1plus` (`s0.json` `meta`) |
| Driver | `tools/em_s0_premise.py --rounds 8 --k 2000` (`--n 10000`, `--walk-rows 60000`, `--walk-k 10`), new in this stage; EA's `tools/ea_s0_premise.py` is left unmodified |
| Client | `tools/kwp.py`: `execute` (text), `execute_prepared` (prepared), and `prepare` + `execute_prepared` with no parameter per statement (`named0`) |
| Two aborted runs | a first run stopped at round 5 when the delete shape's relation ran out of rows (sized for two paths, not three), and a second stopped at the walk on `max_locks_per_txn` (200,000 rows). Both were discarded, and the run above is the third |

## 2. The instrument

Server CPU per statement is `/proc/<pid>/task/*/schedstat` over a block of
2,000 statements, every server thread. The parts come from a spike that is
not in the source tree (`archive/.../em_spike.hpp`, `apply_spike.py`). It is
EA-S0's spike, which times parse, `FingerprintOf` (`fp`), `ArgumentsOf`
(`args`), `Substitute` (`subst`), `PlaceholderCount` (`count`) and
`exec::Compile` (`compile`), with five more scopes:
`FindNamespaceOidByName` (`ns`), `KeptParse::Of` (`keptof`), `HolesDelimited`
(`holes`), `parser::Bind` (`bind`) and `BoundStatement::Materialize` (`mat`).
Each scope is one `steady_clock` pair, about 40 ns, and that cost is inside
every number. A nested scope is counted inside its parent too: `args` inside
`compile` under a plan, `ns` inside `compile` for `ResolveRelation`'s call
(`ResolveView`'s is outside it), and `parse`, `holes` and `args` inside
`keptof`.

Each round runs, per shape, every path, the order rotating per round. 300
statements a path a shape warm the server first. Keys are uniform over
10,000 rows of each relation, all BTREE. The phases run in order: every
shape (`base`); the namespace pair after `CREATE VIEW em_v AS SELECT id, v
FROM t` (`view`); the pair after 100 more `CREATE TABLE`s, the view kept (`pad100`); the
join after `CREATE PLAN em_p ... VARIANT lo WHEN ARG(1) <= 5000 ... VARIANT
hi ELSE ...` (`plan`); then the walked `UPDATE`.

## 3. Server CPU and its parts (µs a statement, medians of 8 rounds; the walk, of 3 blocks of 10)

| cell | server CPU | range | fp | parse | args | bind | mat | ns | compile |
|---|---|---|---|---|---|---|---|---|---|
| point `SELECT`, text | 6.535 µs | 6.431 - 6.842 µs | 0.245 µs | 0.811 µs | - | - | - | - | 0.450 µs |
| point `SELECT`, prepared | 5.622 µs | 5.541 - 6.006 µs | - | - | - | 0.049 µs | 0.081 µs | - | 0.461 µs |
| point `UPDATE`, text | 6.772 µs | 6.704 - 6.799 µs | 0.252 µs | 0.792 µs | - | - | - | - | - |
| point `UPDATE`, prepared | 5.912 µs | 5.859 - 5.929 µs | - | - | - | 0.068 µs | 0.089 µs | - | - |
| point `INSERT`, text | 6.180 µs | 6.161 - 6.217 µs | 0.250 µs | 0.519 µs | - | - | - | - | - |
| point `INSERT`, prepared | 5.644 µs | 5.563 - 8.738 µs | - | - | - | 0.080 µs | 0.082 µs | - | - |
| point `DELETE`, text | 6.312 µs | 6.283 - 6.354 µs | 0.206 µs | 0.681 µs | - | - | - | - | - |
| point `DELETE`, prepared | 5.460 µs | 5.421 - 5.500 µs | - | - | - | 0.057 µs | 0.069 µs | - | - |
| three-relation join, text | 10.574 µs | 10.380 - 11.254 µs | 0.870 µs | 1.933 µs | - | - | - | - | 1.410 µs |
| three-relation join, prepared | 7.696 µs | 7.556 - 8.503 µs | - | - | - | 0.054 µs | 0.138 µs | - | 1.362 µs |
| query-layer `SELECT v * 2`, text | 9.834 µs | 9.741 - 10.604 µs | 0.297 µs | 1.593 µs (3 calls) | - | - | - | - | 0.860 µs (2 calls) |
| `ns1.t2`, text | 7.083 µs | 7.058 - 9.284 µs | 0.285 µs | 0.877 µs | - | - | - | 0.415 µs | 0.870 µs |
| `t2` under `USE ns1`, text | 6.604 µs | 6.459 - 6.949 µs | 0.249 µs | 0.822 µs | - | - | - | 0.001 µs | 0.454 µs |
| `ns1.t2`, one view, text | 7.467 µs | 7.428 - 7.513 µs | 0.285 µs | 0.874 µs | - | - | - | 0.765 µs (2 calls) | 0.785 µs |
| `t2` under `USE`, one view, text | 6.572 µs | 6.533 - 6.649 µs | 0.248 µs | 0.816 µs | - | - | - | 0.001 µs | 0.439 µs |
| `ns1.t2`, 100 more relations, text | 9.472 µs | 9.441 - 9.501 µs | 0.288 µs | 0.877 µs | - | - | - | 2.757 µs (2 calls) | 1.751 µs |
| `t2` under `USE`, 100 more relations, text | 6.515 µs | 6.490 - 6.545 µs | 0.248 µs | 0.814 µs | - | - | - | 0.002 µs | 0.434 µs |
| join under a two-variant plan, text | 11.627 µs | 11.537 - 12.565 µs | 0.875 µs | 1.953 µs | 0.663 µs | - | - | - | 2.290 µs |
| join under a two-variant plan, prepared | 7.993 µs | 7.982 - 8.086 µs | - | - | - | 0.054 µs | 0.137 µs | - | 1.526 µs |
| walked `UPDATE`, 60,000 rows, text | 121,362 µs | 121,284 - 122,853 µs | 1.402 µs | 20.02 µs (6 calls) | - | - | - | - | - |

`-` is a part not called on that path (under 0.0005 µs a statement). In
the walk's 10-statement blocks the closing `PING`'s own lex adds 0.1 call
to `fp`, `count` and `subst`. The
`named0` path, a zero-hole `KeptParse::Of` built and materialized at every
execution, costs 1.32 µs (`keptof`) a point `SELECT` at `C_PARSE`: parse
0.84 µs, `HolesDelimited` 0.17 µs, `ArgumentsOf` 0.18 µs and 0.13 µs of
its own. On the join it costs 3.44 µs: 1.99 µs, 0.65 µs, 0.66 µs and
0.14 µs. Against today's text path, with the parse taken as moved and
`FingerprintOf` as removed, it adds `holes`, `args`, its own work, `bind`
and `mat` and removes `fp`: +0.36 µs on a point `SELECT`, +0.33 to
+0.42 µs over the four point shapes and +0.76 µs on the join. Its server
CPU (9.71 µs on a point `SELECT`) includes a second round trip and is not
compared. Every other `named0` figure is in `s0.json`.

## 4. Each candidate against EM-Q1 (a)'s bar

| | removable cost (C1 net; C3, C4 gross) | of the shape's server CPU | bar (3 %) |
|---|---|---|---|
| **C1**, point `SELECT` | fp 0.245 less bind 0.049 and mat 0.081 = 0.115 µs | 1.8 % | not met |
| **C1**, point `UPDATE` | 0.252 - 0.068 - 0.089 = 0.095 µs | 1.4 % | not met |
| **C1**, point `INSERT` | 0.250 - 0.080 - 0.082 = 0.088 µs | 1.4 % | not met |
| **C1**, point `DELETE` | 0.206 - 0.057 - 0.069 = 0.080 µs | 1.3 % | not met |
| **C1**, three-relation join | 0.870 - 0.054 - 0.138 = 0.678 µs | 6.4 % | **met** |
| **C2**, walked `UPDATE` | one parse a resume, about 3.3 µs, against a slice of about 20 ms (121 ms over 6 parses, 5 resumes and the first) | 0.02 % | not met |
| **C3**, join under a two-variant plan | `ArgumentsOf` 0.663 µs, gross | 5.7 % | **met** |
| **C4**, `ns1.t2` | ns 0.415 µs, gross (the cell less `t2` under `USE`: 0.479 µs) | 5.9 % | **met** |
| **C4**, `ns1.t2`, one view | ns 0.765 µs, two scans (0.895 µs) | 10.2 % | **met** |
| **C4**, `ns1.t2`, the view and 100 more relations | ns 2.757 µs, two scans (2.957 µs) | 29.1 % | **met** |

C1's net figure takes the prepared path's bind and copy as the zero-hole
parse's, which `named0`'s bind (0.055 µs) and copy (0.072 µs) agree with
on the point `SELECT`. It does not count the work C1 would add at
`C_PARSE` beyond the parse it moves there. That work is the 0.13 µs of
`KeptParse::Of`'s own, and `HolesDelimited` and `ArgumentsOf` where they
are not skipped. A zero-hole path skips the last two by construction, and
G1 measures what is left. C3's figure is gross: G1 measures what
collecting the arguments during the parse costs instead.

## 5. Against this engine's earlier numbers

EA-S0 (`bench/v3.4.17/results-ea-s0-premise-v3.4.17-202-g481d101aa.md`,
at `481d101aa`) put the second lex at 0.23 µs on the point shapes and
0.86 µs on the join, and a text point `SELECT` at 6.39 µs. Here the
lex is 0.21 to 0.25 µs and 0.87 µs, and the `SELECT` 6.54 µs, about 2 %
dearer. None of the five scopes this spike adds runs on the text path
(§3), so the difference is not the spike's; it is not attributed. The
prepared point `SELECT`
is 5.62 µs here and 6.07 µs at EA-S0: EA's close removed its parse.

## 6. The repeat census for C5

`[source-read]` at `b46ac5140`. `tools/eqbench`, the scenario harness since
EO, sends every DML statement as a named statement with `?` holes
(`tools/eqbench/src/kwp.rs:5`, `query` and `exec`). Its text path, `run`,
carries only setup and DDL (`kwp.rs:458-466`). EA's kept parse therefore
already serves every statement the scenarios time, and no text DML
statement repeats in them, with identical literals or otherwise.
`sim/`'s corpus draws its literals from a seeded generator, so a repeat
there is chance, not a shape class. C5 has no shape class to serve, and
G0 drops it.

## 7. S0 (iv): a leading space and the refusal byte

`archive/.../s0iv.py` sends seven statements as text and as a named
statement, each with two leading spaces and without (output in
`s0iv.out`). Without the spaces, all seven name the same byte on both
paths. With them, six of the seven name a byte 2 higher when named,
because `KeptParse::Of` parses the untrimmed text and the dispatcher the
trimmed one. This predates EM, and EM-R1 keeps it. It is recorded in
`docs/inflight/bugs/kept-parse-refusal-byte-counts-leading-whitespace.md`,
and C1 parses the trimmed text so that an unnamed statement keeps the text
path's bytes.
