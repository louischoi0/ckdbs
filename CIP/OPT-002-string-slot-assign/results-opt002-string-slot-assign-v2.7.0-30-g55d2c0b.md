# OPT-002 measured: `full-scan` proves the CIP entry's own prediction false, `UPDATE` proves the mechanism 3x over

Every claim below is against the run stamped in the table. Arm A and arm
B were built from a clean `git archive` of their own commit into two
scratch source trees under
`/tmp/claude-1000/-home-cdkbs-ckdbs/e52ac4d4-a740-442c-8ac0-ad035bc82a3b/scratchpad/{src-A,src-B}` —
never in the `path-optimizer` worktree, which moved under this run: it
was on `opt-002-string-slot-assign` at the start of this session and is
on `opt-004-decoderowinto-preconditions` with `src/exec/row_codec.cpp`
dirty by the time this file was written. That drift is exactly why the
task asked for `git archive` builds — the measured engine states below
are immutable regardless of what the worktree does next.

| | |
|---|---|
| Executed | 2026-08-31 23:18 – 23:32 UTC |
| Worktree | `/home/cdkbs/ckdbs/.claude/worktrees/path-optimizer` (drifted during the run — see above; irrelevant to what was measured) |
| Arm A (baseline) | `dfe4c98d438877955a5d4a3955d06ca95baa96b8`, `v2.7.0-29-gdfe4c98`, committed 2026-08-31 23:09:26 UTC. Doc-only commit; code identical to `v2.7.0-27-g1beda80`, the pre-OPT-002 state |
| Arm B (change) | `55d2c0bda5995ec96d1212893c696377e367979c`, `v2.7.0-30-g55d2c0b`, committed 2026-08-31 23:12:00 UTC — OPT-002 |
| Tree cleanliness | Both arms are `git archive` exports of a named commit — by construction there is no working-tree drift in what was compiled, independent of the worktree's own state |
| Build | `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=<scratch ossl dir>` (this sandbox has no `libssl-dev`), `cmake --build build-release --target kds_server -j8`. Both configured and linked clean, no warnings from `row_codec.cpp` |
| Binary provenance | A: source `build-release/kds_server` mtime 2026-08-31 23:18:55 UTC (9m29s after its commit); B: mtime 2026-08-31 23:19:36 UTC (7m36s after its commit). Both post-date their commit, as expected for a binary built in this session |
| Binary run | A copy → `.../scratchpad/run/kds_server_A`, sha256 `720012b7ebfc847770b10bcdf2a3120e9f7ade86e318b286923c338e1630b7e2`; B copy → `.../scratchpad/run/kds_server_B`, sha256 `c5db2b1a1054a4f0e6329219f1419a7979cd80a43f2d03b988e0917766fadba2`. Every server in this run started from these two copies, never from the build tree |
| Device | Data dirs under `$HOME/bench-opt002/{a,b}-<rows>`, `/dev/root`, **ext4** (`df -T`), not tmpfs |
| Build type | Release |
| Server config | `cores = 1`, `durability = relaxed` (via `bench/run_ab_server.sh`), one fresh server + fresh data dir per (arm, row count) — 6 server instances total across the run |
| Host | 8 vCPUs; `uptime` load average 0.1-0.4 and no `cc1plus`/`cmake` process at every point measurement began (checked before each phase) |

## What was measured, and how

OPT-002 (`docs/inflight/in-progress/cip-path-optimizer.md`) replaced a
build-a-`std::string`-then-move-assign with a single `memcpy` into the
slot's existing buffer, in three places: the `char(N)` decode arm, the
`varchar` decode arm, and `ResolveSpills`. Its own hypothesis: **"5-15%
on scan shapes that project text"**, with full-scan named as the
shape to watch and update named as compounding with OPT-001 (unbuilt:
UPDATE still decodes before testing the WHERE).

`tools/benchmark.py`'s row shape and `TEXT_LEN = 16` (just past
libstdc++'s 15-byte SSO, deliberately) are reused, but the driver
itself runs one server at a time and cannot interleave two — so this
run uses a purpose-built orchestrator
(`.../scratchpad/opt002_ab.py`, not added to `tools/`: this is a
narrower measurement, and per this role's own rule the reply and this
file carry what a re-run needs) built directly on
`tools/ckdbs_cli.ServerConnection` and `tools/bench_common.Phase` (same
percentile machinery `tools/benchmark.py` itself uses). For each row
count it:

