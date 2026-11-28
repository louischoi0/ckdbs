//! Where a phase's time went, by each engine's own accounting (EN-Q8).
//!
//! A profiler holds a connection of its own, outside the timed loop, and
//! takes a snapshot of the engine's views at a phase's head and end; the
//! delta, divided by the phase's operations, is the breakdown. The common
//! categories are:
//!
//! | category | KDS | PostgreSQL | Oracle |
//! |---|---|---|---|
//! | `parse` | not reported apart | not reported (a prepared statement parses once, outside the loop) | `parse time elapsed` less `hard parse elapsed time` |
//! | `optimise` | not reported apart | `pg_stat_statements.total_plan_time` | `hard parse elapsed time` |
//! | `parse_optimise` | `sys.statement_stats.compile_us` | the two above summed | the two above summed |
//! | `execution` | `session_waits`' busy classes but the four below, less `compile_us` | `total_exec_time` less read and sampled lock | `sql execute elapsed time` less the waits below it |
//! | `disk_read` | class User I/O (`io.*`) | `shared_blk_read_time` | wait class `User I/O` |
//! | `commit` | class Commit (`commit.*`) | sampled `IO` `Wal*` and `LWLock` `WALWrite` | wait class `Commit` |
//! | `lock` | class Application (`app.*`; `sys.lock_waits` per wait) | sampled `Lock` and other `LWLock` waits | wait classes `Application`, `Concurrency` |
//! | `network` | class Network (`net.*`) | sampled `Client`/`IPC` waits while active | wait class `Network` |
//!
//! `formatting` is the harness's own (`decode_ns`), added by the runner:
//! the server's encoding of a row is inside each engine's execution and no
//! engine reports it apart.

use crate::engine::*;
use crate::stats::Event;
use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

#[derive(Default, Clone, Debug)]
pub struct Snap {
    pub counters: BTreeMap<String, f64>,
    /// (name, class) -> (count, total µs)
    pub events: BTreeMap<(String, String), (f64, f64)>,
}

impl Snap {
    pub fn delta(&self, before: &Snap) -> Snap {
        let mut d = Snap::default();
        for (k, v) in &self.counters {
            d.counters.insert(k.clone(), v - before.counters.get(k).copied().unwrap_or(0.0));
        }
        for (k, (c, t)) in &self.events {
            let (c0, t0) = before.events.get(k).copied().unwrap_or((0.0, 0.0));
            if c - c0 > 0.0 || t - t0 > 0.0 {
                d.events.insert(k.clone(), (c - c0, t - t0));
            }
        }
        d
    }
    fn c(&self, k: &str) -> Option<f64> {
        self.counters.get(k).copied()
    }
    fn class_us(&self, pred: impl Fn(&str, &str) -> bool) -> f64 {
        self.events.iter().filter(|((n, c), _)| pred(n, c)).map(|(_, (_, t))| *t).sum()
    }
    pub fn event_list(&self) -> Vec<Event> {
        self.events
            .iter()
            .map(|((n, c), (k, t))| Event { name: n.clone(), class: c.clone(), count: *k, total_us: *t })
            .collect()
    }
}

pub trait Profiler: Send {
    /// The measured connections' session keys (`Engine::session_key`).
    fn set_sessions(&mut self, keys: Vec<i64>);
    fn snap(&mut self) -> DbResult<Snap>;
    /// Starts and stops anything sampled over a phase (PostgreSQL's waits).
    fn phase_start(&mut self) {}
    fn phase_stop(&mut self) {}
    fn breakdown(&self, d: &Snap, ops: u64) -> BTreeMap<String, Option<f64>>;
}

fn per(v: Option<f64>, ops: u64) -> Option<f64> {
    v.map(|x| if ops == 0 { 0.0 } else { (x / ops as f64).max(0.0) })
}

// ---- KDS --------------------------------------------------------------------

pub struct KdsProfiler {
    e: Box<dyn Engine>,
    sessions: Vec<i64>,
    q_stmt: Stmt,
    q_waits: Stmt,
    q_locks: Stmt,
    /// `sys.lock_waits`' `seq` is each core's own count, so the last seen is
    /// kept per core.
    last_lock_seq: BTreeMap<i64, i64>,
    lock_rows: BTreeMap<(String, String), (f64, f64)>,
}

impl KdsProfiler {
    pub fn new(mut e: Box<dyn Engine>) -> DbResult<KdsProfiler> {
        let q_stmt = e.prepare("SELECT compile_us, page_reads, shape_text FROM sys.statement_stats")?;
        let q_waits = e.prepare("SELECT session_id, event, count, total_us FROM sys.session_waits")?;
        let q_locks = e.prepare("SELECT core, seq, waited_us, unit, mode, outcome FROM sys.lock_waits")?;
        Ok(KdsProfiler {
            e,
            sessions: vec![],
            q_stmt,
            q_waits,
            q_locks,
            last_lock_seq: BTreeMap::new(),
            lock_rows: BTreeMap::new(),
        })
    }
}

