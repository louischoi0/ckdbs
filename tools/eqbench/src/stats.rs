//! A phase's record: every operation's latency, the client's CPU and decode
//! time, the server's CPU, and the engine's own breakdown (`profile.rs`).
//!
//! A phase may be recorded on several threads (one per terminal); each
//! thread fills its own `Recorder` and the phase is their merge.

use serde::Serialize;
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::time::Instant;

pub fn thread_cpu_ns() -> u64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_THREAD_CPUTIME_ID, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

/// The whole server's CPU: KDS's process (every thread) or a container's
/// cgroup (every process in it).
#[derive(Clone, Debug)]
pub enum ServerCpu {
    None,
    Pid(i32),
    Cgroup(PathBuf),
}

impl ServerCpu {
    pub fn parse(s: &str) -> ServerCpu {
        if let Some(p) = s.strip_prefix("pid:") {
            ServerCpu::Pid(p.parse().expect("pid:<n>"))
        } else if let Some(p) = s.strip_prefix("cgroup:") {
            ServerCpu::Cgroup(PathBuf::from(p))
        } else {
            ServerCpu::None
        }
    }
    /// Nanoseconds of CPU, or None where not measured.
    pub fn read(&self) -> Option<u64> {
        match self {
            ServerCpu::None => None,
            ServerCpu::Pid(p) => {
                // Every thread's time on CPU in nanoseconds (schedstat's
                // first field): /proc/<pid>/stat counts in clock ticks, too
                // coarse for a phase of a few milliseconds.
                let mut ns = 0u64;
                for t in std::fs::read_dir(format!("/proc/{p}/task")).ok()?.flatten() {
                    if let Ok(s) = std::fs::read_to_string(t.path().join("schedstat")) {
                        ns += s.split_whitespace().next().and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
                    }
                }
                Some(ns)
            }
            ServerCpu::Cgroup(p) => {
                let s = std::fs::read_to_string(p.join("cpu.stat")).ok()?;
                let line = s.lines().find(|l| l.starts_with("usage_usec "))?;
                Some(line[11..].trim().parse::<u64>().ok()? * 1000)
            }
        }
    }
}

#[derive(Default)]
pub struct Recorder {
    pub lat_ns: Vec<u64>,
    pub errors: BTreeMap<String, u64>,
    pub rows: u64,
    pub cpu_ns: u64,
    pub decode_ns: u64,
    cpu0: u64,
    dec0: u64,
}

impl Recorder {
    pub fn new() -> Recorder {
        Recorder::default()
    }
    /// Marks the thread's CPU and the engine's decode counter at the head.
    pub fn start(&mut self, decode_ns: u64) {
        self.cpu0 = thread_cpu_ns();
        self.dec0 = decode_ns;
    }
    pub fn stop(&mut self, decode_ns: u64) {
        self.cpu_ns += thread_cpu_ns() - self.cpu0;
        self.decode_ns += decode_ns - self.dec0;
    }
    #[inline]
    pub fn add(&mut self, t0: Instant) {
        self.lat_ns.push(t0.elapsed().as_nanos() as u64);
    }
    pub fn error(&mut self, what: &str) {
        *self.errors.entry(what.to_string()).or_insert(0) += 1;
    }
    pub fn merge(&mut self, o: Recorder) {
        self.lat_ns.extend(o.lat_ns);
        for (k, v) in o.errors {
            *self.errors.entry(k).or_insert(0) += v;
        }
        self.rows += o.rows;
        self.cpu_ns += o.cpu_ns;
        self.decode_ns += o.decode_ns;
    }
}

#[derive(Serialize, Clone, Debug, Default)]
pub struct Latency {
    pub ops: u64,
    pub mean_us: f64,
    pub p0_us: f64,
    pub p25_us: f64,
    pub p50_us: f64,
    pub p95_us: f64,
    pub p99_us: f64,
    pub max_us: f64,
}

pub fn latency(v: &[u64]) -> Latency {
    if v.is_empty() {
        return Latency::default();
    }
    let mut s = v.to_vec();
    s.sort_unstable();
    let q = |p: f64| s[((s.len() - 1) as f64 * p).round() as usize] as f64 / 1000.0;
    Latency {
        ops: s.len() as u64,
        mean_us: s.iter().sum::<u64>() as f64 / s.len() as f64 / 1000.0,
        p0_us: q(0.0),
        p25_us: q(0.25),
        p50_us: q(0.50),
        p95_us: q(0.95),
        p99_us: q(0.99),
        max_us: q(1.0),
    }
}

/// One phase as written to the JSON.
#[derive(Serialize, Clone, Debug, Default)]
pub struct PhaseOut {
    pub name: String,
    pub latency: Latency,
    /// operations per second over the phase's wall clock (all terminals)
    pub throughput_ops: f64,
    pub wall_s: f64,
    pub terminals: u32,
    pub errors: BTreeMap<String, u64>,
    pub rows: u64,
    pub client_cpu_us_per_op: f64,
    pub client_decode_us_per_op: f64,
    pub server_cpu_us_per_op: Option<f64>,
    /// The engine's own accounting of where the time went, µs per
    /// operation, keyed by the common categories; None is "not reported by
    /// the engine".
    pub breakdown: BTreeMap<String, Option<f64>>,
    /// Every wait or lock event the engine reported in the phase.
    pub events: Vec<Event>,
    /// Raw counters from the engine's views, delta over the phase.
    pub raw: BTreeMap<String, f64>,
}

#[derive(Serialize, Clone, Debug)]
pub struct Event {
    pub name: String,
    pub class: String,
    pub count: f64,
    pub total_us: f64,
}
