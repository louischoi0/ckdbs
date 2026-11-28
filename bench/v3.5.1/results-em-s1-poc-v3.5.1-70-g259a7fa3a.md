# EM-S1: the three spikes against the base, with an A2 floor

**Finding.** On worktree `em-front-end-reuse` from `259a7fa3a`
(`v3.5.1-70-g259a7fa3a`; the spikes applied to detached copies of
`b46ac5140`, whose engine source `259a7fa3a` does not change), with the A2
floor within ±0.4 % on every cell, G1 passes two of the three:

- **C4 passes.** The namespace map saves 0.40 µs of 6.94 µs on `ns1.t2`
  (5.7 %), and 0.77 µs of 7.21 µs with a view in the instance (10.6 %).
- **C3 passes.** Collecting a plan's arguments in the parse saves 0.67 µs
  of 11.64 µs on the join under a two-variant plan (5.7 %).
- **C4 and C3 together** cost nothing on the cells they do not serve that
  A's own spread separates. The lean is +0.04 µs (+0.6 % and +0.7 %) on the
  point `SELECT`, text and prepared, which is outside the floor (±0.1 %
  there) and inside A's spread (6.21 to 6.33 µs), and it is reported as a
  lean, per EM-R7.
- **C1 fails.** Over C4 and C3, the one parse at `C_PARSE` adds 0.35 to
  0.45 µs to every text point statement (+5.9 % to +7.2 % of A) and 0.41 µs
  to the query-layer `SELECT` (+4.4 %). It saves 0.08 µs on the join. What
  it adds is more than the 0.21 to 0.25 µs lex it removes: a parse at
  `C_PARSE` that collects its arguments and runs 0.10 to 0.14 µs dearer
  than the dispatcher's, and a kept parse's allocations and copies (the
  `KeptParse`, its arguments and exact text, the `BoundStatement`, the
  substituted text, `Materialize`). The kept failure carries its own text
  and status. C1 is not built (G1), and EM-R7 would refuse it anyway.