fn kds_class(event: &str) -> &'static str {
    match event.split('.').next().unwrap_or("") {
        "cpu" => "CPU",
        "commit" => "Commit",
        "io" => "User I/O",
        "sysio" => "System I/O",
        "xcore" => "CrossCore",
        "sched" => "Scheduler",
        "res" => "Resource",
        "net" => "Network",
        "app" => "Application",
        "idle" => "Idle",
        _ => "Other",
    }
}

impl Profiler for KdsProfiler {
    fn set_sessions(&mut self, keys: Vec<i64>) {
        self.sessions = keys;
    }
    fn snap(&mut self) -> DbResult<Snap> {
        let mut s = Snap::default();
        let mut rows = Rows::new();
        // compile_us and page_reads are per statement shape for the whole
        // instance (no session column): every run is alone on its server.
        self.e.query(self.q_stmt, &[], &mut rows)?;
        for r in &rows {
            if r[2].s().contains("sys.") {
                continue;
            }
            for (k, col) in ["compile_us", "page_reads"].iter().enumerate() {
                if let Val::I64(v) = r[k] {
                    *s.counters.entry(col.to_string()).or_insert(0.0) += v as f64;
                }
            }
        }
        self.e.query(self.q_waits, &[], &mut rows)?;
        for r in &rows {
            if !self.sessions.contains(&r[0].i()) {
                continue;
            }
            let ev = r[1].s().to_string();
            let cls = kds_class(&ev).to_string();
            let e = s.events.entry((ev, cls)).or_insert((0.0, 0.0));
            e.0 += r[2].i() as f64;
            e.1 += r[3].i() as f64;
        }
        // Lock waits: a ring of rows per core, each one wait; the rows past
        // a core's last seen sequence go into running totals.
        self.e.query(self.q_locks, &[], &mut rows)?;
        let mut seen = self.last_lock_seq.clone();
        for r in &rows {
            let (core, seq) = (r[0].i(), r[1].i());
            if seq <= self.last_lock_seq.get(&core).copied().unwrap_or(0) {
                continue;
            }
            let m = seen.entry(core).or_insert(0);
            *m = (*m).max(seq);
            let name = format!("lock {} {} {}", r[3].s(), r[4].s(), r[5].s());
            let e = self.lock_rows.entry((name, "Lock".into())).or_insert((0.0, 0.0));
            e.0 += 1.0;
            e.1 += r[2].i() as f64;
        }
        self.last_lock_seq = seen;
        for (k, v) in &self.lock_rows {
            s.events.insert(k.clone(), *v);
        }
        Ok(s)
    }
    /// The time split is the measured sessions' own `sys.session_waits`, by
    /// each event's class; `compile_us` is `sys.statement_stats`' (wall
    /// time from a statement's head to its chain compiled), taken out of
    /// the CPU it is spent in.
    fn breakdown(&self, d: &Snap, ops: u64) -> BTreeMap<String, Option<f64>> {
        let mut b = BTreeMap::new();
        let compile = d.c("compile_us");
        let class = |c: &str| d.class_us(|_, k| k == c);
        let exec = d.class_us(|_, k| !matches!(k, "Commit" | "User I/O" | "Network" | "Application" | "Idle" | "Lock"));
        b.insert("parse".into(), None);
        b.insert("optimise".into(), None);
        b.insert("parse_optimise".into(), per(compile, ops));
        b.insert("execution".into(), per(Some(exec - compile.unwrap_or(0.0)), ops));
        b.insert("disk_read".into(), per(Some(class("User I/O")), ops));
        b.insert("commit".into(), per(Some(class("Commit")), ops));
        b.insert("lock".into(), per(Some(class("Application")), ops));
        b.insert("network".into(), per(Some(class("Network")), ops));
        b
    }
}

// ---- PostgreSQL -------------------------------------------------------------

pub struct PgProfiler {
    client: postgres::Client,
    params: String,
    sessions: Vec<i64>,
    sampler: Option<(Arc<AtomicBool>, std::thread::JoinHandle<()>)>,
    samples: Arc<Mutex<Samples>>,
    have_pss: bool,
}

/// The sampler's running totals: per (event, type), the samples and the µs
/// they stand for.
#[derive(Default)]
struct Samples {
    by: BTreeMap<(String, String), (f64, f64)>,
    n: f64,
}

