//! Scenario 0, the brokerage book: `tools/scenario0_stockmarket.py` (KDS)
//! and its twin `tools/pg_scenario0_stockmarket.py` (PostgreSQL), ported to
//! one code path for KDS, PostgreSQL and Oracle.
//!
//! **The workload.** Five relations; N trader terminals each run business
//! transactions - two `trades` legs inserted, two `accounts` rows updated
//! with balances computed client-side - against a disjoint partition of
//! accounts, while one reporter terminal wakes every `profit_interval_ms`,
//! and at each crossed simulated period reads `profit_users` sampled users'
//! accounts by `user_id` (a non-key equality, no index: a walk of
//! `s0_accounts` per user) and appends one `s0_user_periodic_profit` row per
//! user. `days` simulated business days are compressed into the `seconds`
//! of the measured phase (or into the work, under `txn_target`).
//!
//! **Relation kinds** (`bench/README.md`, "What a relation is stored as"):
//!
//! | relation | KDS kind | the statements that decided it |
//! |---|---|---|
//! | `s0_accounts` | BTREE | `UPDATE ... WHERE id = ?` twice per transaction and the verify's `SELECT ... WHERE id = ?`: points by the tuple key. The reporter's `WHERE user_id = ?` walks it whole on any kind. Also the foreign key's parent under `fk=1` (a descent by the key) |
//! | `s0_trades` | HEAP | `INSERT` only during the run; read once, by `COUNT(*)`, in the check: append and walk |
//! | `s0_user_periodic_profit` | HEAP | `INSERT` only; `COUNT(*)` in the check: append and walk |
//! | `s0_users` | HEAP | loaded and never read by a statement (the reporter samples user ids the client generated): append only |
//! | `s0_assets` | HEAP | loaded and never read by a statement: append only |
//!
//! The Python declares every relation BTREE (AS-Q6's BTREE-only mark,
//! replaced by EO's rule), so no delta crosses from a Python run to this one.
//!
//! **Phases**, the Python's names:
//!
//! - `load-users`, `load-assets`, `load-accounts`: one prepared single-row
//!   `INSERT` per row, autocommit per row as the Python's (`load_batch=N`
//!   groups N rows a transaction instead; the commit is in the row that
//!   carries it). One operation a row.
//! - `txn`: the measured phase, every trader and the reporter concurrent.
//!   One operation is one committed business transaction (its four
//!   statements, plus `BEGIN`/`COMMIT` under `txn=1`), so its throughput is
//!   the TPS. The reporter records nothing into it, but its walks are in
//!   the phase's server CPU, as the Python's TPS is measured under them.
//! - `trade-insert`, `account-update`, `profit-scan`, `profit-insert`:
//!   **derived from the `txn` phase**, every execution of that statement
//!   kind in it, failed ones included; they carry latency, throughput over
//!   the `txn` phase's wall clock and errors, and no server CPU, client CPU
//!   or breakdown (those are the `txn` phase's, which they are part of).
//!   The reporter's scan is warmed before the phase (32 unrecorded
//!   executions on other users) so its parse and plan are not in it, and
//!   every write statement on its own connection (6 executions, each in a
//!   transaction rolled back, on ids no terminal issues).
//!
//! **Retries.** Partitions are disjoint, so no two traders write one
//! account and the Python retries nothing. Here a refusal classed
//! `ErrClass::Conflict` (a KDS stale descent's `TxnConflict` that reached
//! the client, a PostgreSQL serialization failure, Oracle's ORA-00060 or
//! ORA-08177) is retried, up to 100 attempts, and each attempt is counted
//! as `conflict` in the phase's errors: under `txn=1` the whole transaction
//! is rolled back and re-run with the same values, in autocommit the one
//! statement is re-run. Any other refusal is recorded by its class and, as
//! in the Python, leaves a `torn` transaction in autocommit (the statements
//! that did apply stay applied, and the client's balances follow them) or a
//! rolled-back one under `txn=1`.
//!
//! **Checks.** `balance-verify`: `verify` sampled accounts read back by id
//! against the traders' in-memory balance, asset quantity and trade count
//! (the Python's `--verify`). `balance-conserved`: `COUNT`, `SUM(balance)`,
//! `SUM(asset_qty)` and `SUM(trade_count)` over `s0_accounts` equal the
//! client's totals, and - with no torn transaction - equal the conserved
//! values (opening cash times accounts, zero net quantity, two legs per
//! committed transaction). `trades-rows` and `profit-rows`: each relation's
//! `COUNT(*)` equals the rows the client saw inserted.
//!
//! **Knobs** (`--set k=v`), the Python's defaults (the PostgreSQL control
//! run of `bench/v3.5.0/results-pg-control-scenarios-v3.4.17-202-g481d101aa.md`
//! §2 has no scenario 0 cell): `users` 10000, `assets` 10000,
//! `accounts_per_user` 2 (each user 1 below to 1 above), `days` 180,
//! `seconds` 60, `traders` 1, `profit` 1, `profit_interval_ms` 1000,
//! `profit_period_days` 7, `profit_users` 50, `txn` 0, `fk` 0
//! (`s0_trades.account_id REFERENCES s0_accounts`), `verify` 200,
//! `load_batch` 0, `txn_target` 0 (the Python's `--txn-per-user N` is
//! `txn_target` = N x `users`; `seconds` is then a ceiling and the business
//! clock runs on committed work).
//!
//! **Left out**, and why:
//!
//! - `--echo`, `--latency-trace`, `--server-log`, `--json`'s own report and
//!   `--timeout`: the harness's own output and the engines' own profilers
//!   (`src/profile.rs`) take their place.
//! - `--cabin` (a Cabin on `accounts.user_id`) and the twin's
//!   `--user-id-index`: a KDS-only structure and its PostgreSQL analogue,
//!   not one statement on three engines; and `SHOW CABINS`, read only for it.
//! - `--sync` (KDS's `SYNC` line command), which no other engine has.
//! - `--suffix`: tables are named `s0_*` and dropped at the start instead.
//! - Server-issued ids: the Python's `INSERT`s omit the key and read the
//!   issued one back; here every key is the client's (users, assets and
//!   accounts 1..n; a trade leg `(trader << 40) + n`; a profit row 1..n),
//!   since PostgreSQL and Oracle have no issued key without an identity
//!   column.
//! - `trade_day` is the Python's wall-clock business day in a time-based
//!   run, so its values (and how many transactions a run reaches) differ
//!   between engines; every other parameter is the same seeded sequence on
//!   every engine.

