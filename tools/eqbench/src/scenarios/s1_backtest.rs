//! Scenario 1, the backtest: 30 years of daily bars, their derived
//! features, eight models walked forward over them, and every read shape
//! that workload issues priced on its own. Ported from
//! `tools/scenario1_backtest.py` and its twin `tools/pg_scenario1_backtest.py`;
//! the phase names are theirs.
//!
//! ## Relation kinds on KDS (`bench/README.md`, "What a relation is stored as")
//!
//! | relation | kind | the statements that decide it |
//! |---|---|---|
//! | `s1_exchanges` | BTREE | `read-join-point` reaches a row by its key (`e.id = s.exchange_id`) |
//! | `s1_symbols` | BTREE | `backtest-read` and `read-join-point` reach a row by its key (`s.id = ...`) |
//! | `s1_sessions` | HEAP | only appended: no statement reads it (the Python reads it only under `--fk`, not ported) |
//! | `s1_daily_bars` | BTREE | `read-bar-lookup` (key point), `read-bar-range` (key range), `backtest-read` and `read-join-point` (a key probe per row) |
//! | `s1_daily_stats` | HEAP | appended, then walked: `WHERE session_no = ?` / `WHERE symbol_id = ?`, no key access, no index |
//! | `s1_models` | BTREE | `compare-all` and `compare-one` probe it by key (`m.id = r.model_id`) |
//! | `s1_model_results` | HEAP | appended, then walked: `compare-all` whole, `compare-one` by `model_id`, no key access, no index |
//! | `s1_write_probe` | HEAP | the write sweep only appends to it |
//!
//! The Python's `--bars-clustered btree|heap` is not offered: `daily_bars`
//! is BTREE by the rule. The Python creates no secondary index on KDS, so
//! none is created here on any engine. PostgreSQL and Oracle store every
//! relation as their own heap table with the first column the primary key
//! (`create_table`).
//!
//! ## Phases, in order
//!
//! `load-exchanges`, `load-symbols`, `load-models` (autocommit rows),
//! `load-sessions`, `load-bars`, `load-stats` (`batch` rows a transaction;
//! a row's latency is its `INSERT`, the `BEGIN`/`COMMIT` in the wall clock,
//! as the Python times them); `backtest-read` (the 3-relation cross-section
//! join, one per rebalance), `result-insert` (8 autocommit rows a
//! rebalance), `backtest-replay`, `compare-all`, `compare-one`;
//! `read-bar-lookup`, `read-bar-range`, `read-symbol-history`,
//! `read-day-slice`, `read-join-point`, `read-join-exists`; `agg-global`,
//! `agg-by-symbol`, `agg-by-session`, `agg-day-slice`, `agg-distinct`; the
//! QPS matrix as `qps-<shape>-cold` and `qps-<shape>-warm` over the
//! Python's seven shapes; the write sweep as `write-batch-<n>`; the
//! connection sweep as `join-conn-<n>`, one terminal a connection.
//!
//! Every phase of reads is preceded by `warmup` unrecorded executions of
//! its statements; a cold cell's warm-up uses one argument it reserves, so
//! no cold argument is seen twice.
//!
//! ## What differs from the Python, and why
//!
//! - Every key is supplied by the client (1..n in load order: bars and
//!   their feature rows symbol-major, as the Python's per-symbol load
//!   issued them), so no reply has to carry an issued id back and every
//!   engine stores the same keys.
//! - `backtest-read` and `result-insert` are two phases run one after the
//!   other, where the Python interleaves them: all cross-sections are read,
//!   then the models run client-side, then the result rows are written. The
//!   statements, their arguments and the rows written are the Python's; a
//!   phase is one recorder, which is what keeps each phase's server CPU and
//!   breakdown its own.
//! - Each cross-section is sorted by `symbol_id` client-side before a
//!   model reads it (outside the timed region), so ties in a ranking and
//!   `vol-break`'s arrival order fall the same on every engine; the model
//!   totals in `meta.model_equity_bp` are therefore equal across engines.
//! - The prices are a geometric walk of this harness's own seeded
//!   generator, not Python's Mersenne Twister: the same on every engine,
//!   not the Python's bytes.
//! - The write sweep's batch 1 is autocommit (the Python label's meaning;
//!   its code sent `BEGIN`/`COMMIT` around one row), and a row's latency
//!   there includes the `BEGIN` and `COMMIT` it opens or closes, so the
//!   mean is the Python's elapsed over rows.
//! - `s1_model_results` is given its statistics refresh (`analyze`) after
//!   `result-insert`, besides every relation's after the load; the twin
//!   analysed it empty.
//!
//! ## Left out
//!
//! - The QPS matrix's `cabin` and `dropped` cases (and PostgreSQL's `index`
//!   case): KDS-only, or a structure the other engines would build
//!   differently; `cold` and `warm` are ported.
//! - `--cabin`, `--fk`, `--bars-clustered`, `--analyze` (KDS's `ANALYZE`
//!   plans), `--show-models` (the ranking table; the per-model totals are
//!   in the JSON's meta), `--echo`, `--sync`, `--server-log`,
//!   `--no-load-txn` (`--set batch=0` gives it).
//!
//! ## Options (`--set k=v`), defaults the PostgreSQL control's `pg-s1`
//!
//! `bars` (252, 1008, 10080 or 60480: the control's four sizes, setting
//! `years`/`symbols`/`exchanges`), `years` 30, `symbols` 8, `exchanges` 2,
//! `start_year` 1995, `rebalance` 21, `top_k` 3, `batch` 200, `ops` 200,
//! `replay` 1, `compare_rounds` 4, `qps_ops` 100, `warm_keys` 8,
//! `write_batches` "1,10,100,1000", `write_ops` 2000, `connections`
//! "1,2,4,8", `conn_ops` 200, `warmup` 24.

use super::*;
use std::collections::{BTreeMap, BTreeSet};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

const SESSIONS_PER_YEAR: i64 = 252;
const FULL_WEIGHT_BP: i64 = 10_000;
const SECTORS: i64 = 8;
const COUNTRIES: [&str; 8] = ["KR", "US", "JP", "GB", "DE", "SG", "HK", "AU"];
const EXCHANGE_CODES: [&str; 8] = ["XKRX", "XNYS", "XTKS", "XLON", "XETR", "XSES", "XHKG", "XASX"];

const FAMILY_MOMENTUM: i64 = 0;
const FAMILY_VOLATILITY: i64 = 1;

/// name, family, lookback, param_bp - the Python's `MODELS`.
const MODELS: [(&str, i64, i64, i64); 8] = [
    ("mom-5", FAMILY_MOMENTUM, 5, 0),
    ("mom-20", FAMILY_MOMENTUM, 20, 0),
    ("mom-60", FAMILY_MOMENTUM, 60, 0),
    ("mom-120", FAMILY_MOMENTUM, 120, 0),
    ("vol-low", FAMILY_VOLATILITY, 20, 0),
    ("vol-high", FAMILY_VOLATILITY, 20, 0),
    ("vol-tgt", FAMILY_VOLATILITY, 60, 0),
    ("vol-break", FAMILY_VOLATILITY, 10, 20000),
];

