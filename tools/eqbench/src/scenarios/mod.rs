//! The scenario framework: connections, the measured phase, the dialect
//! helpers, a seeded generator, and the checks a scenario reports.

pub mod s0_stockmarket;
pub mod s1_backtest;
pub mod s2_freight;
pub mod s3_library;
pub mod s4_cabinopt;
pub mod s5_orderentry;
pub mod smoke;

use crate::engine::*;
use crate::profile::Profiler;
use crate::stats::{latency, PhaseOut, Recorder, ServerCpu};
use serde::Serialize;
use std::collections::BTreeMap;
use std::time::Instant;

/// How to open a measured connection.
#[derive(Clone, Debug)]
pub enum ConnSpec {
    Kds { host: String, port: u16, durability: u8 },
    Pg { params: String },
    Ora { user: String, password: String, connect: String },
}

impl ConnSpec {
    pub fn kind(&self) -> Kind {
        match self {
            ConnSpec::Kds { .. } => Kind::Kds,
            ConnSpec::Pg { .. } => Kind::Pg,
            ConnSpec::Ora { .. } => Kind::Ora,
        }
    }
    pub fn open(&self) -> DbResult<Box<dyn Engine>> {
        Ok(match self {
            ConnSpec::Kds { host, port, durability } => Box::new(crate::kwp::Kwp::connect(host, *port, *durability)?),
            ConnSpec::Pg { params } => Box::new(crate::pg::Pg::connect(params)?),
            ConnSpec::Ora { user, password, connect } => Box::new(crate::ora::Ora::connect(user, password, connect)?),
        })
    }
}

#[derive(Serialize, Clone, Debug)]
pub struct Check {
    pub name: String,
    pub ok: bool,
    pub detail: String,
}

pub struct Ctx {
    pub kind: Kind,
    pub conn: ConnSpec,
    pub server_cpu: ServerCpu,
    pub profiler: Option<Box<dyn Profiler>>,
    pub seed: u64,
    pub opts: BTreeMap<String, String>,
    pub phases: Vec<PhaseOut>,
    pub checks: Vec<Check>,
    pub meta: BTreeMap<String, serde_json::Value>,
    pub status: Option<String>,
}

impl Ctx {
    pub fn connect(&self) -> Box<dyn Engine> {
        self.conn.open().unwrap_or_else(|e| panic!("connect: {e}"))
    }
    pub fn opt(&self, k: &str, default: u64) -> u64 {
        self.opts.get(k).map(|v| v.parse().unwrap_or_else(|_| panic!("--set {k}={v}: not a number"))).unwrap_or(default)
    }
    pub fn opt_s(&self, k: &str, default: &str) -> String {
        self.opts.get(k).cloned().unwrap_or_else(|| default.to_string())
    }
    pub fn set_meta(&mut self, k: &str, v: impl Serialize) {
        self.meta.insert(k.to_string(), serde_json::to_value(v).unwrap());
    }
    pub fn check(&mut self, name: &str, ok: bool, detail: impl Into<String>) {
        let detail = detail.into();
        eprintln!("check {name}: {} {detail}", if ok { "ok" } else { "FAILED" });
        self.checks.push(Check { name: name.to_string(), ok, detail });
    }
    fn note_status(&self, what: &str) {
        if let Some(p) = &self.status {
            let _ = std::fs::write(p, format!("{} {}\n", unix_now(), what));
        }
    }

