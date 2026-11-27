# EA-S0: what the parse of a prepared statement costs the server

**Finding.** On worktree `ea-prepared-statement-reuse` at `481d101aa`
(`v3.4.17-202-g481d101aa`, the spike below applied), **the parse is 11.9 % to
20.9 % of a prepared statement's server CPU on every shape**: 0.77 us of
6.07 us on a point `SELECT`, 0.76 us of 6.37 us on a point `UPDATE` and
1.93 us of 9.38 us on a three-relation join by key (medians of 8 rounds; the
range is every round's). **EA-Q1 (a)'s bar - under 3 % on every shape builds
the quote alone - is not met on any shape, so EA builds §2.1-§2.3.** With the
parse removed, `exec::Compile` would be 8.1 % to 8.2 % of what is left on the
point `SELECT`, nothing on the `UPDATE` (a write by its key compiles no chain)
and 18.9 % to 22.1 % on the join: EA-Q3's figure, for the operator, with
EA-Q3 (a) standing.

A second finding, outside EA: **a text statement over KWP lexes its text
twice**, once at `C_PARSE` for the informational `pattern_id`
(`kwp_session.cpp`, `FingerprintOf`) and once in its parse: 0.23 us on the
point shapes and 0.86 us on the join, 3.4 % to 8.1 % of a text statement's
server CPU. EA-R4 keeps the text path's instructions where they are, so the
lex stays; it is written into `known-gaps.md`.

## 1. Stamp

| | |
|---|---|
| Executed | 2026-11-27, 17:54:12 to 17:54:15 UTC (one server, one run) |
| Branch / commit | `worktree-ea-prepared-statement-reuse` at `481d101aa`, `git describe --tags` = `v3.4.17-202-g481d101aa`, plus the EA-S0 spike (§2); no EA code in the build |
| Build | `build-release`, `CMAKE_BUILD_TYPE=Release`, `-DKDS_BUILD_TESTS=OFF -DKDS_BUILD_SIM=OFF`, target `kds_server`, built in this session at this commit with the spike applied |
| Binary | a copy at `/home/ukjin/ea-s0-run/kds_server`, `sha256` `a7ab68e7c3bf3f6951cc86bef6526e9194add48efb1a9794ada0b1db69c03ee1` (`archive/.../binary.sha256`) |
| Device | data file and WAL under `/home/ukjin/ea-s0-run/work`: `/dev/mapper/fedora-root`, xfs (`df -T`); not tmpfs. A first run at 17:33 UTC had its data on the scratchpad's tmpfs, against rule 2, and is discarded; its figures agreed with these to 0.3 us |
| Server config | `cores = 1`, `durability = relaxed`, `buffer_pool_frames = 65536`, `log_level = warn`, every other key default (`tools/dk_close.py`'s `Server`) |
| Port | 15881 |
| Pinning | server on CPU 2 (`reactor_cpus` and every thread's affinity), client on CPU 5 |
| Host | load 1.44 / 2.61 / 3.63 at the first cell, 1.48 / 2.60 / 3.63 at the last; no `cc1plus` in any cell (each cell's `host` in `s0.json`) |
| Driver | `tools/ea_s0_premise.py --rounds 8 --k 2000 --n 10000` |
| Client | `tools/kwp.py`: `Connection.execute` for the text path (an unnamed `C_PARSE` of the text with its literal, `C_BIND` with no parameter, `C_EXECUTE`), `Connection.execute_prepared` for the prepared one (`C_BIND` of a named statement's `?` with `int64` values, `C_EXECUTE`) - the prepare path EA-Q2 (a) asks of `tools/kwp.py`, which it already had (BN-S1) |

## 2. The instrument

Server CPU per statement is `/proc/<pid>/task/*/schedstat` over a block of
2,000 statements, every server thread. The parts come from a spike that is
not in the source tree (`archive/.../ea_spike.hpp`, `apply_spike.py`): one
`steady_clock` scope at the head of `Parser::Parse`, `ParseQuery`,
`ParseConditions` (together "parse"), `FingerprintOf` ("fp"), `ArgumentsOf`
("args"), `kwp_session.cpp`'s `Substitute` ("subst") and `PlaceholderCount`
("count"), and `exec::Compile` ("compile"), an outer call timed once, summed
in ns and read back through `PING`'s reply before and after each block. The
spike's own cost is two clock reads a scope, about 40 ns; it is inside every
number here, parse included. "parse" is lex and parse together, since the
lexer runs inside the parse.

Each round runs, per shape, the text block and the prepared block, the order
alternating per round; 500 statements a path a shape warm the server first.
Keys are uniform over 10,000 rows of each relation, all BTREE.

## 3. Server CPU and its parts (us a statement, medians of 8 rounds)

| shape | path | server CPU (us) | parse (us) | fp (us) | subst (us) | count (us) | compile (us) | parse share (%), every round |
|---|---|---|---|---|---|---|---|---|
| point `SELECT` | text | 6.39 | 0.75 | 0.23 | 0.05 | 0.04 | 0.44 | 11.2 - 11.9 |
| point `SELECT` | prepared | 6.07 | 0.77 | 0.00 | 0.07 | 0.00 | 0.43 | 12.2 - 12.9 |
| point `UPDATE` | text | 6.71 | 0.75 | 0.23 | 0.06 | 0.04 | 0.00 | 11.1 - 11.2 |
| point `UPDATE` | prepared | 6.37 | 0.76 | 0.00 | 0.10 | 0.00 | 0.00 | 11.9 - 12.0 |
| three-relation join | text | 10.59 | 1.87 | 0.86 | 0.10 | 0.06 | 1.48 | 16.9 - 18.3 |
| three-relation join | prepared | 9.38 | 1.93 | 0.00 | 0.13 | 0.00 | 1.48 | 19.2 - 20.9 |

Server CPU ranged 5.96 - 6.36 us (point, prepared), 6.34 - 6.39 us (update,
prepared) and 9.20 - 9.99 us (join, prepared) over the rounds. Every part was
called once a statement on its path, `ArgumentsOf` never (no plan is
enabled), and the `UPDATE` compiles no chain.

The shapes: `SELECT * FROM t WHERE id = ?`; `UPDATE t SET w = ? WHERE id = ?`;
`SELECT a.v, b.v, cc.v FROM a JOIN b ON a.bid = b.id JOIN cc ON b.cid = cc.id
WHERE a.id = ?`.

## 4. Client latency (us, medians over 8 rounds of each block's percentile)

| shape | path | p0 | p25 | p50 | p95 | p99 |
|---|---|---|---|---|---|---|
| point `SELECT` | text | 17.4 | 17.9 | 18.0 | 19.0 | 19.9 |
| point `SELECT` | prepared | 16.6 | 17.1 | 17.3 | 18.2 | 19.2 |
| point `UPDATE` | text | 14.2 | 14.8 | 14.9 | 15.8 | 17.4 |
| point `UPDATE` | prepared | 13.8 | 14.4 | 14.5 | 15.4 | 16.9 |
| three-relation join | text | 21.2 | 21.8 | 22.0 | 23.2 | 24.4 |
| three-relation join | prepared | 19.9 | 20.5 | 20.7 | 21.8 | 23.1 |

No wait breakdown was taken: one connection on `relaxed`, nothing parks.

## 5. Against this engine's earlier numbers

BN's close put parse and compile together at 1.1 - 4.5 us, 5 - 15 % of a
point statement's server CPU (`bench/v3.0.0/archive/bn-close-v2.7.0-787-g8679367e/partsB.md`,
a v2.x number, set beside nothing here). CX-S6 put `exec::Compile` at 2.8 -
4.1 % of a plain shape's client p50 (`bench/v3.4.12/results-cx-s6-compile-share-v3.4.12-112-g9712f5f0.md`
§8). Here compile is 0.43 us of a point `SELECT`'s 17.3 us p50 (2.5 %) and
1.48 us of the join's 20.7 us (7.1 %), the same order.

## 6. The census: where a parameter's text is substituted, and where a statement's text is read again

`[source-read]` at `481d101aa`:

- **Substituted in one place**: `src/server/kwp_session.cpp`, `Substitute`
  (`C_BIND`), through `LiteralOf` per value. Nothing else in `src/` splices a
  value into statement text; the newline protocol has no parameters and the
  KWP load stream carries rows, not text.
- **The statement's own parse**, once a statement on each kind's path:
  `HandleSelect` (`run->parser`), and `ParseKeepingShape` for `INSERT`
  (`InsertInner`), `UPDATE` (`UpdateInner`) and `DELETE` (`DeleteInner`).
- **Read again from the text**: `WithReturning` parses the whole statement a
  second time for a write with `RETURNING`; `TryQueryLayer` parses it with the
  query grammar when the native parse or compile declines; a multi-variant
  administrator's plan lexes it for its arguments (`AdminPlan::Choose`,
  `parser::ArgumentsOf`); a SET value's scalar subquery runs its own sliced
  text (`Expr::subquery_sql`); a statement run again after a wait or a
  structural refusal is dispatched again from its text (`AwaitWriteBlock`,
  `StructuralRerunDue`); `LogStatement` and `RecordStatement` keep it.
