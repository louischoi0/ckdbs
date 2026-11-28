//! s4_cabinopt: scenario 4's day 1 - the hot-predicate trading day of
//! `tools/scenario4_cabinopt_days.py` (KDS) and its PostgreSQL twin
//! `tools/pg_scenario4_cabinopt_days.py`, one code path for KDS, PostgreSQL
//! and Oracle.
//!
//! Two 10,000-row boards and three tapes of 200, 1,000 and 10,000 rows, each
//! `(id, symbol, qty, price)`, every symbol matching 10 rows at load. Day 1's
//! groups, in the Python's day order, are phases:
//!
//! | phase | statement | ops (default) |
//! |---|---|---|
//! | `open` | `INSERT` into `board_a`, autocommit, 60 % onto the day's 8 hot symbols | 240 |
//! | `board` | `symbol = ?` on `board_a`, 80 % hot | 2,400 |
//! | `tape200`, `tape1k`, `tape10k` | `symbol = ?` on each tape, 85 % onto 6 hot values | 396 each |
//! | `pk` | `id = ?` on `board_a`, the control that touches no Cabin | 240 |
//! | `close` | `COUNT(*)`, `SUM(qty)`, `GROUP BY symbol` on `board_a`, `COUNT(*)` on `tape_10k` | 3 rounds of 4 |
//!
//! **Arms** (`--set arm=off|declared|off,declared`): `off` is a plain
//! `symbol` column (no index, no Cabin); `declared` declares the column
//! `CABIN` at `CREATE TABLE` (`docs/spec/cabin.md` §8.1: a Cabin created with
//! the relation, a value observed on its first selection), KDS only. KDS
//! runs both by default, each arm over its own five relations on the one
//! server, interleaved per group (`board[off]`, `board[declared]`, ...);
//! PostgreSQL and Oracle run `off` alone, with no index on `symbol`, as the
//! PostgreSQL twin does, and a one-arm run names its phases by group alone.
//!
//! **The answers are compared, not assumed**: every operation's answer is
//! folded into its group's digest (rows as a multiset, since without
//! `ORDER BY` emission order is no contract on any engine), after the
//! operation's timer and inside the phase's client CPU; the Python's
//! verification set (the hot probes, two hot values a tape, the zero-row
//! probe, both boards' `COUNT(*)`, `SUM(qty)`) runs untimed after `close` as
//! group `verify`. Check `answers-equal-across-arms` compares every group's
//! digest between the arms of one run; `meta.digests[arm][group]` is what a
//! caller compares across engines (the same seed gives the same data, ids
//! and draws on all three).
//!
//! Relation kinds on KDS (`bench/README.md`, "What a relation is stored as"),
//! the same in both arms:
//!
//! **Kinds, EN-Q9 (a)** (adopted on the operator's standing go-ahead of
//! 2026-11-28): the tapes and `board_b` are only walked on `off`, which the
//! rule would make HEAP, but this scenario's subject is the declared Cabin,
//! whose authority is the tuple key and which resolves through the
//! clustered tree; the arms keep one kind so the arm comparison isolates the
//! Cabin. Every relation is BTREE on both arms.
//!
//! | relation | kind | the statements that decided it |
//! |---|---|---|
//! | `s4_<arm>_board_a` | BTREE | `pk`: a point by the tuple key; `board`: a walk on `off`, on `declared` a Cabin whose authority is the tuple key, resolved through the clustered tree when its page hint fails (`cabin.md` §3, C2); `open` appends ids above the load |
//! | `s4_<arm>_board_b` | BTREE | the same shape as `board_a` (the Python's day-2 board); day 1 only counts it |
//! | `s4_<arm>_tape_200`, `_1k`, `_10k` | BTREE | `tape*`: on `off` a walk, on `declared` a Cabin resolved by the tuple key (C2); `close` walks `tape_10k`. No secondary index reaches them, so the Cabin's tuple key resolve decides |
//!
//! **What differs from the Python, and what is left out:**
//!
//! - **The `on` arm is left out.** Its subject is the controller's
//!   lifecycle across days (CREATE, DECAYING, DROP, re-nomination), which
//!   the Python reaches with server keys no SQL sets (`decay_half_life` 5 s,
//!   `cabin_optimizer_snapshot_interval_ms` 500); `SET CABIN_OPTIMIZER ON`
//!   is the instance's one switch (`cabin-controller.md`, `OptimizerSurface`),
//!   so on a shared server it would change every concurrent run; and a
//!   controller-made Cabin serves no statement unless an administrator's
//!   plan names it (`cabin-controller.md` PO11), so on day 1 its reads take
//!   `off`'s path.
//! - **Day 1 only, unpaced, on every engine** (the twin's shape). The
//!   declared Cabin needs no pacing: it observes a value on its first
//!   selection. Days 2 and 3, the trading slots and the overnight polls are
//!   not ported.
//! - **The trading groups are not interleaved per block**: the Python runs
//!   12 blocks of board, tapes and pk; a phase here is one group's draws
//!   whole. The arms are interleaved per group instead of per block.
//! - **Ids are named**: the Python's KDS inserts omit the key (issued
//!   1, 2, ...) and the twin uses `bigserial`; here every engine names the
//!   same ids in the same order. `symbol` is `varchar(16)` (KDS's bare
//!   `varchar` is the instance width).
//! - **The declared Cabin is declared at `CREATE TABLE`**; the Python's
//!   `declared` arm ran `CREATE CABIN ON t(symbol)` after the load.
//! - Not ported: the load as a timed phase, `ANALYZE` and `EXPLAIN` plan
//!   evidence, `SHOW CABIN_OPTIMIZER` captures. The
//!   declared arm's `sys.cabins` rows are kept in `meta.cabins` and checked.
//! - Each read phase is preceded by `warmup` unrecorded executions of its
//!   statements: the symbol probes with the zero-row symbol `ZZZZ99`, which
//!   reads the relation's pages without observing any timed value, so the
//!   declared Cabin still learns its day-1 values inside the timed phase;
//!   `open`'s `INSERT` with the zero-row symbol on an id above the day's,
//!   each execution in a transaction rolled back.