/// Indices into a feature row: the stored columns of `daily_stats` after
/// its three keys, in column order.
const F_RET: usize = 0;
const F_MOM: [(i64, usize); 4] = [(5, 1), (20, 2), (60, 3), (120, 4)];
const F_VOL: [(i64, usize); 3] = [(10, 5), (20, 6), (60, 7)];
const NFEAT: usize = 8;

fn col_of(table: &[(i64, usize)], lookback: i64) -> usize {
    table.iter().find(|(l, _)| *l == lookback).map(|(_, c)| *c).unwrap_or_else(|| panic!("no window {lookback}"))
}

// ---- the schema ---------------------------------------------------------------

const T_EXCHANGES: Table = Table {
    name: "s1_exchanges",
    cols: &[("id", Ty::I64), ("code", Ty::Str(8)), ("country", Ty::Str(4)), ("tz_min", Ty::I32)],
    store: Store::Btree,
    refs: &[],
};
const T_SYMBOLS: Table = Table {
    name: "s1_symbols",
    cols: &[("id", Ty::I64), ("ticker", Ty::Str(16)), ("exchange_id", Ty::I64), ("sector", Ty::I32), ("listed_session", Ty::I32)],
    store: Store::Btree,
    refs: &[],
};
const T_SESSIONS: Table = Table {
    name: "s1_sessions",
    cols: &[("id", Ty::I64), ("session_no", Ty::I32), ("year", Ty::I32), ("month", Ty::I32), ("dow", Ty::I32)],
    store: Store::Heap,
    refs: &[],
};
const T_BARS: Table = Table {
    name: "s1_daily_bars",
    cols: &[
        ("id", Ty::I64),
        ("symbol_id", Ty::I64),
        ("session_id", Ty::I64),
        ("session_no", Ty::I32),
        ("open", Ty::I64),
        ("high", Ty::I64),
        ("low", Ty::I64),
        ("close", Ty::I64),
        ("volume", Ty::I64),
    ],
    store: Store::Btree,
    refs: &[],
};
const T_STATS: Table = Table {
    name: "s1_daily_stats",
    cols: &[
        ("id", Ty::I64),
        ("bar_id", Ty::I64),
        ("symbol_id", Ty::I64),
        ("session_no", Ty::I32),
        ("ret_bp", Ty::I64),
        ("mom5_bp", Ty::I64),
        ("mom20_bp", Ty::I64),
        ("mom60_bp", Ty::I64),
        ("mom120_bp", Ty::I64),
        ("vol10_bp", Ty::I64),
        ("vol20_bp", Ty::I64),
        ("vol60_bp", Ty::I64),
    ],
    store: Store::Heap,
    refs: &[],
};
const T_MODELS: Table = Table {
    name: "s1_models",
    cols: &[("id", Ty::I64), ("name", Ty::Str(16)), ("family", Ty::I32), ("lookback", Ty::I32), ("top_k", Ty::I32), ("param_bp", Ty::I64)],
    store: Store::Btree,
    refs: &[],
};
const T_RESULTS: Table = Table {
    name: "s1_model_results",
    cols: &[
        ("id", Ty::I64),
        ("model_id", Ty::I64),
        ("period_no", Ty::I32),
        ("session_no", Ty::I32),
        ("pnl_bp", Ty::I64),
        ("equity_bp", Ty::I64),
        ("positions", Ty::I32),
        ("trades", Ty::I32),
    ],
    store: Store::Heap,
    refs: &[],
};
const T_PROBE: Table = Table {
    name: "s1_write_probe",
    cols: &[("id", Ty::I64), ("a", Ty::I64), ("b", Ty::I64), ("c", Ty::I64), ("d", Ty::I64)],
    store: Store::Heap,
    refs: &[],
};
const TABLES: [&Table; 8] = [&T_EXCHANGES, &T_SYMBOLS, &T_SESSIONS, &T_BARS, &T_STATS, &T_MODELS, &T_RESULTS, &T_PROBE];

/// A table alias: `t AS a` where the engine takes it, `t a` on Oracle,
/// which refuses `AS` before a table alias.
fn al(k: Kind, table: &str, alias: &str) -> String {
    match k {
        Kind::Ora => format!("{table} {alias}"),
        _ => format!("{table} AS {alias}"),
    }
}

// ---- the generated data -------------------------------------------------------

fn unit(rng: &mut Rng) -> f64 {
    (rng.next() >> 11) as f64 / (1u64 << 53) as f64
}
fn uniform(rng: &mut Rng, lo: f64, hi: f64) -> f64 {
    lo + (hi - lo) * unit(rng)
}
/// Box-Muller, one draw a call.
fn gauss(rng: &mut Rng, mu: f64, sigma: f64) -> f64 {
    let u1 = 1.0 - unit(rng); // (0, 1]
    let u2 = unit(rng);
    mu + sigma * (-2.0 * u1.ln()).sqrt() * (2.0 * std::f64::consts::PI * u2).cos()
}

/// (now / before - 1) in basis points, rounded half to even as Python's
/// `round`.
fn basis_points(now: i64, before: i64) -> i64 {
    if before <= 0 {
        return 0;
    }
    (((now - before) as f64) * 10_000.0 / before as f64).round_ties_even() as i64
}

/// One symbol's closes in minor units: a geometric walk with a per-symbol
/// volatility and drift, floored at 100.
fn generate_series(sessions: usize, rng: &mut Rng) -> Vec<i64> {
    let mut price = rng.range(5_000, 500_000) as f64;
    let daily_vol = uniform(rng, 0.006, 0.035);
    let drift = gauss(rng, 0.0002, 0.0004);
    (0..sessions)
        .map(|_| {
            price *= (drift + gauss(rng, 0.0, daily_vol)).exp();
            (price as i64).max(100)
        })
        .collect()
}

/// The feature rows of one series: return, momentum at four lookbacks,
/// population volatility of the returns at three windows; a window not yet
/// filled is 0.
fn derive_stats(closes: &[i64]) -> Vec<[i64; NFEAT]> {
    let mut rets = vec![0i64];
    for i in 1..closes.len() {
        rets.push(basis_points(closes[i], closes[i - 1]));
    }
    let mom = |i: usize, w: usize| if i >= w { basis_points(closes[i], closes[i - w]) } else { 0 };
    let vol = |i: usize, w: usize| {
        if i < w {
            return 0;
        }
        let win = &rets[i + 1 - w..=i];
        let mean = win.iter().sum::<i64>() as f64 / w as f64;
        let var = win.iter().map(|&x| (x as f64 - mean).powi(2)).sum::<f64>() / w as f64;
        var.sqrt().round_ties_even() as i64
    };
    (0..closes.len())
        .map(|i| {
            let mut f = [0i64; NFEAT];
            f[F_RET] = rets[i];
            for (w, c) in F_MOM {
                f[c] = mom(i, w as usize);
            }
            for (w, c) in F_VOL {
                f[c] = vol(i, w as usize);
            }
            f
        })
        .collect()
}