    /// One measured phase over `engines`, one thread each. `op(i, engine,
    /// rec)` runs terminal `i`'s operations and records each with
    /// `rec.add(t0)`; the phase's server CPU, profile delta and wall clock
    /// are taken around all of them.
    pub fn phase<F>(&mut self, name: &str, engines: &mut [Box<dyn Engine>], op: F) -> PhaseOut
    where
        F: Fn(usize, &mut dyn Engine, &mut Recorder) + Sync,
    {
        self.note_status(&format!("phase {name}"));
        // A profiled phase needs every session's key; a key that cannot be
        // read is asked again, never replaced by one that filters nothing.
        let keys: Vec<i64> = engines
            .iter_mut()
            .map(|e| {
                let mut last = None;
                for _ in 0..50 {
                    match e.session_key() {
                        Ok(k) => return k,
                        Err(err) => last = Some(err),
                    }
                    std::thread::sleep(std::time::Duration::from_millis(10));
                }
                panic!("session key: {}", last.unwrap())
            })
            .collect();
        let mut snap0 = None;
        if let Some(p) = self.profiler.as_mut() {
            p.set_sessions(keys);
            snap0 = Some(p.snap().unwrap_or_else(|e| panic!("profile snapshot: {e}")));
            p.phase_start();
        }
        let cpu0 = self.server_cpu.read();
        let t0 = Instant::now();
        let recs: Vec<Recorder> = std::thread::scope(|s| {
            let hs: Vec<_> = engines
                .iter_mut()
                .enumerate()
                .map(|(i, e)| {
                    let op = &op;
                    s.spawn(move || {
                        let mut r = Recorder::new();
                        r.start(e.decode_ns());
                        op(i, e.as_mut(), &mut r);
                        r.stop(e.decode_ns());
                        r
                    })
                })
                .collect();
            hs.into_iter().map(|h| h.join().expect("terminal thread panicked")).collect()
        });
        let wall = t0.elapsed().as_secs_f64();
        let cpu1 = self.server_cpu.read();
        let mut rec = Recorder::new();
        let terminals = recs.len() as u32;
        for r in recs {
            rec.merge(r);
        }
        let ops = rec.lat_ns.len() as u64;
        let mut out = PhaseOut {
            name: name.to_string(),
            latency: latency(&rec.lat_ns),
            throughput_ops: if wall > 0.0 { ops as f64 / wall } else { 0.0 },
            wall_s: wall,
            terminals,
            errors: rec.errors,
            rows: rec.rows,
            kinds: rec.kinds.iter().map(|(k, v)| (k.to_string(), latency(v))).collect(),
            client_cpu_us_per_op: per_op(rec.cpu_ns as f64 / 1000.0, ops),
            client_decode_us_per_op: per_op(rec.decode_ns as f64 / 1000.0, ops),
            server_cpu_us_per_op: match (cpu0, cpu1) {
                // A thread that exits mid-phase takes its time with it, so
                // the sum can fall: no figure rather than a wrapped one.
                (Some(a), Some(b)) => b.checked_sub(a).map(|d| per_op(d as f64 / 1000.0, ops)),
                _ => None,
            },
            ..Default::default()
        };
        if let Some(p) = self.profiler.as_mut() {
            p.phase_stop();
            let snap1 = p.snap().unwrap_or_else(|e| panic!("profile snapshot: {e}"));
            let d = snap1.delta(snap0.as_ref().unwrap());
            out.breakdown = p.breakdown(&d, ops);
            out.events = d.event_list();
            out.raw = d.counters.clone();
        }
        out.breakdown.insert("formatting_client".into(), Some(out.client_decode_us_per_op));
        eprintln!(
            "phase {name}: {ops} ops, mean {:.1} us, p50 {:.1} us, p99 {:.1} us, {:.0} ops/s, errors {:?}",
            out.latency.mean_us, out.latency.p50_us, out.latency.p99_us, out.throughput_ops, out.errors
        );
        self.phases.push(out.clone());
        out
    }
}

fn per_op(v: f64, ops: u64) -> f64 {
    if ops == 0 {
        0.0
    } else {
        v / ops as f64
    }
}

pub fn unix_now() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

// ---- dialect ----------------------------------------------------------------

#[derive(Clone, Copy, Debug)]
pub enum Ty {
    I64,
    I32,
    I16,
    Str(u32),
}

pub fn ty(k: Kind, t: Ty) -> String {
    match (k, t) {
        (Kind::Kds, Ty::I64) => "int64".into(),
        (Kind::Kds, Ty::I32) => "int32".into(),
        (Kind::Kds, Ty::I16) => "int16".into(),
        (Kind::Kds, Ty::Str(n)) => format!("varchar({})", n.max(16)),
        (Kind::Pg, Ty::I64) => "bigint".into(),
        (Kind::Pg, Ty::I32) => "integer".into(),
        (Kind::Pg, Ty::I16) => "smallint".into(),
        (Kind::Pg, Ty::Str(n)) => format!("varchar({n})"),
        (Kind::Ora, Ty::I64) => "NUMBER(19)".into(),
        (Kind::Ora, Ty::I32) => "NUMBER(10)".into(),
        (Kind::Ora, Ty::I16) => "NUMBER(5)".into(),
        (Kind::Ora, Ty::Str(n)) => format!("VARCHAR2({n})"),
    }
}

/// How KDS stores a relation (`bench/README.md`, "What a relation is
/// stored as"); PostgreSQL's heap and Oracle's heap table are what each
/// engine has, untuned.
#[derive(Clone, Copy, Debug, Serialize)]
pub enum Store {
    Btree,
    Heap,
}

/// A relation: its columns (the first the `int64` key), its KDS kind, and
/// a foreign key per column where one is declared.
pub struct Table<'a> {
    pub name: &'a str,
    pub cols: &'a [(&'a str, Ty)],
    pub store: Store,
    pub refs: &'a [(&'a str, &'a str)],
}