use super::*;
use std::sync::Mutex;

const MATCHES: i64 = 10;
const TAPES: [(i64, &str); 3] = [(200, "200"), (1000, "1k"), (10000, "10k")];
const HOT_BOARD_SYMBOLS: usize = 8;
const HOT_TAPE_VALUES: i64 = 6;
const BOARD_HOT_PCT: u64 = 80;
const TAPE_HOT_PCT: u64 = 85;
const OPEN_HOT_PCT: u64 = 60;
const ZERO_ROW_SYMBOL: &str = "ZZZZ99";
const LOAD_BATCH: usize = 500;

fn board_symbol(board: char, i: i64) -> String {
    format!("{}{i:04}", board.to_ascii_uppercase())
}
fn tape_symbol(i: i64) -> String {
    format!("T{i:04}")
}

/// `(symbol, qty, price)` with the id first: symbol `i % domain`, so every
/// symbol matches exactly `MATCHES` rows at load.
fn make_rows(rows: i64, sym: impl Fn(i64) -> String, rng: &mut Rng) -> Vec<Vec<Val>> {
    let domain = rows / MATCHES;
    (0..rows)
        .map(|i| vec![Val::I64(i + 1), Val::Str(sym(i % domain)), Val::I64(rng.range(1, 999)), Val::I64(rng.range(100, 99_999))])
        .collect()
}

// ---- the answer digest --------------------------------------------------

const FNV_OFF: u64 = 0xcbf2_9ce4_8422_2325;
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;