use super::*;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

/// Every account opens with 1,000,000.00 in minor units (the Python's
/// `OPENING_BALANCE`).
const OPENING_BALANCE: i64 = 100_000_000;
const COUNTRIES: [&str; 8] = ["KR", "US", "JP", "GB", "DE", "SG", "HK", "AU"];
const ASSET_CLASSES: i64 = 4;
const SIDE_BUY: i64 = 0;
const SIDE_SELL: i64 = 1;
/// Attempts of one transaction (or, in autocommit, one statement) refused
/// `Conflict` before it counts as failed.
const MAX_ATTEMPTS: u32 = 100;
/// Unrecorded executions of the reporter's scan before the phase.
const WARMUP: usize = 32;
/// Rolled-back executions of each write statement before the phase: past
/// PostgreSQL's five custom plans.

const USERS: Table = Table {
    name: "s0_users",
    cols: &[("id", Ty::I64), ("name", Ty::Str(16)), ("country", Ty::Str(16)), ("tier", Ty::I32), ("created_day", Ty::I32)],
    store: Store::Heap,
    refs: &[],
};
const ASSETS: Table = Table {
    name: "s0_assets",
    cols: &[("id", Ty::I64), ("symbol", Ty::Str(16)), ("asset_class", Ty::I32), ("last_price", Ty::I64)],
    store: Store::Heap,
    refs: &[],
};
const ACCOUNTS: Table = Table {
    name: "s0_accounts",
    cols: &[
        ("id", Ty::I64),
        ("user_id", Ty::I64),
        ("balance", Ty::I64),
        ("asset_qty", Ty::I64),
        ("trade_count", Ty::I64),
        ("opened_day", Ty::I32),
    ],
    store: Store::Btree,
    refs: &[],
};
const TRADE_COLS: &[(&str, Ty)] = &[
    ("id", Ty::I64),
    ("account_id", Ty::I64),
    ("asset_id", Ty::I64),
    ("side", Ty::I32),
    ("qty", Ty::I64),
    ("price", Ty::I64),
    ("trade_day", Ty::I32),
];
const PROFIT: Table = Table {
    name: "s0_user_periodic_profit",
    cols: &[("id", Ty::I64), ("user_id", Ty::I64), ("period_day", Ty::I32), ("realized", Ty::I64), ("trade_count", Ty::I64)],
    store: Store::Heap,
    refs: &[],
};
/// Parents before children (the foreign key under `fk=1`).
const CREATE_ORDER: [&str; 5] = ["s0_users", "s0_assets", "s0_accounts", "s0_trades", "s0_user_periodic_profit"];