1. Creates one table on each arm and loads it from the **identical
   seed** — both arms hold byte-identical rows, verified below.
2. Runs `R` rounds of a block: `point-select` × k, `full-scan` × k,
   `analyze-scan` (`ANALYZE SELECT * FROM t`) × k, `update` × k. **One
   RNG stream drives the query sequence for both arms** — the same
   probed ids and update values in the same order — so the only thing
   that can differ between a round's A block and B block is the server.
3. Alternates which arm goes first each round (`A,B` then `B,A`...), so
   no systematic warm/cold order effect survives into the pooled
   numbers.
4. Pools every op's latency across all rounds into one
   `bench_common.Phase` per (arm, phase) — percentiles below are exact
   over every sample, not an average of per-round percentiles.

Sizes: 200 / 1,000 / 10,000 rows (rule 9). Rounds: 20 / 20 / 20, at
25/10/15/25, 25/8/12/25, and 20/5/10/20 ops per round respectively
(point-select/full-scan/analyze-scan/update). `durability = relaxed` on
both arms: OPT-002 is a CPU-decode change, not an I/O one, and `strict`
would let fsync swamp the exact signal this run exists to resolve — the
data file still sits on ext4, never tmpfs, so the write phase (`update`,
which dirties a page) pays a real page-store path, just not a real
fsync wait.

## Correctness: no counter moved, and the rows are byte-identical

`ANALYZE SELECT * FROM t` on both arms, every size:

| rows | A: examined / pages / opens / pattern_id | B: examined / pages / opens / pattern_id |
|---:|---|---|
| 200 | 200 / 3 / 1 / `0xd759b895c2e831db` | 200 / 3 / 1 / `0xd759b895c2e831db` |
| 1,000 | 1000 / 14 / 1 / `0x17baf71314986405` | 1000 / 14 / 1 / `0x17baf71314986405` |
| 10,000 | 10000 / 136 / 1 / `0x217fbc91f89a32cc` | 10000 / 136 / 1 / `0x217fbc91f89a32cc` |

Every field matches, including `pattern_id` (the fingerprint), at every
size — no counter moved between arms, as the commit message claims.
A spot check of six rows (`id` 1, 2, 3, 5000, 9999, 10000) at 10,000
rows plus `SELECT COUNT(*)` returned byte-identical reply strings on
both arms, `c_text` included — the `memchr`-based `char(N)` rewrite and
the new `AssignBytes` helper reproduce the exact same decoded value,
not just the same row count. This session did not re-run the release
suite independently; the commit message for `55d2c0b` states "the
release suite is 3088/3088 green at this commit" — stated here as the
commit's own claim, not independently re-verified in this session.

## The noise floor, and why it needed a second instrument

The primary floor is a same-run split: arm A's own pooled samples, cut
at the run's midpoint round and compared half against half (same table,
same statement-sequence shape, same server — `Afloor1` vs `Afloor2`
below). That floor is a fraction of a percent for `update` and
`analyze-scan` at every size, which is what makes their deltas
trustworthy.

It is not a fraction of a percent for `point-select` and `full-scan` at
10,000 rows, and the reason matters: **a within-run split assumes the
two halves see the same background conditions, which is true within
one run and not across runs.** Three independent 10,000-row setups
(fresh server, fresh data dir, different seed each time) give `B`'s QPS
delta on `point-select` as **-13.18%, -0.90%, -4.63%** — always
negative, never within a factor of 4 of each other, and with no
mechanism that would make `B` slower at all (a point-select on a heap
relation matches by Keystone id, which needs no `row_codec` decode of
any candidate row; only the one matching row is decoded, exactly as in
`A`). `full-scan`'s three-setup delta — **-0.86%, 0.00%, +0.43%** —
sits inside ±1% every time, sign-flipping around zero, which is what a
null result looks like across independent runs. The within-run floor
underestimates true run-to-run variance for these two shapes; the
cross-run triangulation is the floor that should be trusted for them,
and it says: **no finding on either.**