A first three-arm run measured C1 with the spike's one shared trim
(`parser::TrimmedText`) out of line. Every cell of that B arm read dearer,
prepared statements included (+0.11 to +0.22 µs). The second run made the
function inline and measured C4 with C3 as an arm of their own, which
separated C1's cost from the rest. Prepared statements still read 0.11 to
0.26 µs dearer with C1 than with C4 and C3 alone. That cost is not
attributed; C1 is dropped either way.

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-28; the four-arm run 11:59:35 to 12:00:20 UTC, the three-arm run 11:57:12 to 11:57:31 UTC |
| Branch / commit | `worktree-em-front-end-reuse` at `259a7fa3a`, `git describe --tags` = `v3.5.1-70-g259a7fa3a` (S0 landed, documents and a driver only). The arms are `b46ac5140` with spikes, built in this session |
| Arms | **A**: `b46ac5140` + EM-S0's counter spike, the S0 binary, `sha256` `dee5a37e...`; **A2**: a byte copy of A, the floor; **C43**: A + C4 + C3, `8631d269...`; **ALL**: A + C4 + C3 + C1, with `TrimmedText` inline, `777e2045...`; **B** (the first run only): ALL with `TrimmedText` out of line, `2d7aa40a...`. Full hashes in `archive/.../binaries.sha256`; the spike diffs in `spike-c3-c4.diff` and `spike-c1-c3-c4.diff` |
| Build | `build-release`, `CMAKE_BUILD_TYPE=Release`, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, each arm's own tree |
| Device | data under `/home/ukjin/em-s1-run/work`: `/dev/mapper/fedora-root`, xfs; not tmpfs |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536` (`tools/dk_close.py`'s `Server`), one fresh server an arm a round |
| Pinning | server on CPU 2, client on CPU 5, both P-cores |
| Host | load 3.24 / 2.48 / 2.19 before the four-arm run and 2.06 / 2.27 / 2.14 after; no `cc1plus` (`s1-four-arms.json` `meta`, every cell's `hosts`) |
| Driver | `tools/em_s1_ab.py --rounds 8 --k 2000` (four arms) and `--rounds 6` (three arms), new in this stage, over `tools/em_s0_premise.py`'s cells |

## 2. Server CPU per statement, the four-arm run (µs, medians of 8 rounds)

| cell | A | A's range | A2 vs A | C43 vs A | ALL vs A | ALL vs C43 (C1) |
|---|---|---|---|---|---|---|
| point `SELECT`, text | 6.273 µs | 6.21 - 6.33 µs | +0.1 % | +0.039 µs (+0.6 %) | +0.418 µs (+6.7 %) | +0.379 µs |
| point `SELECT`, prepared | 5.402 µs | 5.35 - 5.47 µs | -0.1 % | +0.040 µs (+0.7 %) | +0.146 µs (+2.7 %) | +0.106 µs |
| point `UPDATE`, text | 6.493 µs | 6.46 - 6.61 µs | +0.3 % | +0.019 µs (+0.3 %) | +0.468 µs (+7.2 %) | +0.449 µs |
| point `UPDATE`, prepared | 5.726 µs | 5.65 - 5.76 µs | -0.2 % | +0.011 µs (+0.2 %) | +0.273 µs (+4.8 %) | +0.262 µs |
| point `INSERT`, text | 5.987 µs | 5.92 - 6.15 µs | -0.4 % | -0.012 µs (-0.2 %) | +0.351 µs (+5.9 %) | +0.363 µs |
| point `DELETE`, text | 6.115 µs | 5.99 - 6.24 µs | -0.3 % | -0.039 µs (-0.6 %) | +0.373 µs (+6.1 %) | +0.412 µs |
| three-relation join, text | 10.593 µs | 10.51 - 10.77 µs | -0.2 % | -0.048 µs (-0.5 %) | -0.131 µs (-1.2 %) | -0.083 µs |
| three-relation join, prepared | 7.956 µs | 7.83 - 7.99 µs | -0.3 % | +0.034 µs (+0.4 %) | -0.137 µs (-1.7 %) | -0.171 µs |
| query-layer `SELECT v * 2`, text | 9.407 µs | 9.32 - 9.46 µs | -0.2 % | +0.034 µs (+0.4 %) | +0.445 µs (+4.7 %) | +0.411 µs |
| `ns1.t2`, text | 6.942 µs | 6.87 - 6.99 µs | +0.4 % | **-0.397 µs (-5.7 %)** | -0.070 µs (-1.0 %) | +0.327 µs |
| `t2` under `USE ns1`, text | 6.386 µs | 6.29 - 6.47 µs | -0.2 % | +0.033 µs (+0.5 %) | +0.384 µs (+6.0 %) | +0.351 µs |
| `ns1.t2`, one view, text | 7.205 µs | 7.12 - 7.26 µs | -0.0 % | **-0.767 µs (-10.6 %)** | -0.392 µs (-5.4 %) | +0.375 µs |
| join under a two-variant plan, text | 11.637 µs | 11.41 - 12.68 µs | +0.1 % | **-0.669 µs (-5.7 %)** | -0.258 µs (-2.2 %) | +0.411 µs |
| join under a two-variant plan, prepared | 8.647 µs | 8.14 - 8.93 µs | +0.2 % | +0.025 µs (+0.3 %) | -0.031 µs (-0.4 %) | -0.056 µs |

The spike's parts confirm each mechanism (`s1-four-arms.json`, per arm):
with C4 the `ns` scope is never entered on a qualified cell; with C3 the
plan's `args` scope is never entered; with C1 the text path's `fp` scope
is not entered and `bind` and `mat` are.

## 3. G1

| | equality harness (spike tree, Debug) | saving, outside the floor | bar (3 %) | G1 |
|---|---|---|---|---|
| C4 | `CatalogNamespaceCacheTest`, 5 cells, green; its first cell red against the spike with `MemoIsCurrent()` removed (core 1 answered the dropped relation's oid 4001, the scan 4005) | 0.40 µs and 0.77 µs | 5.7 %, 10.6 % | **passes** |
| C3 | `KeptParseTest.ACollectingParseGivesArgumentsOfItsText`, 218 statements, green; the dispatcher's debug cross-check against `ArgumentsOf` at every plan-governed compile | 0.67 µs | 5.7 % | **passes** |
| C1 | `KeptParseTest.AnUnnamedStatementsOneParseIsTheDispatchersParseOfItsText`, over 300 kept and over 30 declined texts, each with and without surrounding white space, green | none: +0.35 to +0.45 µs on every point shape, -0.08 µs on the join | - | **fails** |

The suite was not run on the spike trees as a whole. C4's and C3's
implementation stages (EM-S2a, EM-S2b) build the spike's C4 and C3 code
with these harness cells as tests, and the full suite gates each stage.

## 4. Against this engine's earlier numbers

A here is EM-S0's binary. Its text point `SELECT` is 6.27 µs, 6.54 µs at
EM-S0 (`results-em-s0-premise-v3.5.1-69-gb46ac5140.md`): a fresh server a
round here, one server for every round there. The two differ by more
than either run's spread, so only the in-run deltas above are compared.
