//! s2_freight: scenario 2, freight and cargo - the eight-statement booking
//! transaction, ported from `tools/scenario2_freight.py` (KDS) and its twin
//! `tools/pg_scenario2_freight.py` (PostgreSQL). The plan both implement is
//! `docs/inflight/in-progress/scenario2-freight.md` at `1769487`.
//!
//! **What a booking is.** One cargo placed on one voyage, under one
//! `BEGIN`/`COMMIT`: (1) the cargo by key, (2) its customer's credit by key,
//! (3) the voyage's used capacity by key (`capacity=cached`, the default) or
//! re-derived as `SUM` over the ledger (`capacity=scan`), (4) the cargo
//! type's pricing rules through the `recipes(cargo_type)` index; two
//! client-side checks (capacity, credit); then (5) the freight row, (6) one
//! charge row per fee that applies, (7) the voyage's `booked_cbm` and
//! `revenue`, (8) the customer's `outstanding`, written as values computed
//! from the reads, as the Python writes them. Three outcomes are counted
//! apart: committed, refused by the business (capacity, credit), and a
//! conflict - rolled back and re-driven from the top up to `max_retries`,
//! each conflict counted on the row it happened on.
//!
//! **Relation kinds on KDS** (`bench/README.md`, "What a relation is stored
//! as"; PostgreSQL and Oracle store each as their own heap table):
//!
//! | relation | kind | the statements that decided it |
//! |---|---|---|
//! | `s2_organizations` | BTREE | `credit-lookup` and `org-update` by key; verify's `outstanding` by key |
//! | `s2_ships` | HEAP | loaded and read by no statement (append only); a voyage carries its ship's capacity client-side |
//! | `s2_operations` | BTREE | `capacity-read` and `operation-update` by key; verify's `booked_cbm` by key |
//! | `s2_cargos` | BTREE | `cargo-lookup` by key; `customer-statement` and verify's I3 join reach it by `c.id` |
//! | `s2_fees` | HEAP | loaded and read by no statement (append only); the rate card is priced client-side |
//! | `s2_recipes` | HEAP | `recipe-read` reaches its rows through the `recipes(cargo_type)` index only |
//! | `s2_freights` | HEAP | appended by `freight-insert`; `manifest-scan`, `voyage-rollup` and `capacity=scan` reach it through the `freights(operation_id)` index; `customer-statement` walks it; never read by its key |
//! | `s2_charges` | HEAP | appended by `charge-insert`; walked by verify alone (`WHERE freight_id`), never read by its key |
//!
//! **Indexes**: `freights(operation_id)` and `recipes(cargo_type)` on every
//! engine - the two the PostgreSQL twin creates. The KDS driver created none
//! (a FilterScan, or a Cabin under `--cabin`); one client sends one set of
//! statements, so the same two indexes are created on all three.
//!
//! **Isolation**: each engine's default, `READ COMMITTED` on all three. The
//! Python's `--isolation read-committed|repeatable-read` is not ported.
//! Under `READ COMMITTED` the booking's read-then-write of `booked_cbm` and
//! `outstanding` can lose an update when two bookers share a row (a write
//! that commits between another booker's read and its write is overwritten
//! by a value computed from the stale read - on PostgreSQL and Oracle, and
//! on KDS where the writer committed before the overwriting statement
//! began, `docs/spec/txn.md` §5). So **`lock`** (`auto` | `0` | `1`): `1`
//! makes `credit-lookup` and `capacity-read` `SELECT ... FOR UPDATE`, the
//! one locking read all three take; `auto`, the default, is `1` exactly when
//! `bookers > 1` with `contend=1`, `capacity=cached` and `txn=1`, so the
//! one-booker run sends the Python's statements verbatim. Measured at small
//! scale (20 voyages, 20 customers, 4 bookers, `lock=0`): I1 and I3 failed
//! on PostgreSQL and on KDS, with no conflict raised on either; `lock=1`
//! passed on all three. A conflict on a locking read is counted on that
//! row's axis (`organizations`, `operations`); on any other read, on
//! `read`, as the Python does.
//!
//! **Joins** are spelled with whole relation names: KDS refuses a bare
//! table alias (`FROM s2_freights f`) and Oracle refuses `AS` before one.
//!
//! **Phases**: `booking` - one op a terminal outcome (committed or refused
//! by the business), its latency every attempt's, retries included, as the
//! Python's `booking` phase; a failed booking is `r.error(<class>)`, an
//! abandoned one (out of retries) is counted in `meta.abandoned`. Each
//! statement's own latency (`begin`, the eight, `commit`, `rollback`) is
//! kept per statement kind and reported in `meta.booking_statements`, since
//! one phase holds one op kind. Then the manifest reporter's three reads,
//! each its own phase after an unrecorded warm-up pass: `manifest-scan`,
//! `voyage-rollup`, `customer-statement`.
//!
//! **Options** (`--set k=v`; defaults are `pg-s2-b1`'s arguments,
//! `bench/v3.5.0/results-pg-control-scenarios-v3.4.17-202-g481d101aa.md` §2):
//! `organizations` 300, `ships` 30, `operations` 300, `cargos` 4000,
//! `bookings` 3000 (0: time-bound), `seconds` 60 (a ceiling), `bookers` 1,
//! `contend` 1, `txn` 1, `capacity` cached|scan, `lock` auto, `max_retries`
//! 5, `max_fees` 0, `hot_routes` 6, `capacity_headroom` 1.0,
//! `credit_headroom` 1.0, `verify` 100 (I1 and I2 on 100 voyages, I3 on 100 customers, I4 on
//! 100 freights: 400 checks),
//! `manifest` 1, `manifest_passes` 5, `manifest_voyages` 20,
//! `manifest_customers` 10, `warmup` 50 (read-only bookings each booker runs
//! and rolls back before the phase).
//!
//! **Left out, and why**:
//! - the load's and the DDL's timed phases (`ddl`, `load-*`): the matched
//!   run compares the booking and the reporter only; the load here is the
//!   framework's `load`, 500 rows a transaction, untimed;
//! - engine-issued ids (KDS's omitted Keystone, PostgreSQL's `bigserial`
//!   with `RETURNING id`): the client names every key on every engine, since
//!   the `Engine` trait has no returned-id path Oracle could serve (no
//!   `RETURNING ... INTO`); freights and charges take ids from two counters
//!   shared by the bookers, so they ascend in commit-attempt order, and a
//!   rolled-back attempt burns its ids as an issued key does;
//! - `--fk` and `--cabin` (both off by default; the Cabin is KDS's alone);
//! - `--isolation` (above);
//! - `--schema-only`, `--load-only`, `--suffix`, `--sync`, `--echo`,
//!   `--server-log`, and the KDS driver's read probe (`probe_reads`): run
//!   management and a capability report, nothing measured;
//! - the reporter running *beside* the bookers: the KDS driver runs it in a
//!   second process every `--manifest-interval` seconds, the twin
//!   interleaves it between bookings on one connection, so the two
//!   references already disagree; here its three reads are their own
//!   phases after the bookings, over the whole ledger, so no booking's
//!   latency or server CPU carries a reporter's, and its op counts are a
//!   fixed `manifest_passes` rather than a function of the wall clock;
//! - the simulated business clock from wall time: the Python's `day` is the
//!   elapsed fraction of `--seconds`; here it is the booker's committed
//!   fraction of its target (or of `seconds` when `bookings=0`), so the
//!   values written do not depend on the engine's speed. `day` reaches only
//!   `booked_day`, `applied_day` and the fee windows, which are wider than
//!   the horizon;
//! - the text protocol: every statement is prepared once a connection and
//!   executed with bound values; the generated values come from this
//!   harness's `Rng`, not Python's `random`, so the data has the same
//!   distributions and not the same rows as the Python's.