/// An open/high/low around a close.
fn ohlc_from(close: i64, ret_bp: i64, rng: &mut Rng) -> (i64, i64, i64) {
    let span = (close * 50i64.max(ret_bp.abs()) / 10_000).abs().max(1);
    let open = (close - ((close * ret_bp) as f64 / 10_000.0).round_ties_even() as i64).max(1);
    let high = open.max(close) + rng.range(0, span);
    let low = (open.min(close) - rng.range(0, span)).max(1);
    (open, high, low)
}

struct Data {
    exchanges: Vec<Vec<Val>>,
    symbols: Vec<Vec<Val>>,
    sessions: Vec<Vec<Val>>,
    bars: Vec<Vec<Val>>,
    stats: Vec<Vec<Val>>,
    models: Vec<Vec<Val>>,
    /// closes[symbol index][session_no]
    closes: Vec<Vec<i64>>,
    /// feats[symbol index][session_no]
    feats: Vec<Vec<[i64; NFEAT]>>,
    volumes_sum: i64,
}

impl Data {
    fn bar_id(&self, sym_idx: usize, session_no: usize) -> i64 {
        (sym_idx * self.sessions.len() + session_no + 1) as i64
    }
}

fn generate(seed: u64, years: i64, nsym: i64, nexch: i64, start_year: i64, top_k: i64) -> Data {
    let mut rng = Rng::new(seed);
    let nsess = (years * SESSIONS_PER_YEAR) as usize;
    let exchanges: Vec<Vec<Val>> = (0..nexch)
        .map(|i| {
            let n = EXCHANGE_CODES.len() as i64;
            let code = if i >= n { format!("{}{}", EXCHANGE_CODES[(i % n) as usize], i / n) } else { EXCHANGE_CODES[i as usize].to_string() };
            vec![Val::I64(i + 1), Val::Str(code), COUNTRIES[(i % 8) as usize].into(), Val::I64(rng.range(-12, 12) * 60)]
        })
        .collect();
    let symbols: Vec<Vec<Val>> = (0..nsym)
        .map(|i| vec![Val::I64(i + 1), Val::Str(format!("SYM{i:04}")), Val::I64(i % nexch + 1), Val::I64(i % SECTORS), Val::I64(0)])
        .collect();
    let sessions: Vec<Vec<Val>> = (0..nsess as i64)
        .map(|n| {
            let year = start_year + n / SESSIONS_PER_YEAR;
            let month = 1 + (n % SESSIONS_PER_YEAR) * 12 / SESSIONS_PER_YEAR;
            vec![Val::I64(n + 1), Val::I64(n), Val::I64(year), Val::I64(month), Val::I64(n % 5)]
        })
        .collect();
    let mut bars = Vec::with_capacity(nsess * nsym as usize);
    let mut stats = Vec::with_capacity(nsess * nsym as usize);
    let mut closes = Vec::new();
    let mut feats = Vec::new();
    let mut volumes_sum = 0i64;
    for s in 0..nsym as usize {
        let c = generate_series(nsess, &mut rng);
        let f = derive_stats(&c);
        for n in 0..nsess {
            let id = (s * nsess + n + 1) as i64;
            let (open, high, low) = ohlc_from(c[n], f[n][F_RET], &mut rng);
            let volume = rng.range(1_000, 50_000_000);
            volumes_sum += volume;
            bars.push(vec![
                Val::I64(id),
                Val::I64(s as i64 + 1),
                Val::I64(n as i64 + 1),
                Val::I64(n as i64),
                Val::I64(open),
                Val::I64(high),
                Val::I64(low),
                Val::I64(c[n]),
                Val::I64(volume),
            ]);
            let mut row = vec![Val::I64(id), Val::I64(id), Val::I64(s as i64 + 1), Val::I64(n as i64)];
            row.extend(f[n].iter().map(|&v| Val::I64(v)));
            stats.push(row);
        }
        closes.push(c);
        feats.push(f);
    }
    let models = MODELS
        .iter()
        .enumerate()
        .map(|(i, (name, family, lookback, param))| {
            vec![Val::I64(i as i64 + 1), (*name).into(), Val::I64(*family), Val::I64(*lookback), Val::I64(top_k), Val::I64(*param)]
        })
        .collect();
    Data { exchanges, symbols, sessions, bars, stats, models, closes, feats, volumes_sum }
}

// ---- the models ---------------------------------------------------------------

/// One cross-section row as a model reads it.
#[derive(Clone, Debug)]
struct XRow {
    symbol_id: i64,
    f: [i64; NFEAT],
    close: i64,
}

fn equal_weights(chosen: &[i64]) -> BTreeMap<i64, i64> {
    if chosen.is_empty() {
        return BTreeMap::new();
    }
    let each = FULL_WEIGHT_BP / chosen.len() as i64;
    chosen.iter().map(|&s| (s, each)).collect()
}

/// The Python's `positions_for`: {symbol_id: weight_bp}.
fn positions_for(name: &str, family: i64, lookback: i64, param_bp: i64, top_k: usize, xs: &[XRow]) -> BTreeMap<i64, i64> {
    if family == FAMILY_MOMENTUM {
        let key = col_of(&F_MOM, lookback);
        let mut ranked: Vec<&XRow> = xs.iter().filter(|r| r.f[key] > 0).collect();
        ranked.sort_by(|a, b| b.f[key].cmp(&a.f[key]));
        let chosen: Vec<i64> = ranked.iter().take(top_k).map(|r| r.symbol_id).collect();
        return equal_weights(&chosen);
    }
    let key = col_of(&F_VOL, lookback);
    match name {
        "vol-low" | "vol-high" => {
            let mut ranked: Vec<&XRow> = xs.iter().filter(|r| r.f[key] > 0).collect();
            if name == "vol-high" {
                ranked.sort_by(|a, b| b.f[key].cmp(&a.f[key]));
            } else {
                ranked.sort_by(|a, b| a.f[key].cmp(&b.f[key]));
            }
            let chosen: Vec<i64> = ranked.iter().take(top_k).map(|r| r.symbol_id).collect();
            equal_weights(&chosen)
        }
        "vol-tgt" => {
            let mom20 = col_of(&F_MOM, 20);
            let eligible: Vec<&XRow> = xs.iter().filter(|r| r.f[key] > 0 && r.f[mom20] > 0).collect();
            if eligible.is_empty() {
                return BTreeMap::new();
            }
            let inverse: Vec<(i64, f64)> = eligible.iter().map(|r| (r.symbol_id, 1.0 / r.f[key] as f64)).collect();
            let total: f64 = inverse.iter().map(|(_, v)| v).sum();
            inverse
                .into_iter()
                .map(|(s, share)| (s, (FULL_WEIGHT_BP as f64 * share / total) as i64))
                .filter(|(_, w)| *w > 0)
                .collect()
        }
        "vol-break" => {
            let chosen: Vec<i64> = xs
                .iter()
                .filter(|r| r.f[key] > 0 && r.f[F_RET] > 0 && r.f[F_RET] * 10_000 > param_bp * r.f[key])
                .map(|r| r.symbol_id)
                .take(top_k)
                .collect();
            equal_weights(&chosen)
        }
        _ => panic!("unknown volatility model {name}"),
    }
}