fn mix_bytes(mut h: u64, b: &[u8]) -> u64 {
    for &x in b {
        h = (h ^ x as u64).wrapping_mul(FNV_PRIME);
    }
    h
}
fn mix(h: u64, x: u64) -> u64 {
    mix_bytes(h, &x.to_le_bytes())
}
fn row_hash(row: &[Val]) -> u64 {
    let mut h = FNV_OFF;
    for v in row {
        h = match v {
            Val::Null => mix(h, 0),
            Val::I64(x) => mix(mix(h, 1), *x as u64),
            Val::Str(s) => mix_bytes(mix(mix(h, 2), s.len() as u64), s.as_bytes()),
        };
    }
    h
}
/// One answer folded into a running digest: its row count and its rows as
/// a multiset (an order-free sum of row hashes).
fn fold_rows(h: u64, rows: &Rows) -> u64 {
    let sum = rows.iter().fold(0u64, |a, r| a.wrapping_add(row_hash(r)));
    mix(mix(h, rows.len() as u64), sum)
}
const REFUSED: u64 = 0xE77E_77E7_7E77_E77E;

// ---- one arm's relations and statements ---------------------------------

struct Arm {
    name: &'static str,
    declared: bool,
    board_a: String,
    board_b: String,
    tapes: [String; 3],
}

impl Arm {
    fn new(name: &'static str) -> Arm {
        let t = |s: &str| format!("s4_{name}_{s}");
        Arm {
            name,
            declared: name == "declared",
            board_a: t("board_a"),
            board_b: t("board_b"),
            tapes: TAPES.map(|(_, tag)| t(&format!("tape_{tag}"))),
        }
    }
    fn tables(&self) -> Vec<&str> {
        let mut v = vec![self.board_a.as_str(), self.board_b.as_str()];
        v.extend(self.tapes.iter().map(String::as_str));
        v
    }
}

struct Stmts {
    insert: Stmt,
    board: Stmt,
    tapes: [Stmt; 3],
    pk: Stmt,
    close: [Stmt; 4],
    count_b: Stmt,
}

fn prepare(e: &mut dyn Engine, a: &Arm) -> Stmts {
    let mut p = |sql: String| e.prepare(&sql).unwrap_or_else(|err| panic!("prepare {sql}: {err}"));
    let probe = |t: &str| format!("SELECT id, symbol, qty, price FROM {t} WHERE symbol = ?");
    Stmts {
        insert: p(format!("INSERT INTO {} VALUES (?, ?, ?, ?)", a.board_a)),
        board: p(probe(&a.board_a)),
        tapes: [p(probe(&a.tapes[0])), p(probe(&a.tapes[1])), p(probe(&a.tapes[2]))],
        pk: p(format!("SELECT id, symbol, qty, price FROM {} WHERE id = ?", a.board_a)),
        close: [
            p(format!("SELECT COUNT(*) FROM {}", a.board_a)),
            p(format!("SELECT SUM(qty) FROM {}", a.board_a)),
            p(format!("SELECT symbol, COUNT(*) FROM {} GROUP BY symbol", a.board_a)),
            p(format!("SELECT COUNT(*) FROM {}", a.tapes[2])),
        ],
        count_b: p(format!("SELECT COUNT(*) FROM {}", a.board_b)),
    }
}

// ---- day 1's draws, made once and replayed on every arm and engine ------

struct Day {
    hot_syms: Vec<String>,
    hot_tape: Vec<String>,
    open_rows: Vec<Vec<Val>>,
    board: Vec<String>,
    tapes: [Vec<String>; 3],
    pk: Vec<i64>,
}