use super::*;
use std::collections::HashMap;
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::Mutex;
use std::time::Duration;

// ---- reference data (the Python's constants, verbatim) -----------------------

const PORTS: i64 = 24;
const ROUTE_ANY: i64 = -1;
const CARGO_TYPES: [&str; 8] = ["dry", "reefer", "hazmat", "bulk", "liquid", "vehicle", "project", "container"];
const BASIS_FLAT: i64 = 0;
const BASIS_PER_CBM: i64 = 1;
const FEES: [(&str, i64, i64, i64); 12] = [
    ("terminal-handling", 101, 1, 1_450),
    ("bunker-adjustment", 102, 1, 900),
    ("currency-adjustment", 103, 2, 3),
    ("documentation", 104, 0, 4_500),
    ("seal", 105, 0, 800),
    ("hazmat-surcharge", 106, 1, 6_200),
    ("customs-clearance", 107, 0, 12_000),
    ("lashing", 108, 1, 700),
    ("demurrage-deposit", 109, 0, 30_000),
    ("wharfage", 110, 1, 350),
    ("security", 111, 0, 2_200),
    ("cargo-insurance", 112, 2, 8),
];
const BASE_RATE_PER_CBM: i64 = 2_500;
const RATE_JITTER: i64 = 400;
const COUNTRIES: [&str; 10] = ["KR", "US", "JP", "GB", "DE", "SG", "HK", "AU", "NL", "CN"];
const SHIP_TYPES: i64 = 6;
const ORG_TYPES: i64 = 4;
const CARGO_MIN_CBM: i64 = 5;
const CARGO_MAX_CBM: i64 = 400;
const MILLI: i64 = 1_000;
const CAPACITY_SPREAD: (f64, f64) = (0.5, 2.5);
const CREDIT_SPREAD: (f64, f64) = (0.4, 2.0);
const DECLARED_MIN: i64 = 10_000_00;
const DECLARED_MAX: i64 = 2_000_000_00;
const FEE_MULTIPLE: f64 = 2.2;
const CAPACITY_FLOOR_CBM: i64 = CARGO_MAX_CBM * 2 * MILLI;
const MAX_PER_CBM_FEES: i64 = 1_450 + 900 + 350 + 6_200 + 700;
const MAX_PER_MILLE: i64 = 3 + 8;
const CREDIT_FLOOR: i64 =
    3 * (CARGO_MAX_CBM * (BASE_RATE_PER_CBM + RATE_JITTER + MAX_PER_CBM_FEES) + DECLARED_MAX * MAX_PER_MILLE / 1000 + 50_000);
/// Epoch day of 2026-01-01: 56 years of 365 days and 14 leap days.
const DAY0: i64 = 20_454;
const RULE_WINDOW_DAYS: i64 = 3650;
const HORIZON_DAYS: i64 = 180;

/// Children first, so a drop never meets a dependant.
const TABLES: [&str; 8] =
    ["s2_charges", "s2_freights", "s2_recipes", "s2_fees", "s2_cargos", "s2_operations", "s2_ships", "s2_organizations"];

fn route_code(origin: i64, destination: i64) -> i64 {
    origin * PORTS + destination
}

fn uniform(rng: &mut Rng, (lo, hi): (f64, f64)) -> f64 {
    lo + (rng.next() >> 11) as f64 / (1u64 << 53) as f64 * (hi - lo)
}

fn shuffle<T>(rng: &mut Rng, v: &mut [T]) {
    for i in (1..v.len()).rev() {
        let j = rng.range(0, i as i64) as usize;
        v.swap(i, j);
    }
}

/// `k` distinct items of `v`, in draw order.
fn sample<T: Clone>(rng: &mut Rng, v: &[T], k: usize) -> Vec<T> {
    let mut c = v.to_vec();
    let k = k.min(c.len());
    for i in 0..k {
        let j = rng.range(i as i64, c.len() as i64 - 1) as usize;
        c.swap(i, j);
    }
    c.truncate(k);
    c
}

/// A `SUM` answer as an integer: no row, or NULL (SUM over no rows), is 0.
fn sum0(rows: &Rows) -> i64 {
    match rows.first().and_then(|r| r.first()) {
        None | Some(Val::Null) => 0,
        Some(v) => v.i(),
    }
}

#[derive(Clone, Copy)]
struct Op {
    id: i64,
    ship: i64,
    capacity: i64,
    route: i64,
}

#[derive(Clone, Copy)]
struct Cargo {
    id: i64,
    org: i64,
    ctype: i64,
    cbm: i64,
    value: i64,
}

struct Cfg {
    txn: bool,
    scan: bool,
    lock: bool,
    max_retries: u64,
    max_fees: usize,
    target: u64,
    seconds: f64,
}

/// The fees that apply to one booking, priced, by priority (the Python's
/// `charge_lines`).
fn charge_lines(recipes: &[[i64; 5]], fee_book: &HashMap<i64, (i64, i64)>, route: i64, day: i64, cbm: i64, value: i64, max_fees: usize) -> Vec<(i64, i64)> {
    let mut lines = Vec::new();
    for &[fee_id, rc, priority, from, to] in recipes {
        if rc != ROUTE_ANY && rc != route {
            continue;
        }
        if !(from <= day && day <= to) {
            continue;
        }
        let Some(&(basis, amount)) = fee_book.get(&fee_id) else { continue };
        let charged = match basis {
            BASIS_FLAT => amount,
            BASIS_PER_CBM => amount * (cbm / MILLI),
            _ => value * amount / 1000,
        };
        lines.push((priority, fee_id, charged));
    }
    lines.sort();
    if max_fees > 0 {
        lines.truncate(max_fees);
    }
    lines.into_iter().map(|(_, f, c)| (f, c)).collect()
}

#[derive(Clone, Copy)]
struct BookStmts {
    cargo: Stmt,
    credit: Stmt,
    capacity: Stmt,
    recipe: Stmt,
    freight: Stmt,
    charge: Stmt,
    op_upd: Stmt,
    org_upd: Stmt,
}