struct ModelState {
    id: i64,
    name: &'static str,
    family: i64,
    lookback: i64,
    param_bp: i64,
    positions: BTreeMap<i64, i64>,
    equity_bp: i64,
    periods: Vec<i64>,
    trades: i64,
}

/// The walk forward over the cross-sections read, in session order: marks
/// last period's positions to market from the closes read, appends one
/// result row a model (id, model_id, period_no, session_no, pnl_bp,
/// equity_bp, positions, trades), and picks the new positions.
fn walk_forward(xsecs: &[(i64, Vec<XRow>)], top_k: usize) -> (Vec<ModelState>, Vec<Vec<Val>>, i64) {
    let mut state: Vec<ModelState> = MODELS
        .iter()
        .enumerate()
        .map(|(i, (name, family, lookback, param_bp))| ModelState {
            id: i as i64 + 1,
            name,
            family: *family,
            lookback: *lookback,
            param_bp: *param_bp,
            positions: BTreeMap::new(),
            equity_bp: 0,
            periods: vec![],
            trades: 0,
        })
        .collect();
    let mut results = Vec::new();
    let mut previous: BTreeMap<i64, i64> = BTreeMap::new();
    let mut period_no = 0i64;
    for (step, (session_no, xs)) in xsecs.iter().enumerate() {
        let closes: BTreeMap<i64, i64> = xs.iter().map(|r| (r.symbol_id, r.close)).collect();
        if step > 0 {
            for m in state.iter_mut() {
                let mut pnl = 0i64;
                for (sym, w) in &m.positions {
                    if let (Some(&b), Some(&n)) = (previous.get(sym), closes.get(sym)) {
                        pnl += (w * basis_points(n, b)).div_euclid(10_000);
                    }
                }
                m.equity_bp += pnl;
                m.periods.push(pnl);
                results.push(vec![
                    Val::I64(results.len() as i64 + 1),
                    Val::I64(m.id),
                    Val::I64(period_no),
                    Val::I64(*session_no),
                    Val::I64(pnl),
                    Val::I64(m.equity_bp),
                    Val::I64(m.positions.len() as i64),
                    Val::I64(m.trades),
                ]);
            }
            period_no += 1;
        }
        for m in state.iter_mut() {
            let chosen = positions_for(m.name, m.family, m.lookback, m.param_bp, top_k, xs);
            let a: BTreeSet<i64> = chosen.keys().copied().collect();
            let b: BTreeSet<i64> = m.positions.keys().copied().collect();
            m.trades = a.symmetric_difference(&b).count() as i64;
            m.positions = chosen;
        }
        previous = closes;
    }
    (state, results, period_no)
}

// ---- helpers ------------------------------------------------------------------

static ERR_PRINTS: AtomicU32 = AtomicU32::new(0);

fn refused(r: &mut Recorder, e: &DbError) {
    let class = match e.class {
        ErrClass::Conflict => "conflict",
        ErrClass::Unique => "unique",
        ErrClass::Constraint => "constraint",
        ErrClass::Other => "other",
    };
    r.error(class);
    if ERR_PRINTS.fetch_add(1, Ordering::Relaxed) < 10 {
        eprintln!("s1: refused: {e}");
    }
}

/// One timed query; false on a refusal.
fn tq(e: &mut dyn Engine, r: &mut Recorder, s: Stmt, p: &[Val], rows: &mut Rows) -> bool {
    let t0 = Instant::now();
    match e.query(s, p, rows) {
        Ok(()) => {
            r.add(t0);
            r.rows += rows.len() as u64;
            true
        }
        Err(err) => {
            refused(r, &err);
            false
        }
    }
}

/// Unrecorded executions ahead of a phase.
fn warm(e: &mut dyn Engine, s: Stmt, args: &[Vec<Val>]) {
    let mut rows = Rows::new();
    for a in args {
        if let Err(err) = e.query(s, a, &mut rows) {
            eprintln!("s1: warm-up refused: {err}");
        }
    }
}

fn args1(v: impl IntoIterator<Item = i64>) -> Vec<Vec<Val>> {
    v.into_iter().map(|x| vec![Val::I64(x)]).collect()
}

/// A phase's problems, capped so one bad statement does not flood the
/// check's detail.
struct Problems(Mutex<Vec<String>>);
impl Problems {
    fn new() -> Problems {
        Problems(Mutex::new(Vec::new()))
    }
    fn push(&self, s: String) {
        let mut g = self.0.lock().unwrap();
        if g.len() < 20 {
            g.push(s);
        }
    }
    fn take(&self) -> Vec<String> {
        std::mem::take(&mut *self.0.lock().unwrap())
    }
}

/// A load phase: `rows` through one prepared `INSERT`, `batch` rows a
/// transaction (0: each row its own autocommit). A row's latency is its
/// `INSERT`; the `BEGIN`/`COMMIT` are in the wall clock.
fn load_phase(ctx: &mut Ctx, engines: &mut [Box<dyn Engine>], name: &str, ins: Stmt, rows: &[Vec<Val>], batch: usize) {
    ctx.phase(name, engines, |_, e, r| {
        let mut open = false;
        let mut n = 0;
        for row in rows {
            if batch > 0 && !open {
                e.begin().unwrap();
                open = true;
                n = 0;
            }
            let t0 = Instant::now();
            match e.exec(ins, row) {
                Ok(_) => r.add(t0),
                Err(err) => refused(r, &err),
            }
            n += 1;
            if open && n >= batch {
                e.commit().unwrap();
                open = false;
            }
        }
        if open {
            e.commit().unwrap();
        }
    });
}

fn int_list(s: &str) -> Vec<u64> {
    s.split(',').map(str::trim).filter(|p| !p.is_empty()).map(|p| p.parse().unwrap_or_else(|_| panic!("not a number: {p}"))).collect()
}

fn row_ints(r: &[Val]) -> Vec<i64> {
    r.iter().map(|v| v.i()).collect()
}

// ---- the scenario -------------------------------------------------------------

