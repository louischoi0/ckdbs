# ca-harness: the reduced scenario quad

Runs scenario 1 (BTREE copy), 2, 3 and 4 against one `kds_server` binary and
config, then compares arms. Written for CA-S0 and reused for candidate binaries.

## Usage

    # one pass of one arm (copy the binary first and run from the copy)
    [SCEN="1 2 3 4"] [SAMPLER=so] bench/v3.2.0/archive/ca-harness/quad.sh BINARY ARM OUTDIR REP ['key = value' ...]

    # compare arms (first is the baseline); medians of reps, delta, spread
    python3 bench/v3.2.0/archive/ca-harness/compare.py OUTDIR base adv-off [cand ...]

Interleave arms by calling `quad.sh` as ABAB (`base` rep 1, `adv-off` rep 1, `base` rep 2, ...),
as `../ca-s0-run.sh` does. Output lands in `OUTDIR/ARM-rREP/`: `sN.json`, `sN.stdout`,
`sN.rc`, `sN.wall`, `sN.precheck` (loadavg and any build/ctest process), the `.conf` of each
server and its log.

## What it runs

| Scenario | Command | Server |
|---|---|---|
| s1 | `scenario1_backtest_btree_copy.py` (the BTREE copy of `tools/scenario1_backtest.py`; identical to a fresh 3-keyword `HEAP`->`BTREE` sed at a9f0f5cd), `--bars-clustered btree --verify --sync`, reduced by `S1_ARGS` | cores 8 |
| s2 | `tools/scenario2_freight.py` with `S2_ARGS` | cores 8 |
| s3 | `tools/scenario3_library.py` with `S3_ARGS` (`--index-mode all --server-indexes on --assert-index-reads`) | cores 8 |
| s4 | `tools/scenario4_cabinopt_days.py` with `S4_ARGS`, three servers (off/on/declared) | cores 1 each, `decay_half_life = 5`, `cabin_optimizer_snapshot_interval_ms = 500` |

Every server: `durability = group`, `log_level = warn`, `buffer_pool_frames = 65536`,
ports `PORT_BASE`+1..+3 and +41..+43 (default 15700), a fresh data file under
`DATA` (default `/home/cdkbs/bench-runs/bz/data`, ext4), deleted after the cell. Extra
`key = value` arguments are appended to every server's config.

Environment: `SCEN`, `TOOLS`, `DATA`, `PORT_BASE`, `S1_ARGS`..`S4_ARGS`, `SAMPLER`.

## Sampling

`SAMPLER=<kds_sampler.so>` (build: `gcc -O2 -shared -fPIC -o kds_sampler.so tools/kds_sampler.c`)
preloads the sampler into the server (s4: the `on` server only), brackets the driver with
SIGUSR1/SIGUSR2 and leaves `sN.dump.0` (s4: `s4-on.dump.0`) in the cell. Fold a dump into
categories with `python3 tools/kds_sampler_report.py DUMP --top 25 --parts bench/v3.2.0/archive/ca-harness/parts.json`. The server binary is not built with frame pointers, so only the interrupted
frame is symbolized reliably; the categories are leaf-function categories.

## Rules the caller keeps

Release binary, copied; `sha256sum` it; quiet host (`.precheck` records it, it does not
gate). Fresh data per cell is done here; the binary copy and the stamp are the caller's.
