//! s3_library: reads through a secondary index - a library's circulation
//! (the port of `tools/scenario3_library.py` and its PostgreSQL twin
//! `tools/pg_scenario3_library.py`).
//!
//! Four relations - `users`, `books`, `reservations` and the bulk relation
//! `loans` - and twelve read shapes, most of them an equality or a range on
//! a column that is not the key. `loans` is the row-set axis (`--set
//! loans=200|1000|10000`); `users` and `books` scale with it so that a
//! `WHERE user_id = ?` returns about `matches` rows at every size, and
//! `books-by-genre` (16 genres) is the counter-case whose answer grows with
//! the relation. `--set indexed=1` declares the Python's `--index-mode all`
//! (the mode the matched cells' "indexed" column ran) on every engine,
//! after the load by default (`--set index_when=before` declares them on
//! the empty relations, as the Python's `--index-when before`);
//! `indexed=0` declares none.
//!
//! Relation kinds on KDS (`bench/README.md`, "What a relation is stored
//! as"); the same kinds in both index cells:
//!
//! | relation | kind | the statements that decided it |
//! |---|---|---|
//! | `s3_users` | BTREE | `pk-user` (`id = ?`), `join-loan-user` (`users.id = ?`), `join-no-literal` (`id BETWEEN 1 AND 16`), `exists-correlated` (`id BETWEEN 1 AND 20`): every read of it is by the tuple key, a point or a range |
//! | `s3_books` | HEAP | `books-by-author` (`author_id = ?`), `books-by-genre` (`genre = ?`): no statement names its key; reached through a secondary index, or walked |
//! | `s3_reservations` | HEAP | `resv-by-user` (`user_id = ?`): the same |
//! | `s3_loans` | HEAP | `loans-by-user`, `loans-by-book`, `count-by-user`, `overdue` (`status = 2 AND due_day BETWEEN`), `loans-by-daterange`, and the inner side of `join-loan-user`, `join-no-literal` and `exists-correlated` (`loans.user_id = users.id`): no statement reaches a loan by its id. Indexed, every one of them but `loans-by-daterange` is a secondary-index probe, where a heap's entry carries the rowid and needs no second descent; unindexed, every one is a walk, which a heap reads in no key order and a btree has no key range to bound. Both read the HEAP rule |
//!
//! The indexes (`indexed=1`), the Python's `all` mode: `loans(user_id)`,
//! `loans(book_id)`, `reservations(user_id)`, `books(author_id)`,
//! `books(genre)`, `loans(status, due_day)`, `books(branch_id, genre)`, and
//! `loans(user_id)` covering `book_id, due_day, status` - KDS `COVERING`,
//! PostgreSQL `INCLUDE`; Oracle has no included column, so its covering
//! index is the key `(user_id, book_id, due_day, status)`, Oracle's own
//! spelling of an index that answers those columns.
//!
//! Verification (the Python's five `verify_problems`, each a `ctx.check`):
//! row counts; `loans-by-user` and `count-by-user` against a full scan of
//! `loans` for a sample of `verify` users; every loan's `user_id` names a
//! user; `join-no-literal` and `exists-correlated` against expectations
//! built from two full scans. Added: the `loans` scan equals what the load
//! wrote, and no phase met a refusal (the Python's exit 1 on an error
//! reply). The two join-family answers are compared as multisets, not row
//! for row: neither statement has an `ORDER BY`, so emission order is no
//! contract on any of the three engines (`CLAUDE.md`, the join planner's
//! row), and on a HEAP `loans` the per-user matches are in no key order.
//!
//! Left out, and why:
//! - `--assert-index-reads`: it reads KDS's `ANALYZE <statement>` plan text,
//!   which has no counterpart through the `Engine` trait on PostgreSQL
//!   (`EXPLAIN`) or Oracle (`EXPLAIN PLAN`/`DBMS_XPLAN`) that one code path
//!   could parse; dropped on every engine rather than kept on one. With it
//!   go the Python's `plans` meta, its `SHOW INDEXES` and `SHOW META` footer.
//! - `--cabin`: KDS-only, with no PostgreSQL or Oracle equivalent (the twin
//!   says so), and no matched cell ran it.
//! - `--server-indexes`: a label for a server configuration key, not a
//!   client behaviour.
//! - `--index-mode single|composite|covering`: only `none` and `all` were
//!   the matched cells; `indexed` is the 0/1 between them.
//! - The KDS-issued keys and PostgreSQL's `bigserial ... RETURNING id`: the
//!   harness loads every column, the key included, so each relation's ids
//!   are 1..n on every engine, as both drivers' issued ids were.
//! - The Python's `random.Random` sequence: the harness's `Rng` draws the
//!   same distributions, so the values are not the Python's.
//! - The PostgreSQL twin's `ANALYZE` inside its `create-index` phase: every
//!   engine's statistics refresh runs unrecorded after the load and the
//!   index build (`analyze`), as in every harness scenario.
//!
//! Every statement names its relations in full instead of an alias: KDS
//! takes a table alias only with `AS` and Oracle refuses `AS` there, so the
//! unaliased form is the one text all three parse.