fn class(e: &DbError) -> &'static str {
    match e.class {
        ErrClass::Conflict => "conflict",
        ErrClass::Unique => "unique",
        ErrClass::Constraint => "constraint",
        ErrClass::Other => "other",
    }
}

/// Every execution of one statement kind inside the measured phase.
#[derive(Default)]
struct Sub {
    lat_ns: Vec<u64>,
    errors: BTreeMap<String, u64>,
}

impl Sub {
    fn time<T>(&mut self, f: impl FnOnce() -> DbResult<T>) -> DbResult<T> {
        let t0 = Instant::now();
        let r = f();
        self.lat_ns.push(t0.elapsed().as_nanos() as u64);
        if let Err(e) = &r {
            *self.errors.entry(class(e).to_string()).or_insert(0) += 1;
        }
        r
    }
}

#[derive(Default)]
struct TraderOut {
    state: Vec<(i64, [i64; 3])>,
    committed: u64,
    torn: u64,
    rolled_back: u64,
    underfunded: u64,
    conflicts: u64,
    legs: u64,
    first_error: Option<String>,
}

/// A trader's refusal: into the phase's errors by class, and counted.
fn note(out: &mut TraderOut, r: &mut Recorder, err: &DbError) {
    r.error(class(err));
    if err.class == ErrClass::Conflict {
        out.conflicts += 1;
    } else if out.first_error.is_none() {
        out.first_error = Some(err.to_string());
    }
}

#[derive(Default)]
struct ReporterOut {
    periods: u64,
    rows: u64,
    first_error: Option<String>,
}

/// What every terminal of the measured phase shares.
struct Shared {
    start: OnceLock<Instant>,
    committed: AtomicU64,
    stop: AtomicBool,
    subs: Mutex<BTreeMap<&'static str, Sub>>,
    traders: Mutex<Vec<TraderOut>>,
    reporter: Mutex<Option<ReporterOut>>,
}

impl Shared {
    fn put_sub(&self, name: &'static str, s: Sub) {
        let mut m = self.subs.lock().unwrap();
        let e = m.entry(name).or_default();
        e.lat_ns.extend(s.lat_ns);
        for (k, v) in s.errors {
            *e.errors.entry(k).or_insert(0) += v;
        }
    }
}

/// Which of `days` business days the run is in: progress through the work
/// under a target, else through the wall clock (the Python's `sim_day`).
fn sim_day(start: Instant, seconds: u64, days: i64, sh: &Shared, target: u64) -> i64 {
    let done = if target > 0 {
        sh.committed.load(Ordering::Relaxed) as f64 / target as f64
    } else if seconds == 0 {
        0.0
    } else {
        start.elapsed().as_secs_f64() / seconds as f64
    };
    ((done * days as f64) as i64).clamp(0, days - 1)
}

/// `k` distinct values of `1..=n`, a partial Fisher-Yates (the Python's
/// `rng.sample`).
fn sample(rng: &mut Rng, n: usize, k: usize) -> Vec<i64> {
    let mut v: Vec<i64> = (1..=n as i64).collect();
    let k = k.min(n);
    for i in 0..k {
        let j = rng.range(i as i64, n as i64 - 1) as usize;
        v.swap(i, j);
    }
    v.truncate(k);
    v
}