/// `CREATE TABLE` in `k`'s dialect: the first column the primary key, every
/// column NOT NULL (KDS's default, spelled out on the others).
pub fn create_table(k: Kind, t: &Table) -> String {
    let mut cols = Vec::new();
    for (i, (c, tt)) in t.cols.iter().enumerate() {
        let mut d = format!("{c} {}", ty(k, *tt));
        if i == 0 {
            d.push_str(" PRIMARY KEY");
        } else if k != Kind::Kds {
            d.push_str(" NOT NULL"); // KDS: NOT NULL is the default
        }
        if let Some((_, parent)) = t.refs.iter().find(|(col, _)| col == c) {
            d.push_str(&format!(" REFERENCES {parent}"));
        }
        cols.push(d);
    }
    let tail = match (k, t.store) {
        (Kind::Kds, Store::Btree) => " BTREE",
        (Kind::Kds, Store::Heap) => " HEAP",
        _ => "",
    };
    format!("CREATE TABLE {} ({}){tail}", t.name, cols.join(", "))
}

pub fn drop_table(e: &mut dyn Engine, name: &str) {
    let sql = match e.kind() {
        Kind::Pg => format!("DROP TABLE IF EXISTS {name} CASCADE"),
        Kind::Ora => format!("DROP TABLE {name} CASCADE CONSTRAINTS PURGE"),
        Kind::Kds => format!("DROP TABLE {name}"),
    };
    let _ = e.run(&sql);
}

/// `LIMIT n` in `k`'s dialect, appended after any `ORDER BY`.
pub fn limit(k: Kind, n: u64) -> String {
    match k {
        Kind::Ora => format!(" FETCH FIRST {n} ROWS ONLY"),
        _ => format!(" LIMIT {n}"),
    }
}

/// After a load: each engine's own statistics refresh, as an operator would
/// run one (PostgreSQL `ANALYZE`, Oracle `DBMS_STATS`); KDS gathers its
/// planner's prior itself.
pub fn analyze(e: &mut dyn Engine, tables: &[&str]) {
    for t in tables {
        let sql = match e.kind() {
            Kind::Pg => format!("ANALYZE {t}"),
            Kind::Ora => format!("BEGIN DBMS_STATS.GATHER_TABLE_STATS(USER, '{}'); END;", t.to_uppercase()),
            Kind::Kds => continue,
        };
        e.run(&sql).unwrap_or_else(|err| panic!("{sql}: {err}"));
    }
}

/// Executions of a write before its phase - each statement's first
/// executions (Oracle's hard parse at the first execute, a PostgreSQL
/// backend's custom plans and first touch of the relation) - kept out of
/// the timing: `WARMUP_WRITES` times, each in a transaction rolled back, so
/// nothing stays written. A refusal is reported and the warm-up goes on.
pub const WARMUP_WRITES: usize = 6;

pub fn warm_writes(e: &mut dyn Engine, ws: &[(Stmt, &[Val])]) {
    for _ in 0..WARMUP_WRITES {
        e.begin().unwrap();
        for (s, p) in ws {
            if let Err(err) = e.exec(*s, p) {
                eprintln!("write warm-up refused: {err}");
            }
        }
        e.rollback().unwrap();
    }
}

/// Loads `rows` through one prepared single-row `INSERT`, `batch` rows a
/// transaction: the same statements on every engine.
pub fn load(e: &mut dyn Engine, table: &str, ncols: usize, rows: impl Iterator<Item = Vec<Val>>, batch: usize) {
    let marks = vec!["?"; ncols].join(", ");
    let s = e.prepare(&format!("INSERT INTO {table} VALUES ({marks})")).unwrap();
    let mut n = 0;
    e.begin().unwrap();
    for r in rows {
        e.exec(s, &r).unwrap_or_else(|err| panic!("load {table}: {err}"));
        n += 1;
        if n % batch == 0 {
            e.commit().unwrap();
            e.begin().unwrap();
        }
    }
    e.commit().unwrap();
}

// ---- a seeded generator: the same sequence on every engine ------------------

#[derive(Clone)]
pub struct Rng(u64);

impl Rng {
    pub fn new(seed: u64) -> Rng {
        Rng(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1)
    }
    pub fn next(&mut self) -> u64 {
        // xorshift64*
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
    /// Uniform in [lo, hi].
    pub fn range(&mut self, lo: i64, hi: i64) -> i64 {
        lo + (self.next() % (hi - lo + 1) as u64) as i64
    }
    pub fn chance(&mut self, pct: u64) -> bool {
        self.next() % 100 < pct
    }
    pub fn word(&mut self, len: usize) -> String {
        (0..len).map(|_| (b'a' + (self.next() % 26) as u8) as char).collect()
    }
}

pub fn run(name: &str, ctx: &mut Ctx) {
    match name {
        "smoke" => smoke::run(ctx),
        "s0" | "stockmarket" => s0_stockmarket::run(ctx),
        "s1" | "backtest" => s1_backtest::run(ctx),
        "s2" | "freight" => s2_freight::run(ctx),
        "s3" | "library" => s3_library::run(ctx),
        "s4" | "cabinopt" => s4_cabinopt::run(ctx),
        "s5" | "orderentry" => s5_orderentry::run(ctx),
        _ => panic!("unknown scenario {name}"),
    }
}
