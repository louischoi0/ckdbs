#![allow(dead_code)]
//! eqbench - one client for three engines (EO, `tools/eqbench/README.md`).
//!
//!     eqbench --engine kds --kds 127.0.0.1:15791 [--kds-durability group|strict]
//!     eqbench --engine pg  --pg "host=127.0.0.1 port=15434 user=postgres dbname=bench"
//!     eqbench --engine oracle --ora bench/<pw>@//127.0.0.1:1521/FREEPDB1 --ora-admin system/<pw>
//!         --scenario <name> [--seed N] [--set key=value]... [--server-cpu pid:N|cgroup:PATH]
//!         [--no-profile] [--json PATH] [--status PATH]
//!
//! Passwords may be given as `$ENV` names: `--ora bench/$ORA_PW@...`.

mod engine;
mod kwp;
mod ora;
mod pg;
mod profile;
mod scenarios;
mod stats;

use engine::Kind;
use scenarios::{ConnSpec, Ctx};
use std::collections::BTreeMap;

fn expand(s: &str) -> String {
    // `$NAME` tokens inside a credential string, from the environment.
    let mut out = String::new();
    let mut it = s.chars().peekable();
    while let Some(c) = it.next() {
        if c == '$' {
            let mut name = String::new();
            while let Some(&n) = it.peek() {
                if n.is_ascii_alphanumeric() || n == '_' {
                    name.push(n);
                    it.next();
                } else {
                    break;
                }
            }
            out.push_str(&std::env::var(&name).unwrap_or_else(|_| panic!("${name} is not set")));
        } else {
            out.push(c);
        }
    }
    out
}

/// `user/password@connect`
fn ora_parts(s: &str) -> (String, String, String) {
    let s = expand(s);
    let (cred, connect) = s.split_once('@').expect("user/password@connect");
    let (u, p) = cred.split_once('/').expect("user/password@connect");
    (u.to_string(), p.to_string(), connect.to_string())
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut a: BTreeMap<String, String> = BTreeMap::new();
    let mut opts = BTreeMap::new();
    let mut flags = Vec::new();
    let mut i = 0;
    while i < args.len() {
        let k = args[i].trim_start_matches("--").to_string();
        match k.as_str() {
            "no-profile" => flags.push(k),
            "set" => {
                let (kk, vv) = args[i + 1].split_once('=').expect("--set key=value");
                opts.insert(kk.to_string(), vv.to_string());
                i += 1;
            }
            _ => {
                a.insert(k, args.get(i + 1).cloned().unwrap_or_else(|| panic!("--{} needs a value", args[i])));
                i += 1;
            }
        }
        i += 1;
    }
    // `--env-file PATH`: KEY=VALUE lines (servers.sh's secret file), so a
    // password reaches `$NAME` expansion without passing through a shell.
    if let Some(p) = a.get("env-file") {
        for line in std::fs::read_to_string(p).expect("--env-file").lines() {
            if let Some((k, v)) = line.split_once('=') {
                std::env::set_var(k.trim(), v.trim());
            }
        }
    }
    let kind = Kind::parse(a.get("engine").map(String::as_str).unwrap_or("")).expect("--engine kds|pg|oracle");
    let profile = !flags.iter().any(|f| f == "no-profile");
    let conn = match kind {
        Kind::Kds => {
            let hp = a.get("kds").expect("--kds host:port");
            let (h, p) = hp.rsplit_once(':').unwrap();
            let durability = match a.get("kds-durability").map(String::as_str).unwrap_or("default") {
                "strict" => 1,
                "group" => 2,
                "default" => 0,
                d => panic!("--kds-durability {d}"),
            };
            ConnSpec::Kds { host: h.to_string(), port: p.parse().unwrap(), durability }
        }
        Kind::Pg => ConnSpec::Pg { params: expand(a.get("pg").expect("--pg <params>")) },
        Kind::Ora => {
            let (user, password, connect) = ora_parts(a.get("ora").expect("--ora user/pw@connect"));
            ConnSpec::Ora { user, password, connect }
        }
    };
    let profiler: Option<Box<dyn profile::Profiler>> = if !profile {
        None
    } else {
        Some(match &conn {
            ConnSpec::Kds { .. } => Box::new(profile::KdsProfiler::new(conn.open().expect("profiler connection")).expect("KDS profiler")),
            ConnSpec::Pg { params } => Box::new(profile::PgProfiler::new(params).expect("PG profiler")),
            ConnSpec::Ora { connect, .. } => {
                let (u, p, c) = ora_parts(a.get("ora-admin").map(String::as_str).unwrap_or_else(|| panic!("--ora-admin user/pw (for V$ views)")));
                let _ = connect;
                Box::new(profile::OraProfiler::new(&u, &p, &c).expect("Oracle profiler"))
            }
        })
    };
    let scenario = a.get("scenario").expect("--scenario").clone();
    let mut ctx = Ctx {
        kind,
        conn,
        server_cpu: stats::ServerCpu::parse(a.get("server-cpu").map(String::as_str).unwrap_or("")),
        profiler,
        seed: a.get("seed").map(|s| s.parse().unwrap()).unwrap_or(1),
        opts,
        phases: vec![],
        checks: vec![],
        meta: BTreeMap::new(),
        status: a.get("status").cloned(),
    };
    let t0 = scenarios::unix_now();
    scenarios::run(&scenario, &mut ctx);
    let failed = ctx.checks.iter().filter(|c| !c.ok).count();
    let out = serde_json::json!({
        "harness": "eqbench",
        "engine": kind.name(),
        "scenario": scenario,
        "seed": ctx.seed,
        "options": ctx.opts,
        "profile": profile,
        "started_unix": t0,
        "ended_unix": scenarios::unix_now(),
        "meta": ctx.meta,
        "checks": ctx.checks,
        "phases": ctx.phases,
    });
    let text = serde_json::to_string_pretty(&out).unwrap();
    match a.get("json") {
        Some(p) => std::fs::write(p, text).expect("write --json"),
        None => println!("{text}"),
    }
    if failed > 0 {
        eprintln!("{failed} check(s) failed");
        std::process::exit(2);
    }
}