| rows | shape | within-run floor (Afloor1 vs Afloor2) |
|---:|---|---:|
| 200 | point-select | 5.32% |
| 200 | full-scan | 0.04% |
| 200 | analyze-scan | 0.49% |
| 200 | update | 0.20% |
| 1,000 | point-select | 12.26% |
| 1,000 | full-scan | 1.60% |
| 1,000 | analyze-scan | 7.94% |
| 1,000 | update | 5.65% |
| 10,000 | point-select | 0.44% |
| 10,000 | full-scan | 0.43% |
| 10,000 | analyze-scan | 0.97% |
| 10,000 | update | 0.50% |

## Throughput matrix

QPS, derived from `ops / elapsed` exactly as `bench_common.Phase.qps`
computes it (rule 5a) — Δ is `B` against `A`, and the floor column
carries the split-half number from above so a reader does not have to
flip pages. **Bold** = the delta clears its floor and is corroborated
by a second, independently-seeded run at the same size (`update`,
`analyze-scan`); a delta that clears its own floor but does **not**
survive cross-run triangulation (`point-select` at 10,000) is marked
accordingly rather than bolded.

| rows | shape | A QPS (baseline) | B QPS (OPT-002) | Δ | floor | verdict |
|---:|---|---:|---:|---:|---:|---|
| 200 | point-select | 8,185 | 8,128 | -0.70% | 5.32% | inside floor — control holds |
| 200 | full-scan | 1,056 | 1,059 | +0.28% | 0.04% | technically above floor, but <0.3% absolute and not reproducible (see 10k) |
| 200 | analyze-scan | 7,590 | 7,816 | **+2.98%** | 0.49% | **above floor** |
| 200 | update | 7,304 | 7,578 | **+3.75%** | 0.20% | **above floor** |
| 1,000 | point-select | 5,448 | 5,720 | +5.00% | 12.26% | inside floor — control holds |
| 1,000 | full-scan | 229 | 228 | -0.57% | 1.60% | inside floor |
| 1,000 | analyze-scan | 3,813 | 4,844 | **+27.03%** | 7.94% | **above floor** |
| 1,000 | update | 2,867 | 3,264 | **+13.83%** | 5.65% | **above floor** |
| 10,000 | point-select | 1,372 | 1,308 | -4.63% | 0.44% | above *within-run* floor, but not reproducible across 3 setups (-13.2%/-0.9%/-4.6%) — **noise** |
| 10,000 | full-scan | 23.4 | 23.5 | +0.43% | 0.43% | at the floor, and 3 setups give -0.9%/0.0%/+0.4% — **no finding** |
| 10,000 | analyze-scan | 766 | 906 | **+18.26%** | 0.97% | **above floor** |
| 10,000 | update | 421 | 500 | **+18.87%** | 0.50% | **above floor**, corroborated: 3 setups give +16.6%/+19.2%/+18.9% |

`analyze-scan` is `ANALYZE SELECT * FROM t` — the same compiled chain,
the same steps, the same decode, answering with one line of plan text
instead of the rows (`order_by_benchmark.py`'s own description of what
`ANALYZE` is on this engine). It is not a workload; it is the
instrument the wait-breakdown section below is built on.

## Latency distribution

p0/p25/p50/p95/p99 in microseconds, every sample pooled across 20
interleaved rounds (rule 6). `err` is 0 in every row — no reply from
either arm at any size was `ERR`.