fn prepare_booking(e: &mut dyn Engine, cfg: &Cfg) -> BookStmts {
    let lk = if cfg.lock { " FOR UPDATE" } else { "" };
    let mut p = |sql: &str| e.prepare(sql).unwrap_or_else(|err| panic!("{err}"));
    BookStmts {
        cargo: p("SELECT org_id, cargo_type, cbm, declared_value FROM s2_cargos WHERE id = ?"),
        credit: p(&format!("SELECT credit_limit, outstanding FROM s2_organizations WHERE id = ?{lk}")),
        capacity: if cfg.scan {
            p("SELECT SUM(cbm) FROM s2_freights WHERE operation_id = ?")
        } else {
            p(&format!("SELECT booked_cbm, revenue FROM s2_operations WHERE id = ?{lk}"))
        },
        recipe: p("SELECT fee_id, route_code, priority, valid_from, valid_to FROM s2_recipes WHERE cargo_type = ?"),
        freight: p(
            "INSERT INTO s2_freights (id, operation_id, ship_id, cargo_id, cbm, price_per_cbm, booked_day, status) \
             VALUES (?, ?, ?, ?, ?, ?, ?, 0)",
        ),
        charge: p("INSERT INTO s2_charges (id, freight_id, fee_id, amount, applied_day) VALUES (?, ?, ?, ?, ?)"),
        op_upd: if cfg.scan {
            p("UPDATE s2_operations SET booked_cbm = ? WHERE id = ?")
        } else {
            p("UPDATE s2_operations SET booked_cbm = ?, revenue = ? WHERE id = ?")
        },
        org_upd: p("UPDATE s2_organizations SET outstanding = ? WHERE id = ?"),
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Outcome {
    Committed,
    RejectedCapacity,
    RejectedCredit,
    Conflicted,
    Failed,
}

const AXES: [&str; 4] = ["operations", "organizations", "read", "commit"];

/// What one booker hands back: its tally, its statement latencies, and the
/// belief half of verify.
#[derive(Default)]
struct BookerOut {
    committed: u64,
    rejected_capacity: u64,
    rejected_credit: u64,
    conflicted: u64,
    failed: u64,
    retries: u64,
    abandoned: u64,
    exhausted: bool,
    pool_left: usize,
    axes: BTreeMap<&'static str, u64>,
    failures: BTreeMap<String, u64>,
    first_failure: Option<String>,
    lat: BTreeMap<&'static str, Vec<u64>>,
    booked_cbm: HashMap<i64, i64>,
    outstanding: HashMap<i64, i64>,
    freight_charges: HashMap<i64, i64>,
}

struct Shared<'a> {
    cfg: &'a Cfg,
    fee_book: &'a HashMap<i64, (i64, i64)>,
    next_freight: AtomicI64,
    next_charge: AtomicI64,
    deadline: Instant,
}

struct Booker<'a> {
    e: &'a mut dyn Engine,
    st: BookStmts,
    sh: &'a Shared<'a>,
    rng: Rng,
    ops: Vec<Op>,
    cargos: Vec<Cargo>,
    pool: Vec<usize>,
    rows: Rows,
    out: BookerOut,
}

impl<'a> Booker<'a> {
    fn timed<T>(&mut self, name: &'static str, f: impl FnOnce(&mut dyn Engine, &mut Rows) -> DbResult<T>) -> DbResult<T> {
        let t0 = Instant::now();
        let r = f(&mut *self.e, &mut self.rows);
        self.out.lat.entry(name).or_default().push(t0.elapsed().as_nanos() as u64);
        r
    }
    fn query(&mut self, name: &'static str, s: Stmt, p: &[Val]) -> DbResult<()> {
        self.timed(name, |e, rows| e.query(s, p, rows))
    }
    fn exec(&mut self, name: &'static str, s: Stmt, p: &[Val]) -> DbResult<u64> {
        self.timed(name, |e, _| e.exec(s, p))
    }

    /// Rolls an attempt back and returns its cargo unless it shipped or its
    /// customer can never afford it (the Python's `finish`).
    fn finish(&mut self, o: Outcome, opened: bool, idx: usize) -> Outcome {
        if opened {
            let _ = self.timed("rollback", |e, _| e.rollback());
        }
        if o != Outcome::Committed && o != Outcome::RejectedCredit {
            self.pool.push(idx);
        }
        o
    }
    fn refused(&mut self, err: DbError, axis: &'static str, opened: bool, idx: usize) -> Outcome {
        let o = if err.class == ErrClass::Conflict {
            *self.out.axes.entry(axis).or_default() += 1;
            Outcome::Conflicted
        } else {
            *self.out.failures.entry(format!("{:?}", err.class)).or_default() += 1;
            if self.out.first_failure.is_none() {
                self.out.first_failure = Some(err.msg.clone());
            }
            Outcome::Failed
        };
        self.finish(o, opened, idx)
    }