fn load_rows(e: &mut dyn Engine, ins: Stmt, rows: &[Vec<Val>], batch: u64, table: &str, r: &mut Recorder) {
    if batch > 0 {
        e.begin().unwrap();
    }
    for (n, row) in rows.iter().enumerate() {
        let t0 = Instant::now();
        e.exec(ins, row).unwrap_or_else(|err| panic!("load {table}: {err}"));
        if batch > 0 && (n as u64 + 1) % batch == 0 {
            e.commit().unwrap();
            e.begin().unwrap();
        }
        r.add(t0);
    }
    if batch > 0 {
        e.commit().unwrap();
    }
}

pub fn run(ctx: &mut Ctx) {
    let k = ctx.kind;
    let n_users = ctx.opt("users", 10_000) as usize;
    let n_assets = ctx.opt("assets", 10_000) as i64;
    let per_user = ctx.opt("accounts_per_user", 2) as i64;
    let days = ctx.opt("days", 180).max(1) as i64;
    let seconds = ctx.opt("seconds", 60);
    let n_traders = ctx.opt("traders", 1).max(1) as usize;
    let profit = ctx.opt("profit", 1) != 0;
    let interval = Duration::from_millis(ctx.opt("profit_interval_ms", 1000));
    let period_days = ctx.opt("profit_period_days", 7) as i64;
    let profit_users = ctx.opt("profit_users", 50) as usize;
    let txn = ctx.opt("txn", 0) != 0;
    let fk = ctx.opt("fk", 0) != 0;
    let n_verify = ctx.opt("verify", 200) as usize;
    let load_batch = ctx.opt("load_batch", 0);
    let target = ctx.opt("txn_target", 0);
    let seed = ctx.seed;
    assert!(n_users > 0 && n_assets > 0, "users and assets must be at least 1");

    // ---- schema ----
    let fk_refs: &[(&str, &str)] = if fk { &[("account_id", "s0_accounts")] } else { &[] };
    let trades = Table { name: "s0_trades", cols: TRADE_COLS, store: Store::Heap, refs: fk_refs };
    let tables = [&USERS, &ASSETS, &ACCOUNTS, &trades, &PROFIT];
    let mut e = ctx.connect();
    for t in CREATE_ORDER.iter().rev() {
        drop_table(e.as_mut(), t);
    }
    for t in &tables {
        let sql = create_table(k, t);
        e.run(&sql).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }

    // ---- data: one seeded sequence, the Python's order of draws ----
    let mut rng = Rng::new(seed);
    let users: Vec<Vec<Val>> = (0..n_users)
        .map(|i| {
            let country = COUNTRIES[rng.range(0, COUNTRIES.len() as i64 - 1) as usize];
            vec![Val::I64(i as i64 + 1), Val::Str(format!("user{i:07}")), country.into(), Val::I64(rng.range(0, 3)), Val::I64(0)]
        })
        .collect();
    let assets: Vec<Vec<Val>> = (0..n_assets)
        .map(|i| {
            let class = rng.range(0, ASSET_CLASSES - 1);
            vec![Val::I64(i + 1), Val::Str(format!("SYM{i:06}")), Val::I64(class), Val::I64(rng.range(1_000, 5_000_000))]
        })
        .collect();
    let mut accounts: Vec<Vec<Val>> = Vec::new();
    for uid in 1..=n_users as i64 {
        let n = rng.range((per_user - 1).max(1), per_user + 1).max(1);
        for _ in 0..n {
            let id = accounts.len() as i64 + 1;
            accounts.push(vec![Val::I64(id), Val::I64(uid), Val::I64(OPENING_BALANCE), Val::I64(0), Val::I64(0), Val::I64(0)]);
        }
    }
    let n_accounts = accounts.len();
    assert!(n_accounts >= 2 * n_traders, "{n_accounts} accounts is too few for {n_traders} traders: each needs 2");

    // ---- load ----
    let mut one = vec![e];
    for (phase, t, rows) in [("load-users", &USERS, &users), ("load-assets", &ASSETS, &assets), ("load-accounts", &ACCOUNTS, &accounts)] {
        let marks = vec!["?"; t.cols.len()].join(", ");
        let ins = one[0].prepare(&format!("INSERT INTO {} VALUES ({marks})", t.name)).unwrap();
        ctx.phase(phase, &mut one, |_, e, r| load_rows(e, ins, rows, load_batch, t.name, r));
    }
    let mut e = one.pop().unwrap();
    analyze(e.as_mut(), &CREATE_ORDER);

    // ---- terminals: traders 0..n, then the reporter ----
    let ins_trade_sql = "INSERT INTO s0_trades VALUES (?, ?, ?, ?, ?, ?, ?)";
    let upd_sql = "UPDATE s0_accounts SET balance = ?, asset_qty = ?, trade_count = ? WHERE id = ?";
    let scan_sql = "SELECT id, user_id, balance, asset_qty, trade_count, opened_day FROM s0_accounts WHERE user_id = ?";
    let ins_profit_sql = "INSERT INTO s0_user_periodic_profit VALUES (?, ?, ?, ?, ?)";
    let mut engines: Vec<Box<dyn Engine>> = Vec::new();
    let mut stmts = Vec::new();
    for i in 0..n_traders {
        let mut t = ctx.connect();
        let (ins, upd) = (t.prepare(ins_trade_sql).unwrap(), t.prepare(upd_sql).unwrap());
        // The writes' first executions (Oracle's hard parse, a PostgreSQL
        // backend's first touch of the relation) out of the phase: rolled
        // back, on an id no trader issues and the trader's own first account.
        let acct = i as i64 + 1;
        let leg = [((i as i64) << 40) + (1 << 39), acct, 1, SIDE_BUY, 1, 1_000, 0].map(Val::I64);
        let opening = [OPENING_BALANCE, 0, 0, acct].map(Val::I64);
        warm_writes(t.as_mut(), &[(ins, &leg), (upd, &opening)]);
        stmts.push((ins, upd));
        engines.push(t);
    }
    let mut rep_stmts = None;
    if profit {
        let mut t = ctx.connect();
        let scan = t.prepare(scan_sql).unwrap();
        let ins = t.prepare(ins_profit_sql).unwrap();
        let mut warm = Rng::new(seed + 5555);
        let mut rows = Rows::new();
        for _ in 0..WARMUP {
            t.query(scan, &[Val::I64(warm.range(1, n_users as i64))], &mut rows).unwrap();
        }
        warm_writes(t.as_mut(), &[(ins, &[1 << 40, 1, 0, 0, 0].map(Val::I64))]);
        rep_stmts = Some((scan, ins));
        engines.push(t);
    }
    let parts: Vec<Vec<i64>> = (0..n_traders).map(|i| (1..=n_accounts as i64).skip(i).step_by(n_traders).collect()).collect();
    let per_trader = if target > 0 { target.div_ceil(n_traders as u64) } else { 0 };
    let sh = Shared {
        start: OnceLock::new(),
        committed: AtomicU64::new(0),
        stop: AtomicBool::new(false),
        subs: Mutex::new(BTreeMap::new()),
        traders: Mutex::new(Vec::new()),
        reporter: Mutex::new(None),
    };

    let main = ctx.phase("txn", &mut engines, |i, e, r| {
        let start = *sh.start.get_or_init(Instant::now);
        let deadline = start + Duration::from_secs(seconds);
        if i < n_traders {
            let (ins, upd) = stmts[i];
            let ids = &parts[i];
            let mut rng = Rng::new(seed + 1000 + i as u64);
            let mut state: HashMap<i64, [i64; 3]> = ids.iter().map(|&a| (a, [OPENING_BALANCE, 0, 0])).collect();
            let (mut s_ins, mut s_upd) = (Sub::default(), Sub::default());
            let mut out = TraderOut::default();
            let mut next_trade = ((i as i64) << 40) + 1;
            while Instant::now() < deadline && !(per_trader > 0 && out.committed >= per_trader) {
                let buyer = ids[rng.range(0, ids.len() as i64 - 1) as usize];
                let seller = ids[rng.range(0, ids.len() as i64 - 1) as usize];
                if buyer == seller {
                    continue;
                }
                let asset = rng.range(1, n_assets);
                let day = sim_day(start, seconds, days, &sh, target);
                let price = rng.range(1_000, 500_000);
                let mut qty = rng.range(1, 100);
                let mut notional = price * qty;
                let cash = state[&buyer][0];
                if cash < notional {
                    // Scaled to what the buyer can afford, floor 1, as the Python.
                    qty = (cash / price).max(1);
                    notional = price * qty;
                    if cash < notional {
                        out.underfunded += 1;
                        continue;
                    }
                }
                let (b, s) = (state[&buyer], state[&seller]);
                let b_new = [b[0] - notional, b[1] + qty, b[2] + 1];
                let s_new = [s[0] + notional, s[1] - qty, s[2] + 1];
                let tid = next_trade;
                next_trade += 2;
                let legs: [Vec<Val>; 2] = [
                    vec![tid.into(), buyer.into(), asset.into(), SIDE_BUY.into(), qty.into(), price.into(), day.into()],
                    vec![(tid + 1).into(), seller.into(), asset.into(), SIDE_SELL.into(), qty.into(), price.into(), day.into()],
                ];
                let moves = [(buyer, b_new), (seller, s_new)];
                let upd_params =
                    |(a, v): (i64, [i64; 3])| -> Vec<Val> { vec![v[0].into(), v[1].into(), v[2].into(), a.into()] };

                let t0 = Instant::now();
                let mut ok;
                if txn {
                    let mut attempt = 0;
                    loop {
                        attempt += 1;
                        let res: DbResult<()> = (|| {
                            e.begin()?;
                            for l in &legs {
                                s_ins.time(|| e.exec(ins, l))?;
                            }
                            for m in moves {
                                s_upd.time(|| e.exec(upd, &upd_params(m)))?;
                            }
                            e.commit()
                        })();
                        match res {
                            Ok(()) => {
                                ok = true;
                                break;
                            }
                            Err(err) => {
                                let _ = e.rollback();
                                note(&mut out, r, &err);
                                if err.class == ErrClass::Conflict && attempt < MAX_ATTEMPTS {
                                    continue;
                                }
                                ok = false;
                                break;
                            }
                        }
                    }
                    if ok {
                        for (a, v) in moves {
                            state.insert(a, v);
                        }
                        out.legs += 2;
                    } else {
                        out.rolled_back += 1;
                    }
                } else {
                    // Four autocommit statements; each one's effect is the
                    // client's as soon as it is accepted, so a torn
                    // transaction leaves the client matching the store.
                    ok = true;
                    let (mut written, mut applied) = (0, 0);
                    for step in 0..4 {
                        let mut attempt = 0;
                        let res = loop {
                            attempt += 1;
                            let res = if step < 2 {
                                s_ins.time(|| e.exec(ins, &legs[step]))
                            } else {
                                s_upd.time(|| e.exec(upd, &upd_params(moves[step - 2])))
                            };
                            match res {
                                Err(err) if err.class == ErrClass::Conflict && attempt < MAX_ATTEMPTS => note(&mut out, r, &err),
                                other => break other,
                            }
                        };
                        match res {
                            Ok(_) if step < 2 => {
                                written += 1;
                                applied += 1;
                            }
                            Ok(_) => {
                                let (a, v) = moves[step - 2];
                                state.insert(a, v);
                                applied += 1;
                            }
                            Err(err) => {
                                note(&mut out, r, &err);
                                ok = false;
                            }
                        }
                    }
                    out.legs += written;
                    // Torn: some statement applied and some did not - an
                    // update alone moves the balances off the conserved sums
                    // as surely as a leg alone does.
                    if !ok && applied > 0 {
                        out.torn += 1;
                    }
                }
                if ok {
                    r.add(t0);
                    out.committed += 1;
                    sh.committed.fetch_add(1, Ordering::Relaxed);
                }
            }
            // The measured window is over: the reporter stops with the
            // first trader to finish, as the Python's stop event.
            sh.stop.store(true, Ordering::Relaxed);
            out.state = state.into_iter().collect();
            sh.put_sub("trade-insert", s_ins);
            sh.put_sub("account-update", s_upd);
            sh.traders.lock().unwrap().push(out);
        } else {
            let (scan, ins) = rep_stmts.unwrap();
            let mut rng = Rng::new(seed + 7777);
            let (mut s_scan, mut s_ins) = (Sub::default(), Sub::default());
            let mut out = ReporterOut::default();
            let mut rows = Rows::new();
            let mut next_period = period_days;
            let mut profit_id = 1i64;
            let over = || Instant::now() >= deadline || sh.stop.load(Ordering::Relaxed);
            'run: while !over() {
                // Sleeps `interval`, in 10 ms slices so a finished work
                // target ends the phase within one.
                let wake = (Instant::now() + interval).min(deadline);
                while Instant::now() < wake {
                    if sh.stop.load(Ordering::Relaxed) {
                        break 'run;
                    }
                    std::thread::sleep((wake - Instant::now()).min(Duration::from_millis(10)));
                }
                if over() {
                    break;
                }
                let day = sim_day(start, seconds, days, &sh, target);
                if day < next_period {
                    continue;
                }
                for uid in sample(&mut rng, n_users, profit_users) {
                    if over() {
                        break;
                    }
                    if let Err(err) = s_scan.time(|| e.query(scan, &[Val::I64(uid)], &mut rows)) {
                        r.error(&format!("profit-scan:{}", class(&err)));
                        out.first_error.get_or_insert(err.to_string());
                        continue;
                    }
                    let balance: i64 = rows.iter().map(|row| row[2].i()).sum();
                    let trades: i64 = rows.iter().map(|row| row[4].i()).sum();
                    let realized = balance - rows.len() as i64 * OPENING_BALANCE;
                    let p = [Val::I64(profit_id), Val::I64(uid), Val::I64(day), Val::I64(realized), Val::I64(trades)];
                    match s_ins.time(|| e.exec(ins, &p)) {
                        Ok(_) => {
                            out.rows += 1;
                            profit_id += 1;
                        }
                        Err(err) => {
                            r.error(&format!("profit-insert:{}", class(&err)));
                            out.first_error.get_or_insert(err.to_string());
                        }
                    }
                }
                out.periods += 1;
                next_period = day + period_days;
            }
            sh.put_sub("profit-scan", s_scan);
            sh.put_sub("profit-insert", s_ins);
            *sh.reporter.lock().unwrap() = Some(out);
        }
    });

    // ---- the derived per-statement phases ----
    let subs = std::mem::take(&mut *sh.subs.lock().unwrap());
    for name in ["trade-insert", "account-update", "profit-scan", "profit-insert"] {
        let Some(s) = subs.get(name) else { continue };
        let ops = s.lat_ns.len() as f64;
        let p = PhaseOut {
            name: name.to_string(),
            latency: latency(&s.lat_ns),
            throughput_ops: if main.wall_s > 0.0 { ops / main.wall_s } else { 0.0 },
            wall_s: main.wall_s,
            terminals: if name.starts_with("profit") { 1 } else { n_traders as u32 },
            errors: s.errors.clone(),
            ..Default::default()
        };
        eprintln!(
            "phase {name} (of txn): {} execs, mean {:.1} us, p50 {:.1} us, p99 {:.1} us, errors {:?}",
            p.latency.ops, p.latency.mean_us, p.latency.p50_us, p.latency.p99_us, p.errors
        );
        ctx.phases.push(p);
    }

    let outs = std::mem::take(&mut *sh.traders.lock().unwrap());
    let rep = sh.reporter.lock().unwrap().take();
    let sum = |f: fn(&TraderOut) -> u64| outs.iter().map(f).sum::<u64>();
    let (committed, torn, legs) = (sum(|o| o.committed), sum(|o| o.torn), sum(|o| o.legs));
    let wall = main.wall_s;
    for o in &outs {
        if let Some(m) = &o.first_error {
            eprintln!("s0: a trader's first refusal: {m}");
        }
    }
    if let Some(m) = rep.as_ref().and_then(|r| r.first_error.as_ref()) {
        eprintln!("s0: the reporter's first refusal: {m}");
    }

    // ---- checks ----
    let state: BTreeMap<i64, [i64; 3]> = outs.iter().flat_map(|o| o.state.iter().copied()).collect();
    let mut rows = Rows::new();
    let by_id = e.prepare("SELECT id, balance, asset_qty, trade_count FROM s0_accounts WHERE id = ?").unwrap();
    let ids: Vec<i64> = state.keys().copied().collect();
    let mut vrng = Rng::new(seed + 9999);
    let picks = sample(&mut vrng, ids.len(), n_verify);
    let (mut checked, mut mismatched, mut first) = (0, 0, String::new());
    for p in picks {
        let id = ids[p as usize - 1];
        e.query(by_id, &[Val::I64(id)], &mut rows).unwrap();
        checked += 1;
        let want = state[&id];
        let got = rows.first().map(|r| [r[1].i(), r[2].i(), r[3].i()]);
        if got != Some(want) {
            mismatched += 1;
            if first.is_empty() {
                first = format!("; account {id}: stored balance/qty/trades {got:?}, driver expected {want:?}");
            }
        }
    }
    ctx.check("balance-verify", mismatched == 0, format!("{checked} accounts read back, {mismatched} mismatched{first}"));

    let totals = e.prepare("SELECT COUNT(*), SUM(balance), SUM(asset_qty), SUM(trade_count) FROM s0_accounts").unwrap();
    e.query(totals, &[], &mut rows).unwrap();
    let stored: Vec<i64> = rows[0].iter().map(|v| v.i()).collect();
    let client = vec![
        state.len() as i64,
        state.values().map(|v| v[0]).sum(),
        state.values().map(|v| v[1]).sum(),
        state.values().map(|v| v[2]).sum(),
    ];
    let conserved = vec![n_accounts as i64, n_accounts as i64 * OPENING_BALANCE, 0, 2 * committed as i64];
    let ok = stored == client && (torn > 0 || stored == conserved);
    ctx.check(
        "balance-conserved",
        ok,
        format!("count/sum(balance)/sum(qty)/sum(trades): stored {stored:?}, client {client:?}, conserved {conserved:?}, torn {torn}"),
    );

    let count = |e: &mut Box<dyn Engine>, t: &str, rows: &mut Rows| -> i64 {
        let s = e.prepare(&format!("SELECT COUNT(*) FROM {t}")).unwrap();
        e.query(s, &[], rows).unwrap();
        rows[0][0].i()
    };
    let n_trades = count(&mut e, "s0_trades", &mut rows);
    ctx.check("trades-rows", n_trades == legs as i64, format!("{n_trades} rows, {legs} legs inserted"));
    let profit_rows = rep.as_ref().map(|r| r.rows).unwrap_or(0);
    let n_profit = count(&mut e, "s0_user_periodic_profit", &mut rows);
    ctx.check("profit-rows", n_profit == profit_rows as i64, format!("{n_profit} rows, {profit_rows} inserted"));

    // ---- meta ----
    let kinds: BTreeMap<&str, &str> = tables
        .iter()
        .map(|t| (t.name, match t.store {
            Store::Btree => "BTREE",
            Store::Heap => "HEAP",
        }))
        .collect();
    ctx.set_meta("relation_kinds", kinds);
    ctx.set_meta("users", n_users);
    ctx.set_meta("assets", n_assets);
    ctx.set_meta("accounts", n_accounts);
    ctx.set_meta("traders", n_traders);
    ctx.set_meta("profit", profit);
    ctx.set_meta("txn", txn);
    ctx.set_meta("fk", fk);
    ctx.set_meta("days", days);
    ctx.set_meta("seconds_budget", seconds);
    ctx.set_meta("txn_target", target);
    ctx.set_meta("limit", if target > 0 && committed >= target { "transactions" } else { "seconds" });
    ctx.set_meta("seconds", wall);
    ctx.set_meta("tps", if wall > 0.0 { committed as f64 / wall } else { 0.0 });
    ctx.set_meta("committed", committed);
    ctx.set_meta("torn", torn);
    ctx.set_meta("rolled_back", sum(|o| o.rolled_back));
    ctx.set_meta("underfunded", sum(|o| o.underfunded));
    ctx.set_meta("conflict_retries", sum(|o| o.conflicts));
    ctx.set_meta("per_trader_committed", outs.iter().map(|o| o.committed).collect::<Vec<_>>());
    ctx.set_meta("profit_periods", rep.as_ref().map(|r| r.periods).unwrap_or(0));
    ctx.set_meta("profit_rows", profit_rows);
    ctx.set_meta(
        "derived_phases",
        "trade-insert, account-update, profit-scan and profit-insert are every execution of that statement inside the txn \
         phase: latency, errors and throughput over txn's wall clock only; CPU and breakdown are txn's",
    );
}