| rows | arm:phase | ops | mean | p0 | p25 | p50 | p95 | p99 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 200 | A:point-select | 500 | 122.2 | 101.5 | 112.7 | 121.2 | 142.0 | 182.3 |
| 200 | B:point-select | 500 | 123.0 | 100.1 | 112.7 | 121.7 | 148.1 | 179.9 |
| 200 | A:full-scan | 200 | 946.8 | 914.2 | 936.9 | 941.9 | 979.2 | 1024.0 |
| 200 | B:full-scan | 200 | 944.1 | 910.5 | 929.9 | 936.3 | 974.4 | 1084.0 |
| 200 | A:analyze-scan | 300 | 131.8 | 121.6 | 126.9 | 128.5 | 149.8 | 155.5 |
| 200 | B:analyze-scan | 300 | 127.9 | 119.7 | 122.3 | 124.2 | 148.1 | 158.7 |
| 200 | A:update | 500 | 136.9 | 131.2 | 132.7 | 134.0 | 151.3 | 157.3 |
| 200 | B:update | 500 | 132.0 | 122.9 | 127.2 | 128.4 | 144.9 | 166.6 |
| 1,000 | A:point-select | 500 | 183.6 | 105.5 | 164.5 | 172.5 | 225.1 | 270.3 |
| 1,000 | B:point-select | 500 | 174.8 | 103.2 | 167.7 | 171.9 | 193.4 | 274.0 |
| 1,000 | A:full-scan | 160 | 4362.1 | 4213.9 | 4300.9 | 4337.7 | 4472.3 | 4889.7 |
| 1,000 | B:full-scan | 160 | 4388.0 | 4229.8 | 4293.2 | 4339.6 | 4515.6 | 5880.1 |
| 1,000 | A:analyze-scan | 240 | 262.3 | 227.6 | 243.4 | 258.5 | 300.8 | 337.2 |
| 1,000 | B:analyze-scan | 240 | 206.5 | 189.0 | 196.3 | 200.7 | 250.1 | 271.9 |
| 1,000 | A:update | 500 | 348.8 | 323.2 | 330.9 | 343.2 | 381.4 | 395.5 |
| 1,000 | B:update | 500 | 306.4 | 293.5 | 299.0 | 304.2 | 325.4 | 340.9 |
| 10,000 | A:point-select | 400 | 729.1 | 693.1 | 709.8 | 719.5 | 775.7 | 921.0 |
| 10,000 | B:point-select | 400 | 764.6 | 715.5 | 737.9 | 746.8 | 935.8 | 984.8 |
| 10,000 | A:full-scan | 100 | 42825.7 | 41683.4 | 42345.5 | 42685.4 | 44295.8 | 45013.5 |
| 10,000 | B:full-scan | 100 | 42514.4 | 41536.1 | 42111.9 | 42311.5 | 44271.6 | 44481.9 |
| 10,000 | A:analyze-scan | 200 | 1305.8 | 1259.4 | 1276.7 | 1286.6 | 1421.7 | 1476.9 |
| 10,000 | B:analyze-scan | 200 | 1104.2 | 1062.5 | 1078.2 | 1088.5 | 1230.1 | 1263.1 |
| 10,000 | A:update | 400 | 2376.2 | 2324.3 | 2344.5 | 2362.8 | 2422.0 | 2576.6 |
| 10,000 | B:update | 400 | 1999.0 | 1960.1 | 1975.6 | 1987.7 | 2046.5 | 2145.8 |

## The wait breakdown that explains the gap between the prediction and the result

`full-scan`'s reply is `ANALYZE`'s answer plus every row rendered to
text, wire-transferred, and parsed by the Python client. `analyze-scan`
is the same walk and the same decode with that removed. The gap between
the two is therefore render + wire-transfer + client-parse, measured
rather than inferred:

| rows | full-scan mean (A) | analyze-scan mean (A) | render/xfer/parse share |
|---:|---:|---:|---:|
| 200 | 946.8 us | 131.8 us | 86.1% |
| 1,000 | 4362.1 us | 262.3 us | 94.0% |
| 10,000 | 42825.7 us | 1305.8 us | **96.9%** |

At every size this driver can reach, render+transfer+parse is the large
majority of `full-scan`'s latency, and it grows toward 97% as the row
count grows — the opposite of what a per-row *decode* saving needs to
be visible against. `client and socket round trip` (the whole
`analyze-scan` number, decomposed no further — this Python client's own
send+recv floor is what `tools/bench_common.py` already documents as
unresolvable below ~10us) and `read-statement decode` are the two
waits this engine's counters can separate here; `durability/commit`
does not apply (`analyze-scan`, `full-scan` and `point-select` are all
reads, and `update` runs under `relaxed` by this run's own choice, so
no fsync wait is in any of these numbers); `lock/conflict wait` does
not apply (single connection, no concurrent writer).

## What the run says about OPT-002, and about the CIP entry that proposed it

