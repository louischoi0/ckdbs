//! A check that each backend round-trips every type the scenarios use and
//! reports a profile: create, load, point read, range, update in a
//! transaction, aggregate. Not a benchmark.

use super::*;

pub fn run(ctx: &mut Ctx) {
    let k = ctx.kind;
    let mut e = ctx.connect();
    drop_table(e.as_mut(), "eq_smoke");
    let t = Table {
        name: "eq_smoke",
        cols: &[("id", Ty::I64), ("grp", Ty::I32), ("qty", Ty::I64), ("tag", Ty::Str(24))],
        store: Store::Btree,
        refs: &[],
    };
    e.run(&create_table(k, &t)).unwrap();
    let mut rng = Rng::new(ctx.seed);
    let n = ctx.opt("rows", 2000) as i64;
    load(
        e.as_mut(),
        "eq_smoke",
        4,
        (1..=n).map(|i| vec![Val::I64(i), Val::I64(i % 10), Val::I64(i * 3), Val::Str(rng.word(8))]),
        500,
    );
    analyze(e.as_mut(), &["eq_smoke"]);
    let pk = e.prepare("SELECT id, grp, qty, tag FROM eq_smoke WHERE id = ?").unwrap();
    let range = e.prepare(&format!("SELECT id, qty FROM eq_smoke WHERE id >= ? AND id <= ? ORDER BY id")).unwrap();
    let upd = e.prepare("UPDATE eq_smoke SET qty = qty + ? WHERE id = ?").unwrap();
    let agg = e.prepare("SELECT grp, COUNT(*), SUM(qty) FROM eq_smoke GROUP BY grp ORDER BY grp").unwrap();
    let mut engines = vec![e];
    let mut rows = Rows::new();

    ctx.phase("pk", &mut engines, |_, e, r| {
        let mut rng = Rng::new(7);
        let mut rows = Rows::new();
        for _ in 0..500 {
            let id = rng.range(1, n);
            let t0 = Instant::now();
            e.query(pk, &[Val::I64(id)], &mut rows).unwrap();
            r.add(t0);
            assert_eq!(rows.len(), 1);
            assert_eq!(rows[0][0], Val::I64(id));
            assert_eq!(rows[0][2], Val::I64(id * 3));
            r.rows += 1;
        }
    });
    ctx.phase("range100", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for i in 0..200 {
            let lo = 1 + (i * 7) % (n - 100);
            let t0 = Instant::now();
            e.query(range, &[Val::I64(lo), Val::I64(lo + 99)], &mut rows).unwrap();
            r.add(t0);
            assert_eq!(rows.len(), 100);
            r.rows += rows.len() as u64;
        }
    });
    ctx.phase("update-txn", &mut engines, |_, e, r| {
        for i in 0..300 {
            let t0 = Instant::now();
            e.begin().unwrap();
            e.exec(upd, &[Val::I64(1), Val::I64(1 + i % n)]).unwrap();
            e.exec(upd, &[Val::I64(-1), Val::I64(1 + (i + 1) % n)]).unwrap();
            e.commit().unwrap();
            r.add(t0);
        }
    });
    ctx.phase("agg", &mut engines, |_, e, r| {
        let mut rows = Rows::new();
        for _ in 0..50 {
            let t0 = Instant::now();
            e.query(agg, &[], &mut rows).unwrap();
            r.add(t0);
            assert_eq!(rows.len(), 10);
        }
    });
    engines[0].query(agg, &[], &mut rows).unwrap();
    let total: i64 = rows.iter().map(|r| r[2].i()).sum();
    let expect: i64 = (1..=n).map(|i| i * 3).sum();
    ctx.check("sum-conserved", total == expect, format!("{total} vs {expect}"));
}