impl PgProfiler {
    pub fn new(params: &str) -> DbResult<PgProfiler> {
        let mut client = postgres::Client::connect(params, postgres::NoTls).map_err(|e| other(e.to_string()))?;
        let have_pss = client.batch_execute("CREATE EXTENSION IF NOT EXISTS pg_stat_statements").is_ok()
            && client.query("SELECT 1 FROM pg_stat_statements LIMIT 1", &[]).is_ok();
        Ok(PgProfiler {
            client,
            params: params.to_string(),
            sessions: vec![],
            sampler: None,
            samples: Arc::new(Mutex::new(Samples::default())),
            have_pss,
        })
    }
}

impl Profiler for PgProfiler {
    fn set_sessions(&mut self, keys: Vec<i64>) {
        self.sessions = keys;
    }
    fn snap(&mut self) -> DbResult<Snap> {
        let mut s = Snap::default();
        if self.have_pss {
            let rows = self
                .client
                .query(
                    "SELECT query, calls, (total_plan_time*1000)::int8, (total_exec_time*1000)::int8, \
                     shared_blks_read, (shared_blk_read_time*1000)::int8, shared_blks_hit \
                     FROM pg_stat_statements WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database())",
                    &[],
                )
                .map_err(|e| other(e.to_string()))?;
            for r in rows {
                let q: String = r.get(0);
                if q.contains("pg_stat") || q.contains("pg_backend_pid") || q.contains("pg_database") {
                    continue;
                }
                let get = |i: usize| r.get::<_, Option<i64>>(i).unwrap_or(0) as f64;
                let add = |s: &mut Snap, k: &str, v: f64| *s.counters.entry(k.to_string()).or_insert(0.0) += v;
                add(&mut s, "calls", get(1));
                add(&mut s, "plan_us", get(2));
                add(&mut s, "exec_us", get(3));
                add(&mut s, "shared_blks_read", get(4));
                add(&mut s, "blk_read_us", get(5));
                add(&mut s, "shared_blks_hit", get(6));
            }
        }
        let g = self.samples.lock().unwrap();
        for (k, v) in &g.by {
            s.events.insert(k.clone(), *v);
        }
        s.counters.insert("wait_samples".into(), g.n);
        Ok(s)
    }
    fn phase_start(&mut self) {
        if !self.have_pss {
            return; // the timed pass: nothing sampled
        }
        let stop = Arc::new(AtomicBool::new(false));
        let st = stop.clone();
        let params = self.params.clone();
        let pids: Vec<i32> = self.sessions.iter().map(|p| *p as i32).collect();
        let samples = self.samples.clone();
        let h = std::thread::spawn(move || {
            let Ok(mut c) = postgres::Client::connect(&params, postgres::NoTls) else { return };
            let Ok(q) = c.prepare("SELECT state, coalesce(wait_event_type,''), coalesce(wait_event,'') FROM pg_stat_activity WHERE pid = ANY($1)") else { return };
            let interval = Duration::from_millis(1);
            let mut prev = Instant::now();
            while !st.load(Ordering::Relaxed) {
                let next = Instant::now() + interval;
                if let Ok(rows) = c.query(&q, &[&pids]) {
                    // Each sample stands for the time since the one before.
                    let now = Instant::now();
                    let dt = (now - prev).as_micros() as f64;
                    prev = now;
                    let mut g = samples.lock().unwrap();
                    for r in rows {
                        let state: Option<String> = r.get(0);
                        let ty: String = r.get(1);
                        let ev: String = r.get(2);
                        if state.as_deref() != Some("active") {
                            continue; // idle in transaction: the client's time
                        }
                        let key = if ty.is_empty() { ("CPU".to_string(), "CPU".to_string()) } else { (ev, ty) };
                        let e = g.by.entry(key).or_insert((0.0, 0.0));
                        e.0 += 1.0;
                        e.1 += dt;
                    }
                    g.n += 1.0;
                }
                let now = Instant::now();
                if next > now {
                    std::thread::sleep(next - now);
                }
            }
        });
        self.sampler = Some((stop, h));
    }
    fn phase_stop(&mut self) {
        if let Some((stop, h)) = self.sampler.take() {
            stop.store(true, Ordering::Relaxed);
            let _ = h.join();
        }
    }
    fn breakdown(&self, d: &Snap, ops: u64) -> BTreeMap<String, Option<f64>> {
        let mut b = BTreeMap::new();
        let pss = self.have_pss;
        let plan = if pss { d.c("plan_us") } else { None };
        let exec = if pss { d.c("exec_us") } else { None };
        let read = if pss { d.c("blk_read_us") } else { None };
        // A COMMIT's flush runs after its utility statement returns, so
        // pg_stat_statements does not time it: the sampled WAL waits do -
        // the flush (IO Wal*) and the group commit's wait for it
        // (LWLock WALWrite).
        let is_commit = |n: &str, c: &str| (c == "IO" && n.starts_with("Wal")) || (c == "LWLock" && n == "WALWrite");
        let commit = pss.then(|| d.class_us(is_commit));
        // A lock wait inside a statement is inside its total_exec_time.
        let lock = pss.then(|| d.class_us(|n, c| (c == "Lock" || c == "LWLock") && !is_commit(n, c)));
        b.insert("parse".into(), None);
        b.insert("optimise".into(), per(plan, ops));
        b.insert("parse_optimise".into(), per(plan, ops));
        b.insert("execution".into(), per(exec.map(|e| e - read.unwrap_or(0.0) - lock.unwrap_or(0.0)), ops));
        b.insert("disk_read".into(), per(read, ops));
        b.insert("commit".into(), per(commit, ops));
        b.insert("lock".into(), per(lock, ops));
        b.insert("network".into(), per(pss.then(|| d.class_us(|_, c| c == "Client" || c == "IPC")), ops));
        b
    }
}