fn plan_day(rng: &mut Rng, board_rows: i64, open: u64, board_probes: u64, tape_probes: u64, pk_ops: u64) -> Day {
    let domain = board_rows / MATCHES;
    // A partial shuffle of board a's domain: the day's disjoint hot slice.
    let mut idx: Vec<i64> = (0..domain).collect();
    for j in 0..HOT_BOARD_SYMBOLS {
        let k = rng.range(j as i64, domain - 1) as usize;
        idx.swap(j, k);
    }
    let hot_syms: Vec<String> = idx[..HOT_BOARD_SYMBOLS].iter().map(|&i| board_symbol('a', i)).collect();
    let hot_tape: Vec<String> = (0..HOT_TAPE_VALUES).map(tape_symbol).collect();
    let pick = |rng: &mut Rng, v: &[String]| v[rng.range(0, v.len() as i64 - 1) as usize].clone();
    let open_rows = (0..open as i64)
        .map(|k| {
            let sym = if rng.chance(OPEN_HOT_PCT) { pick(rng, &hot_syms) } else { board_symbol('a', rng.range(0, domain - 1)) };
            vec![Val::I64(board_rows + 1 + k), Val::Str(sym), Val::I64(rng.range(1, 999)), Val::I64(rng.range(100, 99_999))]
        })
        .collect();
    let board = (0..board_probes)
        .map(|_| if rng.chance(BOARD_HOT_PCT) { pick(rng, &hot_syms) } else { board_symbol('a', rng.range(0, domain - 1)) })
        .collect();
    let tapes = TAPES.map(|(size, _)| {
        (0..tape_probes)
            .map(|_| if rng.chance(TAPE_HOT_PCT) { pick(rng, &hot_tape) } else { tape_symbol(rng.range(0, size / MATCHES - 1)) })
            .collect()
    });
    let pk = (0..pk_ops).map(|_| rng.range(1, board_rows)).collect();
    Day { hot_syms, hot_tape, open_rows, board, tapes, pk }
}

// ---- running a group -----------------------------------------------------

type Ops = Vec<(Stmt, Vec<Val>)>;

#[derive(Default, Clone, Copy)]
struct GroupOut {
    digest: u64,
    rows: u64,
    refused: u64,
}

/// `ops` once, in order: each recorded, its answer folded into the digest.
fn run_ops(e: &mut dyn Engine, ops: &[(Stmt, Vec<Val>)], write: bool, mut rec: Option<&mut Recorder>) -> GroupOut {
    let mut out = GroupOut { digest: FNV_OFF, ..Default::default() };
    let mut rows = Rows::new();
    for (s, p) in ops {
        let t0 = Instant::now();
        let res = if write { e.exec(*s, p).map(|n| n as usize) } else { e.query(*s, p, &mut rows).map(|_| rows.len()) };
        match res {
            Ok(n) => {
                if let Some(r) = rec.as_deref_mut() {
                    r.add(t0);
                    r.rows += n as u64;
                }
                out.rows += n as u64;
                out.digest = if write { mix(out.digest, n as u64) } else { fold_rows(out.digest, &rows) };
            }
            Err(err) => {
                if out.refused == 0 {
                    eprintln!("s4: refused: {err}");
                }
                out.refused += 1;
                if let Some(r) = rec.as_deref_mut() {
                    r.error(&format!("{:?}", err.class).to_lowercase());
                }
                out.digest = mix(out.digest, REFUSED);
            }
        }
    }
    out
}

fn phase_name(group: &str, arm: &Arm, arms: usize) -> String {
    if arms == 1 {
        group.to_string()
    } else {
        format!("{group}[{}]", arm.name)
    }
}