    /// One booking attempt; None when the cargo pool is empty.
    fn book_once(&mut self, day: i64) -> Option<Outcome> {
        let cfg = self.sh.cfg;
        let st = self.st;
        let idx = self.pool.pop()?;
        let c = self.cargos[idx];
        let op = self.ops[self.rng.range(0, self.ops.len() as i64 - 1) as usize];
        let lock_axis = |row: &'static str| if cfg.lock { row } else { "read" };

        let opened = cfg.txn;
        if opened {
            if let Err(err) = self.timed("begin", |e, _| e.begin()) {
                return Some(self.refused(err, "read", false, idx));
            }
        }
        // 1. the cargo
        if let Err(err) = self.query("cargo-lookup", st.cargo, &[Val::I64(c.id)]) {
            return Some(self.refused(err, "read", opened, idx));
        }
        if self.rows.is_empty() {
            return Some(self.refused(other(format!("cargo {} not found", c.id)), "read", opened, idx));
        }
        let org = self.rows[0][0].i();
        // 2. the customer's credit
        if let Err(err) = self.query("credit-lookup", st.credit, &[Val::I64(org)]) {
            return Some(self.refused(err, lock_axis("organizations"), opened, idx));
        }
        if self.rows.is_empty() {
            return Some(self.refused(other(format!("organization {org} not found")), "read", opened, idx));
        }
        let (credit_limit, outstanding) = (self.rows[0][0].i(), self.rows[0][1].i());
        // 3. the capacity
        if let Err(err) = self.query("capacity-read", st.capacity, &[Val::I64(op.id)]) {
            return Some(self.refused(err, lock_axis("operations"), opened, idx));
        }
        let (booked, revenue) = if cfg.scan {
            (sum0(&self.rows), 0)
        } else {
            match self.rows.first() {
                Some(r) => (r[0].i(), r[1].i()),
                None => (0, 0),
            }
        };
        // 4. the pricing rules
        if let Err(err) = self.query("recipe-read", st.recipe, &[Val::I64(c.ctype)]) {
            return Some(self.refused(err, "read", opened, idx));
        }
        let recipes: Vec<[i64; 5]> = self.rows.iter().map(|r| [r[0].i(), r[1].i(), r[2].i(), r[3].i(), r[4].i()]).collect();

        if booked + c.cbm > op.capacity {
            return Some(self.finish(Outcome::RejectedCapacity, opened, idx));
        }
        let rate = BASE_RATE_PER_CBM + self.rng.range(-RATE_JITTER, RATE_JITTER);
        let lines = charge_lines(&recipes, self.sh.fee_book, op.route, day, c.cbm, c.value, cfg.max_fees);
        let total = (c.cbm / MILLI) * rate + lines.iter().map(|l| l.1).sum::<i64>();
        if outstanding + total > credit_limit {
            return Some(self.finish(Outcome::RejectedCredit, opened, idx));
        }
        // 5. the order line
        let fid = self.sh.next_freight.fetch_add(1, Ordering::Relaxed);
        let p = [fid, op.id, op.ship, c.id, c.cbm, rate, day].map(Val::I64);
        if let Err(err) = self.exec("freight-insert", st.freight, &p) {
            return Some(self.refused(err, "read", opened, idx));
        }
        // 6. what it was charged
        for &(fee, charged) in &lines {
            let cid = self.sh.next_charge.fetch_add(1, Ordering::Relaxed);
            if let Err(err) = self.exec("charge-insert", st.charge, &[cid, fid, fee, charged, day].map(Val::I64)) {
                return Some(self.refused(err, "read", opened, idx));
            }
        }
        // 7. the voyage
        let r = if cfg.scan {
            self.exec("operation-update", st.op_upd, &[booked + c.cbm, op.id].map(Val::I64))
        } else {
            self.exec("operation-update", st.op_upd, &[booked + c.cbm, revenue + total, op.id].map(Val::I64))
        };
        if let Err(err) = r {
            return Some(self.refused(err, "operations", opened, idx));
        }
        // 8. the customer
        if let Err(err) = self.exec("org-update", st.org_upd, &[outstanding + total, org].map(Val::I64)) {
            return Some(self.refused(err, "organizations", opened, idx));
        }
        if opened {
            if let Err(err) = self.timed("commit", |e, _| e.commit()) {
                let _ = self.e.rollback();
                return Some(self.refused(err, "commit", false, idx));
            }
        }
        self.out.booked_cbm.insert(op.id, booked + c.cbm);
        self.out.outstanding.insert(org, outstanding + total);
        self.out.freight_charges.insert(fid, lines.len() as i64);
        Some(Outcome::Committed)
    }

    /// The Python's `run_bookings`: until the target or the deadline, each
    /// conflicted attempt re-driven up to `max_retries` times.
    fn run(&mut self, r: &mut Recorder) {
        let cfg = self.sh.cfg;
        let started = Instant::now();
        while Instant::now() < self.sh.deadline {
            if cfg.target > 0 && self.out.committed >= cfg.target {
                break;
            }
            let frac = if cfg.target > 0 {
                self.out.committed as f64 / cfg.target as f64
            } else {
                started.elapsed().as_secs_f64() / cfg.seconds.max(1e-9)
            };
            let day = DAY0 + ((frac * HORIZON_DAYS as f64) as i64).min(HORIZON_DAYS - 1);
            let t0 = Instant::now();
            let mut outcome = None;
            for attempt in 0..=cfg.max_retries {
                if attempt > 0 {
                    self.out.retries += 1;
                }
                outcome = self.book_once(day);
                if outcome != Some(Outcome::Conflicted) {
                    break;
                }
                self.out.conflicted += 1;
            }
            match outcome {
                None => {
                    self.out.exhausted = true;
                    break;
                }
                Some(Outcome::Conflicted) => self.out.abandoned += 1,
                Some(Outcome::Failed) => {
                    self.out.failed += 1;
                    r.error("failed");
                }
                Some(o) => {
                    match o {
                        Outcome::Committed => self.out.committed += 1,
                        Outcome::RejectedCapacity => self.out.rejected_capacity += 1,
                        _ => self.out.rejected_credit += 1,
                    }
                    r.add(t0);
                }
            }
        }
        self.out.pool_left = self.pool.len();
    }
}

/// The Python's `partition`: cargos split in both modes; under `contend`
/// every booker shares every voyage and customer, otherwise both are
/// disjoint and each booker takes only its own customers' cargos.
fn partition(ops: &[Op], orgs: &[(i64, i64)], cargos: &[Cargo], bookers: usize, contend: bool) -> Vec<(Vec<Op>, Vec<Cargo>)> {
    (0..bookers)
        .map(|i| {
            if contend {
                (ops.to_vec(), cargos.iter().skip(i).step_by(bookers).copied().collect())
            } else {
                let owned: std::collections::HashSet<i64> = orgs.iter().skip(i).step_by(bookers).map(|o| o.0).collect();
                (ops.iter().skip(i).step_by(bookers).copied().collect(), cargos.iter().filter(|c| owned.contains(&c.org)).copied().collect())
            }
        })
        .collect()
}

fn opt_f(ctx: &Ctx, k: &str, default: f64) -> f64 {
    let v = ctx.opt_s(k, &default.to_string());
    v.parse().unwrap_or_else(|_| panic!("--set {k}={v}: not a number"))
}