pub fn run(ctx: &mut Ctx) {
    let k = ctx.kind;
    let (years, nsym, nexch) = match ctx.opt("bars", 0) {
        0 => (ctx.opt("years", 30) as i64, ctx.opt("symbols", 8) as i64, ctx.opt("exchanges", 2) as i64),
        252 => (1, 1, 1),
        1008 => (1, 4, 2),
        10080 => (5, 8, 2),
        60480 => (30, 8, 2),
        b => panic!("--set bars={b}: one of 252, 1008, 10080, 60480 (or set years/symbols/exchanges)"),
    };
    assert!(years >= 1 && nsym >= 1 && nexch >= 1, "years, symbols and exchanges must be at least 1");
    let start_year = ctx.opt("start_year", 1995) as i64;
    let rebalance = ctx.opt("rebalance", 21).max(1) as usize;
    let top_k = ctx.opt("top_k", 3).max(1) as usize;
    let batch = ctx.opt("batch", 200) as usize;
    let ops = ctx.opt("ops", 200).max(1) as usize;
    let replay = ctx.opt("replay", 1) as usize;
    let compare_rounds = ctx.opt("compare_rounds", 4).max(1) as usize;
    let qps_ops = ctx.opt("qps_ops", 100).max(1) as usize;
    let warm_keys = ctx.opt("warm_keys", 8).max(1) as usize;
    let write_batches = int_list(&ctx.opt_s("write_batches", "1,10,100,1000"));
    let write_ops = ctx.opt("write_ops", 2000) as usize;
    let connections = int_list(&ctx.opt_s("connections", "1,2,4,8"));
    let conn_ops = ctx.opt("conn_ops", 200).max(1) as usize;
    let warmup = ctx.opt("warmup", 24) as usize;
    let scans = (ops / 20).max(1);

    let data = generate(ctx.seed, years, nsym, nexch, start_year, top_k as i64);
    let nsess = data.sessions.len();
    let nbars = data.bars.len() as i64;
    let symbol_ids: Vec<i64> = (1..=nsym).collect();
    let model_ids: Vec<i64> = (1..=MODELS.len() as i64).collect();
    let rebalance_sessions: Vec<i64> = (0..nsess).step_by(rebalance).map(|s| s as i64).collect();
    ctx.set_meta("years", years);
    ctx.set_meta("symbols", nsym);
    ctx.set_meta("exchanges", nexch);
    ctx.set_meta("sessions", nsess);
    ctx.set_meta("bars", nbars);
    ctx.set_meta("rebalance", rebalance);
    ctx.set_meta("top_k", top_k);
    ctx.set_meta("batch", batch);
    let kinds: BTreeMap<&str, Store> = TABLES.iter().map(|t| (t.name, t.store)).collect();
    ctx.set_meta("relation_kinds", kinds);

    // ---- schema
    let mut e = ctx.connect();
    for t in TABLES.iter().rev() {
        drop_table(e.as_mut(), t.name);
    }
    for t in TABLES {
        let sql = create_table(k, t);
        e.run(&sql).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
    let ins = |e: &mut Box<dyn Engine>, t: &Table| {
        let marks = vec!["?"; t.cols.len()].join(", ");
        e.prepare(&format!("INSERT INTO {} VALUES ({marks})", t.name)).unwrap()
    };
    let ins_exch = ins(&mut e, &T_EXCHANGES);
    let ins_sym = ins(&mut e, &T_SYMBOLS);
    let ins_sess = ins(&mut e, &T_SESSIONS);
    let ins_bar = ins(&mut e, &T_BARS);
    let ins_stat = ins(&mut e, &T_STATS);
    let ins_model = ins(&mut e, &T_MODELS);
    let ins_result = ins(&mut e, &T_RESULTS);
    let ins_probe = ins(&mut e, &T_PROBE);

    // ---- the read statements (the QPS matrix's shapes are these same statements)
    let xs_sql = format!(
        "SELECT t.symbol_id, t.ret_bp, t.mom5_bp, t.mom20_bp, t.mom60_bp, t.mom120_bp, t.vol10_bp, t.vol20_bp, t.vol60_bp, b.close, s.ticker \
         FROM {} JOIN {} ON t.bar_id = b.id JOIN {} ON t.symbol_id = s.id WHERE t.session_no = ?",
        al(k, "s1_daily_stats", "t"),
        al(k, "s1_daily_bars", "b"),
        al(k, "s1_symbols", "s")
    );
    let st_xs = e.prepare(&xs_sql).unwrap();
    let st_cmp_all = e
        .prepare(&format!(
            "SELECT r.model_id, r.period_no, r.pnl_bp, r.equity_bp, r.trades, m.name, m.family FROM {} JOIN {} ON r.model_id = m.id",
            al(k, "s1_model_results", "r"),
            al(k, "s1_models", "m")
        ))
        .unwrap();
    let st_cmp_one = e
        .prepare(&format!(
            "SELECT r.period_no, r.pnl_bp, r.equity_bp, r.trades, m.name FROM {} JOIN {} ON r.model_id = m.id WHERE r.model_id = ?",
            al(k, "s1_model_results", "r"),
            al(k, "s1_models", "m")
        ))
        .unwrap();
    let st_lookup = e.prepare("SELECT * FROM s1_daily_bars WHERE id = ?").unwrap();
    let st_range = e.prepare("SELECT * FROM s1_daily_bars WHERE id BETWEEN ? AND ?").unwrap();
    let st_sym_hist = e.prepare("SELECT * FROM s1_daily_stats WHERE symbol_id = ?").unwrap();
    let st_day_slice = e.prepare("SELECT * FROM s1_daily_stats WHERE session_no = ?").unwrap();
    let st_join_point = e
        .prepare(&format!(
            "SELECT b.close, s.ticker, e.code FROM {} JOIN {} ON b.symbol_id = s.id JOIN {} ON s.exchange_id = e.id WHERE b.id = ?",
            al(k, "s1_daily_bars", "b"),
            al(k, "s1_symbols", "s"),
            al(k, "s1_exchanges", "e")
        ))
        .unwrap();
    let st_exists = e
        .prepare(&format!(
            "SELECT s.ticker FROM {} WHERE EXISTS (SELECT b.id FROM {} WHERE b.symbol_id = s.id)",
            al(k, "s1_symbols", "s"),
            al(k, "s1_daily_bars", "b")
        ))
        .unwrap();
    let st_agg_global = e.prepare("SELECT COUNT(*), MIN(close), MAX(close), SUM(volume) FROM s1_daily_bars").unwrap();
    let st_agg_sym = e.prepare("SELECT symbol_id, COUNT(*), MIN(close), MAX(close) FROM s1_daily_bars GROUP BY symbol_id").unwrap();
    let st_agg_sess = e.prepare("SELECT session_no, COUNT(*), SUM(volume) FROM s1_daily_bars GROUP BY session_no").unwrap();
    let st_agg_day = e.prepare("SELECT symbol_id, COUNT(*), SUM(ret_bp) FROM s1_daily_stats WHERE session_no = ? GROUP BY symbol_id").unwrap();
    let st_agg_distinct = e.prepare("SELECT COUNT(DISTINCT symbol_id) FROM s1_daily_bars").unwrap();
    let st_counts: Vec<(&str, Stmt)> =
        TABLES.iter().map(|t| (t.name, e.prepare(&format!("SELECT COUNT(*) FROM {}", t.name)).unwrap())).collect();

    let mut engines = vec![e];

    // ---- the load
    load_phase(ctx, &mut engines, "load-exchanges", ins_exch, &data.exchanges, 0);
    load_phase(ctx, &mut engines, "load-symbols", ins_sym, &data.symbols, 0);
    load_phase(ctx, &mut engines, "load-sessions", ins_sess, &data.sessions, batch);
    load_phase(ctx, &mut engines, "load-bars", ins_bar, &data.bars, batch);
    load_phase(ctx, &mut engines, "load-stats", ins_stat, &data.stats, batch);
    load_phase(ctx, &mut engines, "load-models", ins_model, &data.models, 0);
    analyze(engines[0].as_mut(), &TABLES.iter().map(|t| t.name).collect::<Vec<_>>());

    // ---- the backtest: read every rebalance's cross-section
    let warm_sessions = |n: usize| -> Vec<Vec<Val>> { args1((0..n).map(|i| ((i * rebalance + rebalance / 2 + 1) % nsess) as i64)) };
    warm(engines[0].as_mut(), st_xs, &warm_sessions(warmup));
    let read_back: Mutex<Vec<(i64, Rows)>> = Mutex::new(Vec::new());
    ctx.phase("backtest-read", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for &s in &rebalance_sessions {
            if tq(e, r, st_xs, &[Val::I64(s)], &mut rows) {
                read_back.lock().unwrap().push((s, std::mem::take(&mut rows)));
            } else {
                read_back.lock().unwrap().push((s, Rows::new()));
            }
        }
    });
    // Each cross-section in symbol order, checked against what was generated.
    let mut xs_problems = Vec::new();
    let xsecs: Vec<(i64, Vec<XRow>)> = read_back
        .into_inner()
        .unwrap()
        .into_iter()
        .map(|(s, rows)| {
            let mut xs: Vec<XRow> = rows
                .iter()
                .map(|row| {
                    let v = row_ints(&row[..10]);
                    let mut f = [0i64; NFEAT];
                    f.copy_from_slice(&v[1..9]);
                    XRow { symbol_id: v[0], f, close: v[9] }
                })
                .collect();
            xs.sort_by_key(|x| x.symbol_id);
            if xs.len() != nsym as usize && xs_problems.len() < 20 {
                xs_problems.push(format!("session {s}: {} rows, {nsym} expected", xs.len()));
            }
            for x in &xs {
                let si = (x.symbol_id - 1) as usize;
                let ok = si < data.feats.len() && data.feats[si][s as usize] == x.f && data.closes[si][s as usize] == x.close;
                if !ok && xs_problems.len() < 20 {
                    xs_problems.push(format!("session {s} symbol {}: values differ from the generated row", x.symbol_id));
                }
            }
            (s, xs)
        })
        .collect();
    ctx.check(
        "cross-section-matches-generated",
        xs_problems.is_empty(),
        if xs_problems.is_empty() { format!("{} cross-sections, every row as generated", xsecs.len()) } else { xs_problems.join("; ") },
    );

    let (state, results, periods) = walk_forward(&xsecs, top_k);
    ctx.set_meta("periods", periods);
    ctx.set_meta("model_equity_bp", state.iter().map(|m| (m.name, m.equity_bp)).collect::<BTreeMap<_, _>>());

    load_phase(ctx, &mut engines, "result-insert", ins_result, &results, 0);
    analyze(engines[0].as_mut(), &["s1_model_results"]);

    // ---- the comparison
    if replay > 0 {
        warm(engines[0].as_mut(), st_xs, &warm_sessions(warmup));
        ctx.phase("backtest-replay", &mut engines, |_, e, r| {
            let mut rows = Rows::new();
            for _ in 0..replay {
                for &s in &rebalance_sessions {
                    tq(e, r, st_xs, &[Val::I64(s)], &mut rows);
                }
            }
        });
    }

    warm(engines[0].as_mut(), st_cmp_all, &vec![vec![]; warmup.min(8)]);
    let cmp_rows: Mutex<Rows> = Mutex::new(Rows::new());
    ctx.phase("compare-all", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        if tq(e, r, st_cmp_all, &[], &mut rows) {
            *cmp_rows.lock().unwrap() = rows;
        }
    });
    // The Python's verify: the P&L accumulated in memory against the P&L
    // read back through the comparison join.
    let mut by_model: BTreeMap<i64, (Vec<i64>, i64, i64)> = BTreeMap::new(); // pnl list, last period, equity
    for row in cmp_rows.into_inner().unwrap() {
        let v = row_ints(&row[..5]);
        let entry = by_model.entry(v[0]).or_insert((vec![], -1, 0));
        entry.0.push(v[2]);
        if v[1] > entry.1 {
            entry.1 = v[1];
            entry.2 = v[3];
        }
    }
    let mut problems = Vec::new();
    for m in &state {
        match by_model.get(&m.id) {
            None if !m.periods.is_empty() => problems.push(format!("{}: no rows came back from the comparison join", m.name)),
            None => {}
            Some((pnl, _, equity)) => {
                if *equity != m.equity_bp {
                    problems.push(format!("{}: equity {equity} bp read back, {} bp accumulated", m.name, m.equity_bp));
                }
                if pnl.len() != m.periods.len() {
                    problems.push(format!("{}: {} periods read back, {} written", m.name, pnl.len(), m.periods.len()));
                }
                if pnl.iter().sum::<i64>() != *equity {
                    problems.push(format!("{}: the periods read back sum to {} bp, the latest row's equity is {equity} bp", m.name, pnl.iter().sum::<i64>()));
                }
            }
        }
    }
    ctx.check(
        "verify",
        problems.is_empty(),
        if problems.is_empty() { format!("all {} models' P&L read back through the comparison join matches what was accumulated", state.len()) } else { problems.join("; ") },
    );

    let p = Problems::new();
    warm(engines[0].as_mut(), st_cmp_one, &args1((0..warmup).map(|i| model_ids[i % model_ids.len()])));
    ctx.phase("compare-one", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for _ in 0..compare_rounds {
            for &m in &model_ids {
                if tq(e, r, st_cmp_one, &[Val::I64(m)], &mut rows) && rows.len() as i64 != periods {
                    p.push(format!("compare-one model {m}: {} rows, {periods} expected", rows.len()));
                }
            }
        }
    });

    // ---- the individual read shapes
    let mut rng = Rng::new(ctx.seed ^ 0x5131_0001);
    let bar_keys: Vec<i64> = (0..ops).map(|_| rng.range(1, nbars.max(1))).collect();
    let range_keys: Vec<i64> = (0..ops).map(|_| rng.range(1, (nbars - 200).max(1))).collect();
    let sym_keys: Vec<i64> = (0..scans).map(|_| symbol_ids[rng.range(0, nsym - 1) as usize]).collect();
    let day_keys: Vec<i64> = (0..ops).map(|_| rng.range(0, nsess as i64 - 1)).collect();
    let point_keys: Vec<i64> = (0..ops).map(|_| rng.range(1, nbars.max(1))).collect();
    let agg_day_keys: Vec<i64> = (0..ops).map(|_| rng.range(0, nsess as i64 - 1)).collect();
    let mut wrng = Rng::new(ctx.seed ^ 0x5131_0002);
    let wbars: Vec<Vec<Val>> = args1((0..warmup).map(|_| wrng.range(1, nbars.max(1))));
    let wsess: Vec<Vec<Val>> = args1((0..warmup).map(|_| wrng.range(0, nsess as i64 - 1)));
    let wsyms: Vec<Vec<Val>> = args1((0..warmup).map(|i| symbol_ids[i % symbol_ids.len()]));
    let none: Vec<Vec<Val>> = vec![vec![]; warmup];

    let bar_close = |id: i64| -> i64 {
        let i = (id - 1) as usize;
        data.closes[i / nsess][i % nsess]
    };

    warm(engines[0].as_mut(), st_lookup, &wbars);
    ctx.phase("read-bar-lookup", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for &id in &bar_keys {
            if tq(e, r, st_lookup, &[Val::I64(id)], &mut rows) && (rows.len() != 1 || rows[0][0].i() != id || rows[0][7].i() != bar_close(id)) {
                p.push(format!("read-bar-lookup {id}: {rows:?}"));
            }
        }
    });
    warm(engines[0].as_mut(), st_range, &wbars.iter().map(|a| vec![a[0].clone(), Val::I64(a[0].i() + 200)]).collect::<Vec<_>>());
    ctx.phase("read-bar-range", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for &lo in &range_keys {
            let expect = (lo + 200).min(nbars) - lo + 1;
            if tq(e, r, st_range, &[Val::I64(lo), Val::I64(lo + 200)], &mut rows) && rows.len() as i64 != expect {
                p.push(format!("read-bar-range {lo}: {} rows, {expect} expected", rows.len()));
            }
        }
    });
    warm(engines[0].as_mut(), st_sym_hist, &wsyms);
    ctx.phase("read-symbol-history", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for &s in &sym_keys {
            if tq(e, r, st_sym_hist, &[Val::I64(s)], &mut rows) && rows.len() != nsess {
                p.push(format!("read-symbol-history {s}: {} rows, {nsess} expected", rows.len()));
            }
        }
    });
    warm(engines[0].as_mut(), st_day_slice, &wsess);
    ctx.phase("read-day-slice", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for &s in &day_keys {
            if tq(e, r, st_day_slice, &[Val::I64(s)], &mut rows) && rows.len() as i64 != nsym {
                p.push(format!("read-day-slice {s}: {} rows, {nsym} expected", rows.len()));
            }
        }
    });
    warm(engines[0].as_mut(), st_join_point, &wbars);
    ctx.phase("read-join-point", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for &id in &point_keys {
            if tq(e, r, st_join_point, &[Val::I64(id)], &mut rows) && (rows.len() != 1 || rows[0][0].i() != bar_close(id)) {
                p.push(format!("read-join-point {id}: {rows:?}"));
            }
        }
    });
    warm(engines[0].as_mut(), st_exists, &none);
    ctx.phase("read-join-exists", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for _ in 0..scans {
            if tq(e, r, st_exists, &[], &mut rows) && rows.len() as i64 != nsym {
                p.push(format!("read-join-exists: {} rows, {nsym} expected", rows.len()));
            }
        }
    });

    // ---- the aggregates
    let min_close = data.closes.iter().flatten().min().copied().unwrap_or(0);
    let max_close = data.closes.iter().flatten().max().copied().unwrap_or(0);
    let global = [nbars, min_close, max_close, data.volumes_sum];
    warm(engines[0].as_mut(), st_agg_global, &none);
    ctx.phase("agg-global", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for _ in 0..scans {
            if tq(e, r, st_agg_global, &[], &mut rows) && (rows.len() != 1 || row_ints(&rows[0]) != global) {
                p.push(format!("agg-global: {rows:?}, {global:?} expected"));
            }
        }
    });
    warm(engines[0].as_mut(), st_agg_sym, &none);
    ctx.phase("agg-by-symbol", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for _ in 0..scans {
            if tq(e, r, st_agg_sym, &[], &mut rows) && (rows.len() as i64 != nsym || rows.iter().any(|g| g[1].i() != nsess as i64)) {
                p.push(format!("agg-by-symbol: {} groups, {nsym} of {nsess} expected", rows.len()));
            }
        }
    });
    warm(engines[0].as_mut(), st_agg_sess, &none);
    ctx.phase("agg-by-session", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for _ in 0..scans {
            if tq(e, r, st_agg_sess, &[], &mut rows) && (rows.len() != nsess || rows.iter().any(|g| g[1].i() != nsym)) {
                p.push(format!("agg-by-session: {} groups, {nsess} of {nsym} expected", rows.len()));
            }
        }
    });
    warm(engines[0].as_mut(), st_agg_day, &wsess);
    ctx.phase("agg-day-slice", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for &s in &agg_day_keys {
            if tq(e, r, st_agg_day, &[Val::I64(s)], &mut rows) && rows.len() as i64 != nsym {
                p.push(format!("agg-day-slice {s}: {} groups, {nsym} expected", rows.len()));
            }
        }
    });
    warm(engines[0].as_mut(), st_agg_distinct, &none);
    ctx.phase("agg-distinct", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for _ in 0..scans {
            if tq(e, r, st_agg_distinct, &[], &mut rows) && (rows.len() != 1 || rows[0][0].i() != nsym) {
                p.push(format!("agg-distinct: {rows:?}, {nsym} expected"));
            }
        }
    });

    // ---- the QPS matrix: cold and warm, the Python's seven shapes
    struct Shape<'a> {
        name: &'static str,
        st: Stmt,
        /// draws a key
        key: Box<dyn Fn(&mut Rng) -> i64 + Sync + 'a>,
        /// the statement's arguments for a key
        args: fn(i64) -> Vec<Val>,
        /// rows expected for a key, where known
        expect: Box<dyn Fn(i64) -> Option<usize> + Sync + 'a>,
    }
    fn a1(k: i64) -> Vec<Val> {
        vec![Val::I64(k)]
    }
    fn a2(k: i64) -> Vec<Val> {
        vec![Val::I64(k), Val::I64(k + 200)]
    }
    let shapes: Vec<Shape> = vec![
        Shape { name: "bar-lookup", st: st_lookup, key: Box::new(move |g| g.range(1, nbars.max(1))), args: a1, expect: Box::new(|_| Some(1)) },
        Shape {
            name: "bar-range",
            st: st_range,
            key: Box::new(move |g| g.range(1, (nbars - 200).max(1))),
            args: a2,
            expect: Box::new(move |k| Some(((k + 200).min(nbars) - k + 1) as usize)),
        },
        Shape {
            name: "day-slice",
            st: st_day_slice,
            key: Box::new(move |g| g.range(0, nsess as i64 - 1)),
            args: a1,
            expect: Box::new(move |_| Some(nsym as usize)),
        },
        Shape {
            name: "symbol-history",
            st: st_sym_hist,
            key: Box::new(move |g| g.range(1, nsym)),
            args: a1,
            expect: Box::new(move |_| Some(nsess)),
        },
        Shape {
            name: "cross-join",
            st: st_xs,
            key: Box::new(move |g| g.range(0, nsess as i64 - 1)),
            args: a1,
            expect: Box::new(move |_| Some(nsym as usize)),
        },
        Shape { name: "point-join", st: st_join_point, key: Box::new(move |g| g.range(1, nbars.max(1))), args: a1, expect: Box::new(|_| Some(1)) },
        Shape {
            name: "model-join",
            st: st_cmp_one,
            key: Box::new(|g| g.range(1, MODELS.len() as i64)),
            args: a1,
            expect: Box::new(move |_| Some(periods as usize)),
        },
    ];
    let mut srng = Rng::new(ctx.seed ^ 0x5131_0003);
    let mut sweep_meta = Vec::new();
    for sh in &shapes {
        // cold: one reserved key warms the statement; every measured key
        // is distinct and unseen, and a pool smaller than qps_ops is a
        // shorter run, never padded with repeats.
        let reserved = (sh.key)(&mut srng);
        let mut seen = BTreeSet::from([reserved]);
        let mut cold = Vec::new();
        let mut attempts = 0;
        while cold.len() < qps_ops && attempts < qps_ops * 20 {
            attempts += 1;
            let key = (sh.key)(&mut srng);
            if seen.insert(key) {
                cold.push(key);
            }
        }
        warm(engines[0].as_mut(), sh.st, &vec![(sh.args)(reserved); warmup]);
        let run_cell = |ctx: &mut Ctx, engines: &mut [Box<dyn Engine>], case: &str, keys: &[i64]| {
            ctx.phase(&format!("qps-{}-{case}", sh.name), engines, |_, e, r| {
                let mut rows = Rows::new();
                for &key in keys {
                    if tq(e, r, sh.st, &(sh.args)(key), &mut rows) {
                        if let Some(n) = (sh.expect)(key) {
                            if rows.len() != n {
                                p.push(format!("qps-{}-{case} {key}: {} rows, {n} expected", sh.name, rows.len()));
                            }
                        }
                    }
                }
            })
        };
        let c = run_cell(ctx, &mut engines, "cold", &cold);
        // warm: a small fixed set drawn fresh, cycled.
        let wk: Vec<i64> = (0..warm_keys).map(|_| (sh.key)(&mut srng)).collect();
        let warm_seq: Vec<i64> = (0..qps_ops).map(|i| wk[i % wk.len()]).collect();
        warm(engines[0].as_mut(), sh.st, &(0..warmup).map(|i| (sh.args)(wk[i % wk.len()])).collect::<Vec<_>>());
        let w = run_cell(ctx, &mut engines, "warm", &warm_seq);
        sweep_meta.push(serde_json::json!({
            "shape": sh.name, "cold_ops": cold.len(), "warm_ops": warm_seq.len(),
            "cold_qps": c.throughput_ops, "warm_qps": w.throughput_ops,
            "warm_over_cold": if c.throughput_ops > 0.0 { w.throughput_ops / c.throughput_ops } else { 0.0 },
        }));
    }
    ctx.set_meta("qps_sweep", sweep_meta);

    let mut read_problems = p.take();

    // ---- the write sweep: rows a second against transaction size, on a
    // relation of its own. A row's latency includes the BEGIN and COMMIT
    // it opens or closes; batch 1 is autocommit.
    let mut next_id = 1i64;
    let mut written = 0i64;
    for &size in &write_batches {
        let size = size.max(1) as usize;
        let first = next_id;
        next_id += write_ops as i64;
        written += write_ops as i64;
        ctx.phase(&format!("write-batch-{size}"), &mut engines, |_, e, r| {
            let mut n = 0;
            for i in 0..write_ops as i64 {
                let t0 = Instant::now();
                if size > 1 && n == 0 {
                    e.begin().unwrap();
                }
                let id = first + i;
                if let Err(err) = e.exec(ins_probe, &[Val::I64(id), Val::I64(i), Val::I64(i * 2), Val::I64(i * 3), Val::I64(i * 5)]) {
                    refused(r, &err);
                }
                n += 1;
                if size > 1 && (n == size || i + 1 == write_ops as i64) {
                    e.commit().unwrap();
                    n = 0;
                }
                r.add(t0);
            }
        });
    }

    // ---- the connection sweep: the cross-section join over N connections
    let sessions_cycle: Vec<i64> = if rebalance_sessions.is_empty() { vec![0] } else { rebalance_sessions.clone() };
    for &count in &connections {
        let count = count.max(1) as usize;
        let per = (conn_ops / count).max(1);
        let mut conns: Vec<Box<dyn Engine>> = (0..count).map(|_| ctx.connect()).collect();
        let stmts: Vec<Stmt> = conns.iter_mut().map(|c| c.prepare(&xs_sql).unwrap()).collect();
        for (c, &s) in conns.iter_mut().zip(&stmts) {
            warm(c.as_mut(), s, &warm_sessions(warmup.min(8)));
        }
        ctx.phase(&format!("join-conn-{count}"), &mut conns, |i, e, r| {
            let mut rows = Rows::new();
            for j in 0..per {
                let s = sessions_cycle[(i * per + j) % sessions_cycle.len()];
                if tq(e, r, stmts[i], &[Val::I64(s)], &mut rows) && rows.len() as i64 != nsym {
                    p.push(format!("join-conn-{count} {s}: {} rows, {nsym} expected", rows.len()));
                }
            }
        });
    }
    read_problems.extend(p.take());
    ctx.check(
        "read-shapes-rows",
        read_problems.is_empty(),
        if read_problems.is_empty() { "every read shape returned the rows and values the generated data gives".to_string() } else { read_problems.join("; ") },
    );

    // ---- what each relation holds at the end
    let expect: BTreeMap<&str, i64> = [
        ("s1_exchanges", nexch),
        ("s1_symbols", nsym),
        ("s1_sessions", nsess as i64),
        ("s1_daily_bars", nbars),
        ("s1_daily_stats", nbars),
        ("s1_models", MODELS.len() as i64),
        ("s1_model_results", results.len() as i64),
        ("s1_write_probe", written),
    ]
    .into_iter()
    .collect();
    let mut counts = Vec::new();
    let mut ok = true;
    let mut rows = Rows::new();
    for (name, st) in &st_counts {
        let got = match engines[0].query(*st, &[], &mut rows) {
            Ok(()) => rows[0][0].i(),
            Err(err) => {
                eprintln!("s1: count {name}: {err}");
                -1
            }
        };
        ok &= got == expect[name];
        counts.push(format!("{name} {got}/{}", expect[name]));
    }
    ctx.check("row-counts", ok, counts.join(", "));

    let errs: Vec<String> = ctx.phases.iter().filter(|ph| !ph.errors.is_empty()).map(|ph| format!("{} {:?}", ph.name, ph.errors)).collect();
    ctx.check("no-refusals", errs.is_empty(), if errs.is_empty() { format!("{} phases", ctx.phases.len()) } else { errs.join("; ") });
}