pub fn run(ctx: &mut Ctx) {
    let k = ctx.kind;
    let board_rows = ctx.opt("board_rows", 10_000) as i64;
    let open = ctx.opt("open_inserts", 240);
    let board_probes = ctx.opt("board_probes", 2400);
    let tape_probes = ctx.opt("tape_probes", 396);
    let pk_ops = ctx.opt("pk_ops", 240);
    let close_rounds = ctx.opt("close_rounds", 3);
    let warmup = ctx.opt("warmup", 20) as usize;
    assert!(board_rows % MATCHES == 0 && board_rows / MATCHES >= HOT_BOARD_SYMBOLS as i64, "board_rows: a multiple of 10, at least 80");
    let default_arms = if k == Kind::Kds { "off,declared" } else { "off" };
    let arms: Vec<Arm> = ctx
        .opt_s("arm", default_arms)
        .split(',')
        .map(|a| match a.trim() {
            "off" => Arm::new("off"),
            "declared" if k == Kind::Kds => Arm::new("declared"),
            "declared" => panic!("arm=declared: a Cabin is KDS's; {} runs arm=off", k.name()),
            other => panic!("arm={other}: off or declared (the `on` arm is not ported; see the file's header)"),
        })
        .collect();
    let na = arms.len();

    // ---- setup: identical relations and rows on every arm ---------------
    let mut e = ctx.connect();
    let cols: &[(&str, Ty)] = &[("id", Ty::I64), ("symbol", Ty::Str(16)), ("qty", Ty::I64), ("price", Ty::I64)];
    for a in &arms {
        for t in a.tables() {
            drop_table(e.as_mut(), t);
            let mut ddl = create_table(k, &Table { name: t, cols, store: Store::Btree, refs: &[] });
            if a.declared {
                // KDS only (`cabin.md` §8.1): the column declared `CABIN`.
                let col = format!("symbol {}", ty(k, Ty::Str(16)));
                ddl = ddl.replacen(&col, &format!("{col} CABIN"), 1);
            }
            e.run(&ddl).unwrap_or_else(|err| panic!("{ddl}: {err}"));
        }
        let mut rng = Rng::new(ctx.seed);
        let rows_a = make_rows(board_rows, |i| board_symbol('a', i), &mut rng);
        let rows_b = make_rows(board_rows, |i| board_symbol('b', i), &mut rng);
        load(e.as_mut(), &a.board_a, 4, rows_a.into_iter(), LOAD_BATCH);
        load(e.as_mut(), &a.board_b, 4, rows_b.into_iter(), LOAD_BATCH);
        for (i, (size, _)) in TAPES.iter().enumerate() {
            let rows = make_rows(*size, tape_symbol, &mut rng);
            load(e.as_mut(), &a.tapes[i], 4, rows.into_iter(), LOAD_BATCH);
        }
        analyze(e.as_mut(), &a.tables());
    }
    let day = plan_day(&mut Rng::new(ctx.seed + 1), board_rows, open, board_probes, tape_probes, pk_ops);
    let st: Vec<Stmts> = arms.iter().map(|a| prepare(e.as_mut(), a)).collect();
    let mut engines = vec![e];

    // ---- day 1's groups, each arm in turn per group ---------------------
    let sym_ops = |s: Stmt, v: &[String]| -> Ops { v.iter().map(|x| (s, vec![Val::Str(x.clone())])).collect() };
    let zero_ops = |s: Stmt| -> Ops { (0..warmup).map(|_| (s, vec![Val::from(ZERO_ROW_SYMBOL)])).collect() };
    let mut wrng = Rng::new(ctx.seed + 2);
    let pk_warm: Ops = (0..warmup).map(|_| (st[0].pk, vec![Val::I64(wrng.range(1, board_rows))])).collect();
    let pk_warm_of = |s: &Stmts| -> Ops { pk_warm.iter().map(|(_, p)| (s.pk, p.clone())).collect() };
    let close_ops = |s: &Stmts, rounds: u64| -> Ops { (0..rounds).flat_map(|_| s.close.iter().map(|&c| (c, vec![]))).collect() };

    // (group, write, timed ops per arm, warm-up ops per arm)
    let mut groups: Vec<(String, bool, Vec<Ops>, Vec<Ops>)> = Vec::new();
    // `open`'s warm-up is the INSERT on an id above the day's and the
    // zero-row symbol, each in a transaction rolled back (`run` below).
    let open_warm = vec![Val::I64(1 << 40), Val::from(ZERO_ROW_SYMBOL), Val::I64(1), Val::I64(100)];
    groups.push((
        "open".into(),
        true,
        st.iter().map(|s| day.open_rows.iter().map(|r| (s.insert, r.clone())).collect()).collect(),
        st.iter().map(|s| (0..warmup).map(|_| (s.insert, open_warm.clone())).collect()).collect(),
    ));
    groups.push(("board".into(), false, st.iter().map(|s| sym_ops(s.board, &day.board)).collect(), st.iter().map(|s| zero_ops(s.board)).collect()));
    for (i, (_, tag)) in TAPES.iter().enumerate() {
        groups.push((
            format!("tape{tag}"),
            false,
            st.iter().map(|s| sym_ops(s.tapes[i], &day.tapes[i])).collect(),
            st.iter().map(|s| zero_ops(s.tapes[i])).collect(),
        ));
    }
    groups.push((
        "pk".into(),
        false,
        st.iter().map(|s| day.pk.iter().map(|&id| (s.pk, vec![Val::I64(id)])).collect()).collect(),
        st.iter().map(pk_warm_of).collect(),
    ));
    groups.push((
        "close".into(),
        false,
        st.iter().map(|s| close_ops(s, close_rounds)).collect(),
        st.iter().map(|s| if warmup > 0 { close_ops(s, 1) } else { vec![] }).collect(),
    ));

    let mut digests: Vec<BTreeMap<String, GroupOut>> = vec![BTreeMap::new(); na];
    for (group, write, timed, warm) in &groups {
        for (ai, a) in arms.iter().enumerate() {
            if let Some(w) = warm.get(ai) {
                let e = engines[0].as_mut();
                let mut refused = 0;
                for op in w {
                    // A write's warm-up is rolled back, one transaction each.
                    if *write {
                        e.begin().unwrap();
                    }
                    refused += run_ops(e, std::slice::from_ref(op), *write, None).refused;
                    if *write {
                        e.rollback().unwrap();
                    }
                }
                if refused > 0 {
                    eprintln!("s4: {refused} refusals in {group}[{}]'s warm-up", a.name);
                }
            }
            let ops = &timed[ai];
            let out = Mutex::new(GroupOut::default());
            ctx.phase(&phase_name(group, a, na), &mut engines, |_, e, r| {
                *out.lock().unwrap() = run_ops(e, ops, *write, Some(r));
            });
            digests[ai].insert(group.clone(), out.into_inner().unwrap());
        }
    }

    // ---- the verification set, untimed (the Python's per-day guard) -----
    for (ai, s) in st.iter().enumerate() {
        let mut v: Ops = sym_ops(s.board, &day.hot_syms);
        for i in 0..TAPES.len() {
            v.extend(sym_ops(s.tapes[i], &day.hot_tape[..2]));
        }
        v.push((s.board, vec![Val::from(ZERO_ROW_SYMBOL)]));
        v.push((s.close[0], vec![]));
        v.push((s.count_b, vec![]));
        v.push((s.close[1], vec![]));
        let out = run_ops(engines[0].as_mut(), &v, false, None);
        digests[ai].insert("verify".into(), out);
    }

    // ---- checks ----------------------------------------------------------
    let e = engines[0].as_mut();
    let mut rows = Rows::new();
    let scalar = |e: &mut dyn Engine, s: Stmt| -> i64 {
        let mut rows = Rows::new();
        e.query(s, &[], &mut rows).unwrap_or_else(|err| panic!("check read: {err}"));
        rows.first().map(|r| r[0].i()).unwrap_or(-1)
    };
    for (ai, a) in arms.iter().enumerate() {
        let s = &st[ai];
        let opened = open as i64 - digests[ai]["open"].refused as i64;
        let ca = scalar(e, s.close[0]);
        let cb = scalar(e, s.count_b);
        let c10 = scalar(e, s.close[3]);
        let zero = {
            e.query(s.board, &[Val::from(ZERO_ROW_SYMBOL)], &mut rows).unwrap();
            rows.len()
        };
        let pk = digests[ai]["pk"];
        let ok = ca == board_rows + opened && cb == board_rows && c10 == TAPES[2].0 && zero == 0 && pk.refused == 0 && pk.rows == pk_ops;
        ctx.check(
            &format!("counts[{}]", a.name),
            ok,
            format!(
                "board_a {ca} (expect {}), board_b {cb} (expect {board_rows}), tape_10k {c10}, zero-row probe {zero} rows, pk {} rows of {pk_ops}",
                board_rows + opened,
                pk.rows
            ),
        );
    }
    let refused: u64 = digests.iter().flat_map(|m| m.values()).map(|g| g.refused).sum();
    ctx.check("no-refusals", refused == 0, format!("{refused} statements refused over every group and arm"));
    if na > 1 {
        let differ: Vec<String> = digests[0]
            .iter()
            .filter(|(g, d)| digests[1..].iter().any(|m| m.get(*g).map(|o| o.digest) != Some(d.digest)))
            .map(|(g, _)| g.clone())
            .collect();
        ctx.check(
            "answers-equal-across-arms",
            differ.is_empty(),
            if differ.is_empty() {
                format!("{} groups, arms {}", digests[0].len(), arms.iter().map(|a| a.name).collect::<Vec<_>>().join(","))
            } else {
                format!("groups whose answers differ: {}", differ.join(", "))
            },
        );
    }

    // The declared arm's Cabins: one per relation, and serving reads.
    let mut cabin_meta = Vec::new();
    if let Some(ai) = arms.iter().position(|a| a.declared) {
        let a = &arms[ai];
        let s = e.prepare("SELECT rel_name, status, observed, hits, misses FROM sys.cabins").unwrap();
        e.query(s, &[], &mut rows).unwrap();
        let mine: Vec<&Vec<Val>> = rows.iter().filter(|r| a.tables().contains(&r[0].s())).collect();
        let probed = [&a.board_a, &a.tapes[0], &a.tapes[1], &a.tapes[2]];
        let served = probed.iter().all(|t| mine.iter().any(|r| r[0].s() == t.as_str() && r[1].s() == "active" && r[3].i() > 0));
        for r in &mine {
            cabin_meta.push(serde_json::json!({"rel": r[0].s(), "status": r[1].s(), "observed": r[2].i(), "hits": r[3].i(), "misses": r[4].i()}));
        }
        ctx.check(
            "declared-cabins-served",
            mine.len() == 5 && served,
            format!("{} Cabins on the declared arm's 5 relations; every probed one active with hits: {served}", mine.len()),
        );
    }

    // ---- meta ------------------------------------------------------------
    let dg: BTreeMap<&str, BTreeMap<String, String>> = arms
        .iter()
        .zip(&digests)
        .map(|(a, m)| (a.name, m.iter().map(|(g, o)| (g.clone(), format!("{:016x}", o.digest))).collect()))
        .collect();
    ctx.set_meta("digests", dg);
    ctx.set_meta("arms", arms.iter().map(|a| a.name).collect::<Vec<_>>());
    ctx.set_meta("day", 1);
    ctx.set_meta("paced", false);
    ctx.set_meta("rows", format!("boards 2x{board_rows}, tapes 200/1000/10000, {MATCHES} rows a symbol"));
    ctx.set_meta("hot_board_symbols", &day.hot_syms);
    ctx.set_meta("hot_tape_values", &day.hot_tape);
    ctx.set_meta(
        "relations",
        serde_json::json!({"board_a": "BTREE", "board_b": "BTREE", "tape_200": "BTREE", "tape_1k": "BTREE", "tape_10k": "BTREE"}),
    );
    if !cabin_meta.is_empty() {
        ctx.set_meta("cabins", cabin_meta);
    }
}