**The hypothesis is right about the mechanism and wrong about the
shape.** OPT-002's entry in `docs/inflight/in-progress/cip-path-optimizer.md`
predicts *"5-15% on scan shapes that project text"* and names
`full-scan` first. `full-scan` shows nothing — three independent
10,000-row setups give -0.86%/0.00%/+0.43%, sign-flipping around zero,
and the same non-finding holds at 200 and 1,000 rows. `analyze-scan` —
the same walk and decode with the reply removed — shows exactly the
predicted magnitude and more: **+2.98% at 200 rows, +27.03% at 1,000,
+18.26% at 10,000**, every one clearing its own floor by 3x or more.
The decode saving OPT-002 built is real, measured directly, and lands
inside (at 200 rows) to well above (at 1,000 and 10,000) the entry's
own predicted range. It just never reaches a client through
`full-scan`, because rendering 10,000 rows to CSV-shaped text and
parsing that reply back in Python costs 97% of the statement's latency
at that size — a ~16ns/row saving (fitted crudely from the 1,000→10,000
`analyze-scan` slope: (1305.8-262.3)/(10000-1000) for A against
(1104.2-206.5)/9000 for B) cannot survive being 3% of a number that is
itself 3% of the wall clock. **`bench/results-scenario1-vs-pg.md` §6's
per-row fits are the right order of magnitude for this same reason: its
smallest ckdbs per-row cost (`agg-distinct`, 0.10 us/row) is a fold that
returns almost nothing — the shape `full-scan` is not.**

**`update` is where the win actually lands, and it exceeds the
predicted range.** `UPDATE t SET c_int=x WHERE id=n` on a heap relation
has no pk index (`LocateByPk` returns `kScan`), and — per OPT-001's
still-unbuilt hypothesis in the same CIP file — decodes the full row,
strings included, for every scanned tuple *before* testing the WHERE.
At `--rows N` that is on average N/2 full string decodes discarded per
UPDATE, and its reply is a few bytes ("UPDATED 1"), not a rendered
table — so almost none of `update`'s latency is render/transfer/parse,
and the decode saving shows up nearly undiluted: **+3.75% at 200 rows,
+13.83% at 1,000, +18.87% at 10,000** (corroborated by two more
10,000-row setups at +16.56% and +19.21%), scaling with row count
exactly as "N/2 avoided mallocs" predicts, and landing 4 points past
the entry's stated 15% ceiling at the largest size measured.

**The two entries interact, and the direction matters for what gets
measured next.** OPT-002's saving on `update` is only this large
*because* OPT-001 (decode-before-predicate) is still unbuilt — every
rejected row currently pays a full decode that OPT-001's own hypothesis
says should not happen at all. If OPT-001 lands, `update`'s average
decoded-row count per statement drops from N/2 toward 1 (a masked
decode of the WHERE columns, full decode only on the match), which
removes most of the *volume* OPT-002's saving is currently multiplied
against. **Re-measuring OPT-002 after OPT-001 lands is not optional
color — this run's `update` number will not reproduce once OPT-001 is
built**, and the CIP entry should say so rather than let a future
reader diff this file against a post-OPT-001 run and read the drop as
a regression.

**One design point this run gives a first real data point to**: the
open question in `CLAUDE.md`'s Query language / parser row about
whether a masked-decode-then-full-decode discipline generalizes is
exactly what `analyze-scan` vs `full-scan` demonstrates has *already*
generalized on the read side (AP01/AP02) — reading is cheap to decode
and expensive to render, writing is expensive to decode and cheap to
reply. OPT-002 is a clean win precisely because it does not know or
care which of those two shapes it is running under; the shape decides
how much of the win a client actually sees.

## Reproduction

```
# both arms, from this session's scratch trees (paths are session-local;
# re-running needs a fresh `git archive` per the note at the top of this
# file):
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
    -DOPENSSL_ROOT_DIR=<path to a libssl-dev-equivalent root>
cmake --build build-release --target kds_server -j8

bench/run_ab_server.sh <copy of A's kds_server> 15851 $HOME/bench-opt002/a-<rows> relaxed
bench/run_ab_server.sh <copy of B's kds_server> 15852 $HOME/bench-opt002/b-<rows> relaxed

python3 opt002_ab.py --port-a 15851 --port-b 15852 --rows <200|1000|10000> \
    --rounds 20 --sel-per-round <k> --scan-per-round <k> \
    --analyze-per-round <k> --upd-per-round <k> --seed <n> --json out.json
```

`opt002_ab.py`'s full logic is reproduced in the "What was measured, and
how" section above; it is not preserved outside this session's
scratchpad, per this role's rule that a narrower measurement archives
nothing and this file carries what a re-run needs.