use super::*;

const DAY0: i64 = 20000; // an arbitrary epoch-day base, as the Python's
const LOAN_WINDOW: i64 = 720; // loans spread over this many days
const GENRES: i64 = 16;
const BRANCHES: i64 = 8;
const MEMBER_TYPES: i64 = 4;
const STATUS_OUT: i64 = 0;
const STATUS_RETURNED: i64 = 1;
const STATUS_OVERDUE: i64 = 2;
const JOIN_OUTER_K: i64 = 16; // outer rows for join-no-literal
const EXISTS_OUTER_K: i64 = 20; // outer rows for exists-correlated

const USERS: Table<'static> = Table {
    name: "s3_users",
    cols: &[
        ("id", Ty::I64),
        ("member_code", Ty::Str(16)),
        ("name", Ty::Str(16)),
        ("email", Ty::Str(32)),
        ("joined_day", Ty::I32),
        ("member_type", Ty::I32),
        ("status", Ty::I32),
        ("branch_id", Ty::I32),
        ("fines_owed", Ty::I64),
    ],
    store: Store::Btree,
    refs: &[],
};
const BOOKS: Table<'static> = Table {
    name: "s3_books",
    cols: &[
        ("id", Ty::I64),
        ("isbn", Ty::Str(16)),
        ("title", Ty::Str(16)),
        ("author_id", Ty::I64),
        ("publisher_id", Ty::I32),
        ("published_year", Ty::I32),
        ("genre", Ty::I32),
        ("branch_id", Ty::I32),
        ("copies", Ty::I32),
        ("status", Ty::I32),
    ],
    store: Store::Heap,
    refs: &[],
};
const RESERVATIONS: Table<'static> = Table {
    name: "s3_reservations",
    cols: &[
        ("id", Ty::I64),
        ("user_id", Ty::I64),
        ("book_id", Ty::I64),
        ("placed_day", Ty::I32),
        ("expires_day", Ty::I32),
        ("status", Ty::I32),
        ("queue_pos", Ty::I32),
    ],
    store: Store::Heap,
    refs: &[],
};
const LOANS: Table<'static> = Table {
    name: "s3_loans",
    cols: &[
        ("id", Ty::I64),
        ("user_id", Ty::I64),
        ("book_id", Ty::I64),
        ("loaned_day", Ty::I32),
        ("due_day", Ty::I32),
        ("returned_day", Ty::I32),
        ("status", Ty::I32),
        ("renewals", Ty::I32),
    ],
    store: Store::Heap,
    refs: &[],
};
/// Creation and load order, as the Python's `CREATE_ORDER`.
const TABLES: [&Table<'static>; 4] = [&USERS, &BOOKS, &RESERVATIONS, &LOANS];

/// The Python's `INDEX_MODES["all"]`: single + composite + covering, as
/// (name, relation, key columns, covered columns).
const INDEXES: &[(&str, &str, &[&str], &[&str])] = &[
    ("s3_loans_user", "s3_loans", &["user_id"], &[]),
    ("s3_loans_book", "s3_loans", &["book_id"], &[]),
    ("s3_resv_user", "s3_reservations", &["user_id"], &[]),
    ("s3_books_author", "s3_books", &["author_id"], &[]),
    ("s3_books_genre", "s3_books", &["genre"], &[]),
    ("s3_loans_status_due", "s3_loans", &["status", "due_day"], &[]),
    ("s3_books_branch_genre", "s3_books", &["branch_id", "genre"], &[]),
    ("s3_loans_user_cov", "s3_loans", &["user_id"], &["book_id", "due_day", "status"]),
];

fn create_index(k: Kind, name: &str, table: &str, keys: &[&str], cov: &[&str]) -> String {
    if cov.is_empty() {
        return format!("CREATE INDEX {name} ON {table} ({})", keys.join(", "));
    }
    match k {
        Kind::Kds => format!("CREATE INDEX {name} ON {table} ({}) COVERING ({})", keys.join(", "), cov.join(", ")),
        Kind::Pg => format!("CREATE INDEX {name} ON {table} ({}) INCLUDE ({})", keys.join(", "), cov.join(", ")),
        // No included columns on Oracle: the covered columns join the key.
        Kind::Ora => format!("CREATE INDEX {name} ON {table} ({}, {})", keys.join(", "), cov.join(", ")),
    }
}

/// Row counts, with matches per key held constant across the sweep (the
/// Python's `sizes_for`).
#[derive(Clone, Copy)]
struct Sizes {
    users: i64,
    books: i64,
    reservations: i64,
    loans: i64,
    authors: i64,
}

impl Sizes {
    fn new(loans: i64, matches: i64) -> Sizes {
        let loans = loans.max(20);
        let users = (loans / matches.max(1)).max(4);
        let books = (loans / matches.max(1)).max(4);
        // Authors are fewer than books, so `books-by-author` sits between
        // the one-row and the genre shapes.
        Sizes { users, books, reservations: (loans / 2).max(4), loans, authors: (books / 4).max(2) }
    }
}

/// Every row the load writes, per relation in `TABLES` order.
fn generate(seed: u64, sz: &Sizes) -> [Vec<Vec<Val>>; 4] {
    let mut rng = Rng::new(seed);
    let i = Val::I64;
    let users = (0..sz.users)
        .map(|n| {
            vec![
                i(n + 1),
                Val::Str(format!("M{n:07}")),
                Val::Str(format!("member{n:07}")),
                Val::Str(format!("m{n:07}@library.test")),
                i(DAY0 - rng.range(0, 3650)),
                i(rng.range(0, MEMBER_TYPES - 1)),
                i(0),
                i(rng.range(0, BRANCHES - 1)),
                i(rng.range(0, 4999)),
            ]
        })
        .collect();
    let books = (0..sz.books)
        .map(|n| {
            vec![
                i(n + 1),
                Val::Str(format!("978{n:010}")),
                Val::Str(format!("title{n:07}")),
                i(rng.range(0, sz.authors - 1)),
                i(rng.range(0, 63)),
                i(1950 + rng.range(0, 74)),
                i(rng.range(0, GENRES - 1)),
                i(rng.range(0, BRANCHES - 1)),
                i(rng.range(1, 6)),
                i(0),
            ]
        })
        .collect();
    let reservations = (0..sz.reservations)
        .map(|n| {
            let placed = DAY0 - rng.range(0, LOAN_WINDOW - 1);
            vec![
                i(n + 1),
                i(rng.range(1, sz.users)),
                i(rng.range(1, sz.books)),
                i(placed),
                i(placed + 14),
                i(rng.range(0, 2)),
                i(rng.range(1, 5)),
            ]
        })
        .collect();
    // A third of the loans are out and a slice of those overdue, so
    // `overdue` has a real answer at every size.
    let loans = (0..sz.loans)
        .map(|n| {
            let loaned = DAY0 - rng.range(0, LOAN_WINDOW - 1);
            let due = loaned + 21;
            let (status, returned) = if rng.chance(34) {
                (if due < DAY0 - 7 { STATUS_OVERDUE } else { STATUS_OUT }, 0)
            } else {
                (STATUS_RETURNED, (due + rng.range(-14, 7)).min(DAY0))
            };
            vec![
                i(n + 1),
                i(rng.range(1, sz.users)),
                i(rng.range(1, sz.books)),
                i(loaned),
                i(due),
                i(returned),
                i(status),
                i(rng.range(0, 2)),
            ]
        })
        .collect();
    [users, books, reservations, loans]
}

/// One read shape: its phase name, its statement and how one execution's
/// arguments are drawn.
struct Shape {
    name: &'static str,
    sql: String,
    args: fn(&mut Rng, &Sizes) -> Vec<Val>,
}

fn join_no_literal_sql() -> String {
    format!(
        "SELECT s3_loans.book_id, s3_users.member_code FROM s3_users JOIN s3_loans \
         ON s3_loans.user_id = s3_users.id WHERE s3_users.id BETWEEN 1 AND {JOIN_OUTER_K}"
    )
}

fn exists_correlated_sql() -> String {
    format!(
        "SELECT id FROM s3_users WHERE id BETWEEN 1 AND {EXISTS_OUTER_K} AND EXISTS \
         (SELECT s3_loans.id FROM s3_loans WHERE s3_loans.user_id = s3_users.id)"
    )
}

/// The twelve shapes in the Python's order (`read_phases`).
fn shapes() -> Vec<Shape> {
    let user = |r: &mut Rng, s: &Sizes| vec![Val::I64(r.range(1, s.users))];
    vec![
        // The control: a key point read, which no index should move.
        Shape { name: "pk-user", sql: "SELECT member_code, member_type, branch_id FROM s3_users WHERE id = ?".into(), args: user },
        // The headline: a non-key equality, ~matches rows.
        Shape {
            name: "loans-by-user",
            sql: "SELECT book_id, loaned_day, due_day, status FROM s3_loans WHERE user_id = ?".into(),
            args: user,
        },
        Shape {
            name: "loans-by-book",
            sql: "SELECT user_id, loaned_day, status FROM s3_loans WHERE book_id = ?".into(),
            args: |r, s| vec![Val::I64(r.range(1, s.books))],
        },
        Shape {
            name: "resv-by-user",
            sql: "SELECT book_id, placed_day, queue_pos FROM s3_reservations WHERE user_id = ?".into(),
            args: user,
        },
        Shape {
            name: "books-by-author",
            sql: "SELECT title, published_year, genre FROM s3_books WHERE author_id = ?".into(),
            args: |r, s| vec![Val::I64(r.range(0, s.authors - 1))],
        },
        // The counter-case: 1/16 of the relation, growing with it.
        Shape {
            name: "books-by-genre",
            sql: "SELECT title, author_id, branch_id FROM s3_books WHERE genre = ?".into(),
            args: |r, _| vec![Val::I64(r.range(0, GENRES - 1))],
        },
        // A non-key range no mode indexes.
        Shape {
            name: "loans-by-daterange",
            sql: "SELECT user_id, book_id, due_day FROM s3_loans WHERE loaned_day BETWEEN ? AND ?".into(),
            args: |r, _| {
                let low = DAY0 - r.range(0, LOAN_WINDOW - 1);
                vec![Val::I64(low), Val::I64(low + 30)]
            },
        },
        // Two columns: the composite index's case.
        Shape {
            name: "overdue",
            sql: format!("SELECT user_id, book_id, due_day FROM s3_loans WHERE status = {STATUS_OVERDUE} AND due_day BETWEEN ? AND ?"),
            args: |r, _| {
                let low = DAY0 - r.range(0, LOAN_WINDOW - 1);
                vec![Val::I64(low), Val::I64(low + 60)]
            },
        },
        Shape {
            name: "join-loan-user",
            sql: "SELECT s3_loans.book_id, s3_loans.due_day, s3_users.member_code FROM s3_loans \
                  JOIN s3_users ON s3_loans.user_id = s3_users.id WHERE s3_users.id = ?"
                .into(),
            args: user,
        },
        // Fixed literals: no equality to propagate onto loans.
        Shape { name: "join-no-literal", sql: join_no_literal_sql(), args: |_, _| vec![] },
        Shape { name: "exists-correlated", sql: exists_correlated_sql(), args: |_, _| vec![] },
        Shape { name: "count-by-user", sql: "SELECT COUNT(*) FROM s3_loans WHERE user_id = ?".into(), args: user },
    ]
}

fn class_name(c: ErrClass) -> String {
    format!("{c:?}").to_lowercase()
}

/// A seed of its own for stream `n`, so each phase's arguments are fixed
/// whatever the others draw.
fn sub_seed(seed: u64, n: u64) -> u64 {
    seed.wrapping_mul(1_000_003).wrapping_add(n)
}

/// Runs text statements one by one in a recorded phase; a refusal aborts
/// the scenario, as the Python's DDL refusals abort it.
fn ddl_phase(ctx: &mut Ctx, engines: &mut [Box<dyn Engine>], name: &str, sql: &[String]) {
    let out = ctx.phase(name, engines, |_, e, r| {
        for s in sql {
            let t0 = Instant::now();
            match e.run(s) {
                Ok(()) => r.add(t0),
                Err(err) => {
                    eprintln!("{name}: {s}: {err}");
                    r.error(&class_name(err.class));
                }
            }
        }
    });
    assert!(out.errors.is_empty(), "{name} refused: {:?}", out.errors);
}

pub fn run(ctx: &mut Ctx) {
    let k = ctx.kind;
    let sz = Sizes::new(ctx.opt("loans", 1000) as i64, ctx.opt("matches", 5) as i64);
    let indexed = ctx.opt("indexed", 0) != 0;
    let index_when = ctx.opt_s("index_when", "after");
    assert!(index_when == "after" || index_when == "before", "--set index_when=after|before");
    let ops = ctx.opt("ops", 200);
    let warmup = ctx.opt("warmup", 20);
    let verify = ctx.opt("verify", 25);
    let seed = ctx.seed;

    ctx.set_meta("sizes", serde_json::json!({
        "users": sz.users, "books": sz.books, "reservations": sz.reservations, "loans": sz.loans, "authors": sz.authors,
    }));
    ctx.set_meta("index_mode", if indexed { "all" } else { "none" });
    ctx.set_meta("index_when", &index_when);
    ctx.set_meta("ops_per_shape", ops);
    ctx.set_meta("warmup_per_shape", warmup);
    ctx.set_meta("verify_sample", verify);
    ctx.set_meta(
        "kinds",
        TABLES.iter().map(|t| (t.name.to_string(), t.store)).collect::<std::collections::BTreeMap<_, _>>(),
    );

    let mut e = ctx.connect();
    for t in TABLES.iter().rev() {
        drop_table(e.as_mut(), t.name);
    }
    let mut engines = vec![e];

    let ddl: Vec<String> = TABLES.iter().map(|t| create_table(k, t)).collect();
    ddl_phase(ctx, &mut engines, "ddl", &ddl);
    let idx: Vec<String> = INDEXES.iter().map(|(n, t, keys, cov)| create_index(k, n, t, keys, cov)).collect();
    if indexed && index_when == "before" {
        ddl_phase(ctx, &mut engines, "create-index", &idx);
    }

    // ---- load: one autocommit INSERT a row, as both Python drivers ----
    let data = generate(seed, &sz);
    let ins: Vec<Stmt> = TABLES
        .iter()
        .map(|t| {
            let marks = vec!["?"; t.cols.len()].join(", ");
            engines[0].prepare(&format!("INSERT INTO {} VALUES ({marks})", t.name)).unwrap_or_else(|e| panic!("{}: {e}", t.name))
        })
        .collect();
    // Each INSERT's first executions (Oracle's hard parse, a PostgreSQL
    // backend's first touch of the relation) out of the phase: six a
    // relation, each in a transaction rolled back, on an id the load never
    // writes.
    for (t, rows) in data.iter().enumerate() {
        let Some(first) = rows.first() else { continue };
        let mut w = first.clone();
        w[0] = Val::I64(1 << 40);
        for _ in 0..6 {
            engines[0].begin().unwrap();
            engines[0].exec(ins[t], &w).unwrap_or_else(|err| panic!("load warm-up {}: {err}", TABLES[t].name));
            engines[0].rollback().unwrap();
        }
    }
    ctx.phase("load", &mut engines, |_, e, r| {
        let mut shown = 0;
        for (t, rows) in data.iter().enumerate() {
            for row in rows {
                let t0 = Instant::now();
                match e.exec(ins[t], row) {
                    Ok(n) => {
                        r.add(t0);
                        r.rows += n;
                    }
                    Err(err) => {
                        if shown < 3 {
                            eprintln!("load {}: {err}", TABLES[t].name);
                            shown += 1;
                        }
                        r.error(&class_name(err.class));
                    }
                }
            }
        }
    });
    if indexed && index_when == "after" {
        ddl_phase(ctx, &mut engines, "create-index", &idx);
    }
    analyze(engines[0].as_mut(), &TABLES.map(|t| t.name));

    // ---- the read shapes ----
    let shapes = shapes();
    let stmts: Vec<Stmt> =
        shapes.iter().map(|s| engines[0].prepare(&s.sql).unwrap_or_else(|e| panic!("prepare {}: {e}", s.name))).collect();
    let mut rows = Rows::new();
    for (i, sh) in shapes.iter().enumerate() {
        let st = stmts[i];
        // Unrecorded warm-up, on a stream of its own.
        let mut wr = Rng::new(sub_seed(seed, 500 + i as u64));
        for _ in 0..warmup {
            let _ = engines[0].query(st, &(sh.args)(&mut wr, &sz), &mut rows);
        }
        let phase_seed = sub_seed(seed, 1 + i as u64);
        ctx.phase(sh.name, &mut engines, |_, e, r| {
            let mut rng = Rng::new(phase_seed);
            let mut rows = Rows::new();
            let mut shown = 0;
            for _ in 0..ops {
                let a = (sh.args)(&mut rng, &sz);
                let t0 = Instant::now();
                match e.query(st, &a, &mut rows) {
                    Ok(()) => {
                        r.add(t0);
                        r.rows += rows.len() as u64;
                    }
                    Err(err) => {
                        if shown < 3 {
                            eprintln!("{}: {err}", sh.name);
                            shown += 1;
                        }
                        r.error(&class_name(err.class));
                    }
                }
            }
        });
    }

    // ---- verification: the Python's five invariants, and two more ----
    let e = engines[0].as_mut();
    let q = |e: &mut dyn Engine, s: Stmt, a: &[Val], out: &mut Rows| e.query(s, a, out).map_err(|err| err.to_string());

    // 1. Each relation holds the rows the load wrote.
    let mut bad = Vec::new();
    for (t, want) in TABLES.iter().zip(data.iter().map(|d| d.len() as i64)) {
        let s = e.prepare(&format!("SELECT COUNT(*) FROM {}", t.name)).unwrap();
        match q(e, s, &[], &mut rows) {
            Ok(()) if rows.len() == 1 && rows[0][0].i() == want => {}
            Ok(()) => bad.push(format!("{}: COUNT(*) = {:?}, loaded {want}", t.name, rows.first())),
            Err(m) => bad.push(format!("{}: {m}", t.name)),
        }
    }
    ctx.check("row-counts", bad.is_empty(), bad.join("; "));

    // The full scans every later check is measured against.
    let scan_loans = e.prepare("SELECT user_id, book_id FROM s3_loans").unwrap();
    let scan_users = e.prepare("SELECT id, member_code FROM s3_users").unwrap();
    let mut truth: BTreeMap<i64, Vec<i64>> = BTreeMap::new();
    let mut scanned: Vec<(i64, i64)> = Vec::new();
    q(e, scan_loans, &[], &mut rows).expect("loans scan");
    for r in &rows {
        truth.entry(r[0].i()).or_default().push(r[1].i());
        scanned.push((r[0].i(), r[1].i()));
    }
    let mut loaded: Vec<(i64, i64)> = data[3].iter().map(|r| (r[1].i(), r[2].i())).collect();
    scanned.sort_unstable();
    loaded.sort_unstable();
    ctx.check(
        "loans-scan-equals-load",
        scanned == loaded,
        format!("{} rows scanned, {} loaded", scanned.len(), loaded.len()),
    );
    q(e, scan_users, &[], &mut rows).expect("users scan");
    let members: BTreeMap<i64, String> = rows.iter().map(|r| (r[0].i(), r[1].s().to_string())).collect();

    // 2. `loans-by-user` and `count-by-user`, the measured statements,
    // against the scan, for a sample of distinct users.
    let (by_user, count_by_user) = (stmts[1], stmts[11]);
    let mut pool: Vec<i64> = (1..=sz.users).collect();
    let mut vr = Rng::new(sub_seed(seed, 999));
    let n = (verify as usize).min(pool.len());
    for j in 0..n {
        let pick = vr.range(j as i64, pool.len() as i64 - 1) as usize;
        pool.swap(j, pick);
    }
    let mut bad = Vec::new();
    for &uid in &pool[..n] {
        let mut want = truth.get(&uid).cloned().unwrap_or_default();
        want.sort_unstable();
        match q(e, by_user, &[Val::I64(uid)], &mut rows) {
            Ok(()) => {
                let mut got: Vec<i64> = rows.iter().map(|r| r[0].i()).collect();
                got.sort_unstable();
                if got != want {
                    bad.push(format!("user_id = {uid}: {} rows, scan says {}", got.len(), want.len()));
                }
            }
            Err(m) => bad.push(format!("user_id = {uid}: {m}")),
        }
        match q(e, count_by_user, &[Val::I64(uid)], &mut rows) {
            Ok(()) if rows.len() == 1 && rows[0][0].i() == want.len() as i64 => {}
            Ok(()) => bad.push(format!("COUNT(*) user_id = {uid}: {:?}, scan says {}", rows.first(), want.len())),
            Err(m) => bad.push(format!("COUNT(*) user_id = {uid}: {m}")),
        }
    }
    ctx.check("loans-by-user-vs-scan", bad.is_empty(), format!("{n} users sampled; {}", bad.join("; ")));

    // 3. Every loan's user_id names a user.
    let orphans: Vec<i64> = truth.keys().filter(|u| !members.contains_key(u)).copied().collect();
    ctx.check("loan-user-exists", orphans.is_empty(), format!("{} user_ids name no user: {:?}", orphans.len(), &orphans[..orphans.len().min(5)]));

    // 4. join-no-literal: every (book_id, member_code) of users 1..16, as a
    // multiset (no ORDER BY, so no order is asserted).
    let mut want: Vec<(i64, String)> = Vec::new();
    for uid in 1..=JOIN_OUTER_K {
        if let Some(code) = members.get(&uid) {
            want.extend(truth.get(&uid).into_iter().flatten().map(|b| (*b, code.clone())));
        }
    }
    want.sort();
    let got = q(e, stmts[9], &[], &mut rows).map(|()| {
        let mut g: Vec<(i64, String)> = rows.iter().map(|r| (r[0].i(), r[1].s().to_string())).collect();
        g.sort();
        g
    });
    ctx.check(
        "join-no-literal",
        got.as_ref() == Ok(&want),
        match &got {
            Ok(g) => format!("{} rows, expectation {}", g.len(), want.len()),
            Err(m) => m.clone(),
        },
    );

    // 5. exists-correlated: the ids of users 1..20 with a loan.
    let want: Vec<i64> = (1..=EXISTS_OUTER_K).filter(|u| members.contains_key(u) && truth.contains_key(u)).collect();
    let got = q(e, stmts[10], &[], &mut rows).map(|()| {
        let mut g: Vec<i64> = rows.iter().map(|r| r[0].i()).collect();
        g.sort_unstable();
        g
    });
    ctx.check(
        "exists-correlated",
        got.as_ref() == Ok(&want),
        match &got {
            Ok(g) => format!("{} ids, expectation {}", g.len(), want.len()),
            Err(m) => m.clone(),
        },
    );

    // No phase met a refusal (the Python exits 1 on any error reply).
    let refused: Vec<String> =
        ctx.phases.iter().filter(|p| !p.errors.is_empty()).map(|p| format!("{}: {:?}", p.name, p.errors)).collect();
    ctx.check("no-refusals", refused.is_empty(), refused.join("; "));
}