pub fn run(ctx: &mut Ctx) {
    let k = ctx.kind;
    let n_orgs = ctx.opt("organizations", 300) as i64;
    let n_ships = ctx.opt("ships", 30) as i64;
    let n_ops = ctx.opt("operations", 300) as i64;
    let n_cargos = ctx.opt("cargos", 4000) as i64;
    let bookings = ctx.opt("bookings", 3000);
    let bookers = ctx.opt("bookers", 1).max(1) as usize;
    let contend = ctx.opt("contend", 1) != 0;
    let txn = ctx.opt("txn", 1) != 0;
    let capacity_mode = ctx.opt_s("capacity", "cached");
    let scan = match capacity_mode.as_str() {
        "cached" => false,
        "scan" => true,
        m => panic!("--set capacity={m}: cached or scan"),
    };
    let lock = match ctx.opt_s("lock", "auto").as_str() {
        "auto" => bookers > 1 && contend && !scan && txn,
        "1" => true,
        "0" => false,
        v => panic!("--set lock={v}: auto, 0 or 1"),
    };
    assert!(!(lock && !txn), "lock=1 needs txn=1: a locking read outside a transaction holds nothing (KDS refuses it)");
    assert!(!(lock && scan), "lock=1 with capacity=scan: the scan reads no operations row to lock");
    let cfg = Cfg {
        txn,
        scan,
        lock,
        max_retries: ctx.opt("max_retries", 5),
        max_fees: ctx.opt("max_fees", 0) as usize,
        target: if bookings > 0 { bookings.div_ceil(bookers as u64) } else { 0 },
        seconds: opt_f(ctx, "seconds", 60.0),
    };
    let hot_routes = ctx.opt("hot_routes", 6);
    let capacity_headroom = opt_f(ctx, "capacity_headroom", 1.0);
    let credit_headroom = opt_f(ctx, "credit_headroom", 1.0);
    let verify_n = ctx.opt("verify", 100) as usize;
    let warmup = ctx.opt("warmup", 50);

    // ---- DDL ----------------------------------------------------------------
    let mut e = ctx.connect();
    for t in TABLES {
        drop_table(e.as_mut(), t);
    }
    let defs: [Table; 8] = [
        Table {
            name: "s2_organizations",
            cols: &[
                ("id", Ty::I64),
                ("org_code", Ty::Str(16)),
                ("name", Ty::Str(16)),
                ("country", Ty::I32),
                ("org_type", Ty::I32),
                ("credit_limit", Ty::I64),
                ("outstanding", Ty::I64),
                ("tier", Ty::I32),
                ("contact", Ty::Str(32)),
                ("registered_day", Ty::I32),
                ("status", Ty::I32),
            ],
            store: Store::Btree,
            refs: &[],
        },
        Table {
            name: "s2_ships",
            cols: &[
                ("id", Ty::I64),
                ("imo", Ty::Str(16)),
                ("name", Ty::Str(16)),
                ("ship_type", Ty::I32),
                ("capacity_cbm", Ty::I32),
                ("dwt", Ty::I64),
                ("built_year", Ty::I32),
                ("flag", Ty::Str(8)),
                ("owner_id", Ty::I64),
                ("home_port", Ty::I32),
                ("status", Ty::I32),
            ],
            store: Store::Heap,
            refs: &[],
        },
        Table {
            name: "s2_operations",
            cols: &[
                ("id", Ty::I64),
                ("ship_id", Ty::I64),
                ("origin", Ty::I32),
                ("destination", Ty::I32),
                ("depart_day", Ty::I32),
                ("arrive_day", Ty::I32),
                ("status", Ty::I32),
                ("booked_cbm", Ty::I32),
                ("revenue", Ty::I64),
            ],
            store: Store::Btree,
            refs: &[],
        },
        Table {
            name: "s2_cargos",
            cols: &[
                ("id", Ty::I64),
                ("org_id", Ty::I64),
                ("cargo_type", Ty::I32),
                ("weight_kg", Ty::I64),
                ("cbm", Ty::I32),
                ("hazmat", Ty::I32),
                ("declared_value", Ty::I64),
                ("origin", Ty::I32),
                ("destination", Ty::I32),
                ("ready_day", Ty::I32),
            ],
            store: Store::Btree,
            refs: &[],
        },
        Table {
            name: "s2_fees",
            cols: &[
                ("id", Ty::I64),
                ("fee_name", Ty::Str(24)),
                ("fee_code", Ty::I32),
                ("basis", Ty::I32),
                ("amount", Ty::I64),
                ("valid_from", Ty::I32),
                ("valid_to", Ty::I32),
            ],
            store: Store::Heap,
            refs: &[],
        },
        Table {
            name: "s2_recipes",
            cols: &[
                ("id", Ty::I64),
                ("cargo_type", Ty::I32),
                ("route_code", Ty::I32),
                ("fee_id", Ty::I64),
                ("priority", Ty::I32),
                ("valid_from", Ty::I32),
                ("valid_to", Ty::I32),
            ],
            store: Store::Heap,
            refs: &[],
        },
        Table {
            name: "s2_freights",
            cols: &[
                ("id", Ty::I64),
                ("operation_id", Ty::I64),
                ("ship_id", Ty::I64),
                ("cargo_id", Ty::I64),
                ("cbm", Ty::I32),
                ("price_per_cbm", Ty::I64),
                ("booked_day", Ty::I32),
                ("status", Ty::I32),
            ],
            store: Store::Heap,
            refs: &[],
        },
        Table {
            name: "s2_charges",
            cols: &[("id", Ty::I64), ("freight_id", Ty::I64), ("fee_id", Ty::I64), ("amount", Ty::I64), ("applied_day", Ty::I32)],
            store: Store::Heap,
            refs: &[],
        },
    ];
    for t in &defs {
        let sql = create_table(k, t);
        e.run(&sql).unwrap_or_else(|err| panic!("{err}"));
    }
    for sql in [
        "CREATE INDEX s2_freights_op ON s2_freights (operation_id)",
        "CREATE INDEX s2_recipes_ct ON s2_recipes (cargo_type)",
    ] {
        e.run(sql).unwrap_or_else(|err| panic!("{err}"));
    }
    ctx.set_meta(
        "relation_kinds",
        defs.iter().map(|t| (t.name.to_string(), t.store)).collect::<BTreeMap<_, _>>(),
    );

    // ---- reference data (the Python's generators, in its order) -------------
    let mut rng = Rng::new(ctx.seed);
    let mean_cbm = (CARGO_MIN_CBM + CARGO_MAX_CBM) as f64 / 2.0 * MILLI as f64;
    let cbm_per_voyage = mean_cbm * n_cargos as f64 / n_ops.max(1) as f64 * capacity_headroom;
    let mean_total = mean_cbm / MILLI as f64 * BASE_RATE_PER_CBM as f64 * FEE_MULTIPLE;
    let spend_per_customer = mean_total * n_cargos as f64 / n_orgs.max(1) as f64 * credit_headroom;

    let mut orgs = Vec::new(); // (id, credit_limit)
    let mut rows = Vec::new();
    for i in 0..n_orgs {
        let limit = CREDIT_FLOOR.max((spend_per_customer * uniform(&mut rng, CREDIT_SPREAD)) as i64);
        let id = i + 1;
        orgs.push((id, limit));
        rows.push(vec![
            Val::I64(id),
            Val::Str(format!("ORG{i:06}")),
            Val::Str(format!("org{i:06}")),
            Val::I64(rng.range(0, COUNTRIES.len() as i64 - 1)),
            Val::I64(rng.range(0, ORG_TYPES - 1)),
            Val::I64(limit),
            Val::I64(0),
            Val::I64(rng.range(0, 3)),
            Val::Str(format!("ops{i:06}@example.test")),
            Val::I64(DAY0 - rng.range(0, 3650)),
            Val::I64(0),
        ]);
    }
    load(e.as_mut(), "s2_organizations", 11, rows.into_iter(), 500);

    let mut ships = Vec::new(); // (id, capacity)
    let mut rows = Vec::new();
    for i in 0..n_ships {
        let capacity = CAPACITY_FLOOR_CBM.max((cbm_per_voyage * uniform(&mut rng, CAPACITY_SPREAD)) as i64);
        let id = i + 1;
        ships.push((id, capacity));
        rows.push(vec![
            Val::I64(id),
            Val::Str(format!("IMO{}", 9_000_000 + i)),
            Val::Str(format!("vessel{i:05}")),
            Val::I64(rng.range(0, SHIP_TYPES - 1)),
            Val::I64(capacity),
            Val::I64(rng.range(10_000, 200_000)),
            Val::I64(rng.range(1995, 2025)),
            Val::Str(COUNTRIES[rng.range(0, COUNTRIES.len() as i64 - 1) as usize].to_string()),
            Val::I64(0),
            Val::I64(rng.range(0, PORTS - 1)),
            Val::I64(0),
        ]);
    }
    load(e.as_mut(), "s2_ships", 11, rows.into_iter(), 500);

    let mut ops = Vec::new();
    let mut rows = Vec::new();
    for i in 0..n_ops {
        let (ship, capacity) = ships[rng.range(0, ships.len() as i64 - 1) as usize];
        let origin = rng.range(0, PORTS - 1);
        let destination = (origin + rng.range(1, PORTS - 1)) % PORTS;
        let depart = DAY0 + rng.range(0, 180);
        let id = i + 1;
        ops.push(Op { id, ship, capacity, route: route_code(origin, destination) });
        let arrive = depart + rng.range(3, 40);
        rows.push([id, ship, origin, destination, depart, arrive, 0, 0, 0].map(Val::I64).to_vec());
    }
    load(e.as_mut(), "s2_operations", 9, rows.into_iter(), 500);

    let mut fee_ids = HashMap::new(); // fee_code -> fee_id
    let mut fee_book = HashMap::new(); // fee_id -> (basis, amount)
    let mut rows = Vec::new();
    for (i, (name, code, basis, amount)) in FEES.iter().enumerate() {
        let id = i as i64 + 1;
        fee_ids.insert(*code, id);
        fee_book.insert(id, (*basis, *amount));
        rows.push(vec![
            Val::I64(id),
            Val::Str(name.to_string()),
            Val::I64(*code),
            Val::I64(*basis),
            Val::I64(*amount),
            Val::I64(DAY0 - RULE_WINDOW_DAYS),
            Val::I64(DAY0 + RULE_WINDOW_DAYS),
        ]);
    }
    load(e.as_mut(), "s2_fees", 7, rows.into_iter(), 500);

    let hot: Vec<i64> = (0..hot_routes).map(|_| route_code(rng.range(0, PORTS - 1), rng.range(0, PORTS - 1))).collect();
    let per_type = |t: &str| -> &'static [i64] {
        match t {
            "hazmat" => &[106, 107, 111],
            "reefer" => &[105, 112],
            "liquid" => &[108, 112],
            "vehicle" => &[108, 105],
            "project" => &[108, 109],
            "bulk" => &[109],
            "container" => &[105],
            _ => &[],
        }
    };
    let mut rows = Vec::new();
    for (ct, tname) in CARGO_TYPES.iter().enumerate() {
        let mut codes: Vec<(i64, i64)> = [101, 102, 104, 110].iter().map(|&c| (c, ROUTE_ANY)).collect();
        codes.extend(per_type(tname).iter().map(|&c| (c, ROUTE_ANY)));
        codes.extend(hot.iter().map(|&r| (103, r)));
        for (priority, (code, route)) in codes.into_iter().enumerate() {
            let id = rows.len() as i64 + 1;
            let fee_id = fee_ids[&code];
            rows.push(
                [id, ct as i64, route, fee_id, priority as i64, DAY0 - RULE_WINDOW_DAYS, DAY0 + RULE_WINDOW_DAYS].map(Val::I64).to_vec(),
            );
        }
    }
    let n_recipes = rows.len();
    load(e.as_mut(), "s2_recipes", 7, rows.into_iter(), 500);

    let mut cargos = Vec::new();
    let mut rows = Vec::new();
    for i in 0..n_cargos {
        let (org, _) = orgs[rng.range(0, orgs.len() as i64 - 1) as usize];
        let ctype = rng.range(0, CARGO_TYPES.len() as i64 - 1);
        let cbm = rng.range(CARGO_MIN_CBM, CARGO_MAX_CBM) * MILLI;
        let value = rng.range(DECLARED_MIN, DECLARED_MAX);
        let origin = rng.range(0, PORTS - 1);
        let destination = (origin + rng.range(1, PORTS - 1)) % PORTS;
        let id = i + 1;
        cargos.push(Cargo { id, org, ctype, cbm, value });
        let weight = rng.range(500, 40_000);
        let ready = DAY0 + rng.range(0, 180);
        rows.push([id, org, ctype, weight, cbm, (CARGO_TYPES[ctype as usize] == "hazmat") as i64, value, origin, destination, ready].map(Val::I64).to_vec());
    }
    load(e.as_mut(), "s2_cargos", 10, rows.into_iter(), 500);
    analyze(e.as_mut(), &TABLES);
    ctx.set_meta(
        "loaded",
        serde_json::json!({"organizations": n_orgs, "ships": n_ships, "operations": n_ops, "fees": FEES.len(),
                           "recipes": n_recipes, "cargos": n_cargos}),
    );

    // ---- the bookers ----------------------------------------------------------
    let slices = partition(&ops, &orgs, &cargos, bookers, contend);
    if let Some(i) = slices.iter().position(|(o, c)| o.is_empty() || c.is_empty()) {
        panic!("bookers={bookers} contend=0 leaves booker {i} with no voyages or no cargo: raise operations/organizations/cargos");
    }
    let mut engines: Vec<Box<dyn Engine>> = (0..bookers).map(|_| ctx.connect()).collect();
    let stmts: Vec<BookStmts> = engines.iter_mut().map(|b| prepare_booking(b.as_mut(), &cfg)).collect();
    // Unrecorded warm-up: the four reads of `warmup` bookings a connection,
    // rolled back, nothing written.
    let mut wr = Rng::new(ctx.seed.wrapping_add(7000));
    for (b, st) in engines.iter_mut().zip(&stmts) {
        let mut rows = Rows::new();
        for _ in 0..warmup {
            let c = cargos[wr.range(0, cargos.len() as i64 - 1) as usize];
            let op = ops[wr.range(0, ops.len() as i64 - 1) as usize];
            if txn {
                b.begin().unwrap();
            }
            b.query(st.cargo, &[Val::I64(c.id)], &mut rows).unwrap_or_else(|err| panic!("warm-up: {err}"));
            b.query(st.credit, &[Val::I64(c.org)], &mut rows).unwrap_or_else(|err| panic!("warm-up: {err}"));
            b.query(st.capacity, &[Val::I64(op.id)], &mut rows).unwrap_or_else(|err| panic!("warm-up: {err}"));
            b.query(st.recipe, &[Val::I64(c.ctype)], &mut rows).unwrap_or_else(|err| panic!("warm-up: {err}"));
            if txn {
                b.rollback().unwrap();
            }
        }
    }

    let shared = Shared {
        cfg: &cfg,
        fee_book: &fee_book,
        next_freight: AtomicI64::new(1),
        next_charge: AtomicI64::new(1),
        deadline: Instant::now() + Duration::from_secs_f64(cfg.seconds),
    };
    let outs: Mutex<Vec<(usize, BookerOut)>> = Mutex::new(Vec::new());
    let seed = ctx.seed;
    let booking = ctx.phase("booking", &mut engines, |i, e, r| {
        let (ops_i, cargos_i) = &slices[i];
        let mut rng = Rng::new(seed.wrapping_add(1000 + i as u64));
        let mut pool: Vec<usize> = (0..cargos_i.len()).collect();
        shuffle(&mut rng, &mut pool);
        let mut b = Booker {
            e,
            st: stmts[i],
            sh: &shared,
            rng,
            ops: ops_i.clone(),
            cargos: cargos_i.clone(),
            pool,
            rows: Rows::new(),
            out: BookerOut::default(),
        };
        b.run(r);
        outs.lock().unwrap().push((i, b.out));
    });
    drop(engines);

    let mut outs = outs.into_inner().unwrap();
    outs.sort_by_key(|o| o.0);
    let mut lat: BTreeMap<&'static str, Vec<u64>> = BTreeMap::new();
    let mut axes: BTreeMap<&'static str, u64> = AXES.iter().map(|a| (*a, 0)).collect();
    let mut failures: BTreeMap<String, u64> = BTreeMap::new();
    let (mut committed, mut rej_cap, mut rej_credit, mut conflicted, mut failed, mut retries, mut abandoned) = (0, 0, 0, 0, 0, 0, 0);
    let mut exhausted = false;
    let mut pool_left = 0;
    let mut first_failure = None;
    let mut booked_cbm: HashMap<i64, i64> = HashMap::new();
    let mut outstanding: HashMap<i64, i64> = HashMap::new();
    let mut freight_charges: HashMap<i64, i64> = HashMap::new();
    let mut per_booker = Vec::new();
    for (_, o) in outs {
        committed += o.committed;
        rej_cap += o.rejected_capacity;
        rej_credit += o.rejected_credit;
        conflicted += o.conflicted;
        failed += o.failed;
        retries += o.retries;
        abandoned += o.abandoned;
        exhausted |= o.exhausted;
        pool_left += o.pool_left;
        per_booker.push(o.committed);
        for (a, n) in o.axes {
            *axes.entry(a).or_default() += n;
        }
        for (a, n) in o.failures {
            *failures.entry(a).or_default() += n;
        }
        if first_failure.is_none() {
            first_failure = o.first_failure;
        }
        for (name, v) in o.lat {
            lat.entry(name).or_default().extend(v);
        }
        booked_cbm.extend(o.booked_cbm);
        outstanding.extend(o.outstanding);
        freight_charges.extend(o.freight_charges);
    }
    ctx.set_meta(
        "booking_statements",
        lat.iter().map(|(n, v)| (n.to_string(), crate::stats::latency(v))).collect::<BTreeMap<_, _>>(),
    );
    ctx.set_meta(
        "outcomes",
        serde_json::json!({"committed": committed, "rejected-capacity": rej_cap, "rejected-credit": rej_credit,
                           "conflicted": conflicted, "failed": failed}),
    );
    ctx.set_meta("axes", &axes);
    ctx.set_meta("retries", retries);
    ctx.set_meta("abandoned", abandoned);
    ctx.set_meta("failures", &failures);
    ctx.set_meta("first_failure", &first_failure);
    ctx.set_meta("per_booker", &per_booker);
    ctx.set_meta("cargo_pool_exhausted", exhausted);
    ctx.set_meta("cargo_pool_left", pool_left);
    ctx.set_meta("tps", if booking.wall_s > 0.0 { committed as f64 / booking.wall_s } else { 0.0 });
    ctx.set_meta("freights", freight_charges.len());
    ctx.set_meta("charges", freight_charges.values().sum::<i64>());
    ctx.set_meta(
        "mode",
        serde_json::json!({"txn": txn, "capacity": capacity_mode, "lock": lock, "bookers": bookers, "contend": contend,
                           "max_retries": cfg.max_retries, "max_fees": cfg.max_fees, "bookings": bookings,
                           "hot_routes": hot_routes, "capacity_headroom": capacity_headroom,
                           "credit_headroom": credit_headroom,
                           "isolation": "each engine's default: READ COMMITTED on all three"}),
    );
    eprintln!(
        "bookings: committed {committed}, rejected-capacity {rej_cap}, rejected-credit {rej_credit}, conflicted {conflicted} \
         ({retries} retries, {abandoned} abandoned), failed {failed}, axes {axes:?}"
    );
    if let Some(f) = &first_failure {
        eprintln!("first failed booking: {f}");
    }
    ctx.check("bookings-without-failure", failed == 0, format!("{failed} failed booking(s), first: {first_failure:?}"));

    // ---- the reporter: three reads, each its own phase ------------------------
    if ctx.opt("manifest", 1) != 0 {
        let passes = ctx.opt("manifest_passes", 5) as usize;
        let voyages = ctx.opt("manifest_voyages", 20) as usize;
        let customers = ctx.opt("manifest_customers", 10) as usize;
        let mut mr = Rng::new(ctx.seed.wrapping_add(9000));
        let op_ids: Vec<i64> = ops.iter().map(|o| o.id).collect();
        let org_ids: Vec<i64> = orgs.iter().map(|o| o.0).collect();
        // One warm-up pass, then `passes` recorded ones.
        let voyage_draws: Vec<Vec<i64>> = (0..=passes).map(|_| sample(&mut mr, &op_ids, voyages)).collect();
        let customer_draws: Vec<Vec<i64>> = (0..=passes).map(|_| sample(&mut mr, &org_ids, customers)).collect();
        let scan_s = e.prepare("SELECT * FROM s2_freights WHERE operation_id = ?").unwrap();
        let rollup_s = e.prepare("SELECT status, COUNT(*), SUM(cbm) FROM s2_freights WHERE operation_id = ? GROUP BY status").unwrap();
        let stmt_s = e
            .prepare(
                "SELECT s2_cargos.org_id, SUM(s2_freights.cbm) FROM s2_freights JOIN s2_cargos \
                 ON s2_freights.cargo_id = s2_cargos.id WHERE s2_cargos.org_id = ? GROUP BY s2_cargos.org_id",
            )
            .unwrap();
        let mut engines = vec![e];
        for (name, s, draws) in
            [("manifest-scan", scan_s, &voyage_draws), ("voyage-rollup", rollup_s, &voyage_draws), ("customer-statement", stmt_s, &customer_draws)]
        {
            let mut rows = Rows::new();
            for &id in &draws[0] {
                engines[0].query(s, &[Val::I64(id)], &mut rows).unwrap_or_else(|err| panic!("{name} warm-up: {err}"));
            }
            ctx.phase(name, &mut engines, |_, e, r| {
                let mut rows = Rows::new();
                for &id in draws[1..].iter().flatten() {
                    let t0 = Instant::now();
                    match e.query(s, &[Val::I64(id)], &mut rows) {
                        Ok(()) => {
                            r.add(t0);
                            r.rows += rows.len() as u64;
                        }
                        Err(err) => r.error(&format!("{:?}", err.class)),
                    }
                }
            });
        }
        e = engines.pop().unwrap();
    }

    // ---- verification (§4's I1-I4, on a sample) ---------------------------------
    if verify_n > 0 {
        let p = |e: &mut Box<dyn Engine>, sql: &str| e.prepare(sql).unwrap_or_else(|err| panic!("{err}"));
        let ledger_s = p(&mut e, "SELECT SUM(cbm) FROM s2_freights WHERE operation_id = ?");
        let column_s = p(&mut e, "SELECT booked_cbm FROM s2_operations WHERE id = ?");
        let freights_s = p(
            &mut e,
            "SELECT s2_freights.id, s2_freights.cbm, s2_freights.price_per_cbm FROM s2_freights JOIN s2_cargos \
             ON s2_freights.cargo_id = s2_cargos.id WHERE s2_cargos.org_id = ?",
        );
        let owed_s = p(&mut e, "SELECT outstanding FROM s2_organizations WHERE id = ?");
        let charged_s = p(&mut e, "SELECT SUM(amount) FROM s2_charges WHERE freight_id = ?");
        let lines_s = p(&mut e, "SELECT id FROM s2_charges WHERE freight_id = ?");
        let capacity: HashMap<i64, i64> = ops.iter().map(|o| (o.id, o.capacity)).collect();
        let mut vr = Rng::new(ctx.seed.wrapping_add(5000));
        let (mut a, mut b) = (Rows::new(), Rows::new());
        // (checks, failures, first failure, unanswered, first unanswered) per invariant
        let mut tally: BTreeMap<&str, (u64, u64, Option<String>, u64, Option<String>)> = BTreeMap::new();
        let mut note = |inv: &'static str, ok: Option<bool>, what: String| {
            let t = tally.entry(inv).or_default();
            match ok {
                Some(true) => t.0 += 1,
                Some(false) => {
                    t.0 += 1;
                    t.1 += 1;
                    t.2.get_or_insert(what);
                }
                None => {
                    t.3 += 1;
                    t.4.get_or_insert(what);
                }
            }
        };

        let mut booked: Vec<i64> = booked_cbm.iter().filter(|(_, &v)| v > 0).map(|(&k, _)| k).collect();
        booked.sort_unstable();
        for op in sample(&mut vr, &booked, verify_n) {
            let r1 = e.query(ledger_s, &[Val::I64(op)], &mut a);
            let r2 = e.query(column_s, &[Val::I64(op)], &mut b);
            if let Some(err) = r1.err().or(r2.err()) {
                note("I1", None, format!("operation {op}: {err}"));
                note("I2", None, format!("operation {op}: {err}"));
                continue;
            }
            if b.is_empty() {
                continue;
            }
            let (ledger, column) = (sum0(&a), b[0][0].i());
            // I1: the derived column against its ledger, and - only where the
            // slices are disjoint - against what the one booker wrote.
            let i1 = if ledger != column {
                Err(format!("operation {op}: booked_cbm={column}, SUM(freights.cbm)={ledger}"))
            } else if !contend && column != booked_cbm[&op] {
                Err(format!("operation {op}: stored {column}, driver wrote {} - a write was lost", booked_cbm[&op]))
            } else {
                Ok(())
            };
            match i1 {
                Ok(()) => note("I1", Some(true), String::new()),
                Err(m) => note("I1", Some(false), m),
            }
            // I2: within the ship's capacity.
            note("I2", Some(column <= capacity[&op]), format!("operation {op}: booked_cbm={column} over capacity {}", capacity[&op]));
        }

        let mut owing: Vec<i64> = outstanding.iter().filter(|(_, &v)| v > 0).map(|(&k, _)| k).collect();
        owing.sort_unstable();
        for org in sample(&mut vr, &owing, verify_n) {
            let r1 = e.query(freights_s, &[Val::I64(org)], &mut a);
            let r2 = e.query(owed_s, &[Val::I64(org)], &mut b);
            if let Some(err) = r1.err().or(r2.err()) {
                note("I3", None, format!("organization {org}: {err}"));
                continue;
            }
            if b.is_empty() {
                continue;
            }
            let stored = b[0][0].i();
            let freights: Vec<(i64, i64, i64)> = a.iter().map(|r| (r[0].i(), r[1].i(), r[2].i())).collect();
            let mut recomputed = Some(0i64);
            for (fid, cbm, rate) in freights {
                match e.query(charged_s, &[Val::I64(fid)], &mut b) {
                    Ok(()) => *recomputed.as_mut().unwrap() += (cbm / MILLI) * rate + sum0(&b),
                    Err(err) => {
                        note("I3", None, format!("organization {org}: {err}"));
                        recomputed = None;
                        break;
                    }
                }
            }
            if let Some(rc) = recomputed {
                note("I3", Some(rc == stored), format!("organization {org}: outstanding={stored}, recomputed={rc}"));
            }
        }

        let mut written: Vec<i64> = freight_charges.keys().copied().collect();
        written.sort_unstable();
        for fid in sample(&mut vr, &written, verify_n) {
            match e.query(lines_s, &[Val::I64(fid)], &mut a) {
                Err(err) => note("I4", None, format!("freight {fid}: {err}")),
                Ok(()) => {
                    let want = freight_charges[&fid];
                    note("I4", Some(a.len() as i64 == want), format!("freight {fid}: {} charge rows stored, {want} written", a.len()));
                }
            }
        }

        let mut total = (0, 0, 0);
        for inv in ["I1", "I2", "I3", "I4"] {
            let (checks, fails, first, unanswered, first_un) = tally.remove(inv).unwrap_or_default();
            total.0 += checks;
            total.1 += fails;
            total.2 += unanswered;
            let mut detail = format!("{checks} checks, {fails} failure(s), {unanswered} unanswered");
            if let Some(f) = first {
                detail.push_str(&format!("; first: {f}"));
            }
            if let Some(f) = first_un {
                detail.push_str(&format!("; first unanswered: {f}"));
            }
            ctx.check(inv, checks > 0 && fails == 0 && unanswered == 0, detail);
        }
        ctx.set_meta("verify", serde_json::json!({"checks": total.0, "failures": total.1, "unanswered": total.2}));
    }
}