// ---- Oracle -----------------------------------------------------------------

pub struct OraProfiler {
    conn: oracle::Connection,
    sessions: Vec<i64>,
}

impl OraProfiler {
    pub fn new(user: &str, password: &str, connect: &str) -> DbResult<OraProfiler> {
        let conn = oracle::Connection::connect(user, password, connect).map_err(|e| other(e.to_string()))?;
        Ok(OraProfiler { conn, sessions: vec![] })
    }
}

impl Profiler for OraProfiler {
    fn set_sessions(&mut self, keys: Vec<i64>) {
        self.sessions = keys;
    }
    fn snap(&mut self) -> DbResult<Snap> {
        let mut s = Snap::default();
        if self.sessions.is_empty() {
            return Ok(s);
        }
        let sids = self.sessions.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(",");
        let oe = |e: oracle::Error| other(e.to_string());
        let rows = self
            .conn
            .query(&format!("SELECT stat_name, SUM(value) FROM v$sess_time_model WHERE sid IN ({sids}) GROUP BY stat_name"), &[])
            .map_err(oe)?;
        for r in rows {
            let r = r.map_err(oe)?;
            let n: String = r.get(0).map_err(oe)?;
            let v: f64 = r.get(1).map_err(oe)?;
            s.counters.insert(format!("tm:{n}"), v);
        }
        let rows = self
            .conn
            .query(
                &format!(
                    "SELECT n.name, SUM(t.value) FROM v$sesstat t JOIN v$statname n ON n.statistic# = t.statistic# \
                     WHERE t.sid IN ({sids}) AND n.name IN ('parse count (total)', 'parse count (hard)', \
                     'execute count', 'physical reads', 'session logical reads', 'user commits', 'redo size') GROUP BY n.name"
                ),
                &[],
            )
            .map_err(oe)?;
        for r in rows {
            let r = r.map_err(oe)?;
            let n: String = r.get(0).map_err(oe)?;
            let v: f64 = r.get(1).map_err(oe)?;
            s.counters.insert(format!("st:{n}"), v);
        }
        let rows = self
            .conn
            .query(
                &format!(
                    "SELECT event, wait_class, SUM(total_waits), SUM(time_waited_micro) FROM v$session_event \
                     WHERE sid IN ({sids}) GROUP BY event, wait_class"
                ),
                &[],
            )
            .map_err(oe)?;
        for r in rows {
            let r = r.map_err(oe)?;
            let n: String = r.get(0).map_err(oe)?;
            let c: String = r.get(1).map_err(oe)?;
            let k: f64 = r.get(2).map_err(oe)?;
            let t: f64 = r.get(3).map_err(oe)?;
            s.events.insert((n, c), (k, t));
        }
        Ok(s)
    }
    fn breakdown(&self, d: &Snap, ops: u64) -> BTreeMap<String, Option<f64>> {
        let mut b = BTreeMap::new();
        let parse = d.c("tm:parse time elapsed");
        let hard = d.c("tm:hard parse elapsed time");
        let exec = d.c("tm:sql execute elapsed time");
        let io = d.class_us(|_, c| c == "User I/O");
        let lock = d.class_us(|_, c| c == "Application" || c == "Concurrency");
        let commit = d.class_us(|_, c| c == "Commit");
        let net = d.class_us(|_, c| c == "Network");
        b.insert("parse".into(), per(parse.map(|p| p - hard.unwrap_or(0.0)), ops));
        b.insert("optimise".into(), per(hard, ops));
        b.insert("parse_optimise".into(), per(parse, ops));
        b.insert("execution".into(), per(exec.map(|e| e - io - lock), ops));
        b.insert("disk_read".into(), per(Some(io), ops));
        b.insert("commit".into(), per(Some(commit), ops));
        b.insert("lock".into(), per(Some(lock), ops));
        b.insert("network".into(), per(Some(net), ops));
        b
    }
}
