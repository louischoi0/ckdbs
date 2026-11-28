//! Scenario 5: commercial OLTP, an order-entry workload (EN §4).
//!
//! After the shape of TPC-C's five transactions - **not TPC-C**: not
//! audited, not the specification's full rules (no keying or think time,
//! no customer lookup by last name, scaled-down cardinalities), and its
//! rate is no tpmC. Nine relations, every key a flat `int64` the client
//! computes, money in cents.
//!
//! | relation | key | KDS kind | why |
//! |---|---|---|---|
//! | `s5_warehouses` | `w_id` | BTREE | every read and write is a point by key |
//! | `s5_districts` | `w_id*100 + d` | BTREE | point by key (the next order id, the year to date) |
//! | `s5_customers` | `district*100000 + c` | BTREE | point by key (payment, new order, delivery) |
//! | `s5_items` | `i_id` | BTREE | point by key |
//! | `s5_stock` | `w_id*1000000 + i_id` | BTREE | point by key; the stock-level join probes it by key |
//! | `s5_orders` | `district*10000000 + o_id` | BTREE | point by key (delivery); order status by an index on the customer, then by key |
//! | `s5_order_lines` | `order*100 + number` | BTREE | every read is a key range (an order's lines, a district's last 20 orders) |
//! | `s5_new_orders` | the order's key | BTREE | delivery takes a district's oldest by a key range, ordered by key |
//! | `s5_history` | issued by the client | HEAP | append only: never read but by the closing check's walk |
//!
//! The mix, from one seeded generator a terminal: new order 45 %, payment
//! 43 %, order status 4 %, delivery 4 %, stock level 4 %, each one
//! transaction; a new order whose last item does not exist (1 %) rolls back
//! as TPC-C's does and is counted apart. A write conflict or deadlock is
//! rolled back and retried up to 5 times, counted.
//!
//! Phases: each transaction alone on one terminal first (`new-order`,
//! `payment`, `order-status`, `delivery`, `stock-level`), on the state the
//! load left, so each engine's breakdown is one transaction's on the same
//! data; then `mix` (all terminals, `seconds` long: throughput and each
//! transaction's latency). So that each alone phase does the same
//! statements on every engine, every customer holds an order
//! from the load (`orders` >= `customers`, both 1,000 by default: `order-status`
//! always finds one) and a district's queue of undelivered orders (30 % of
//! `orders`) outlasts `delivery`'s `alone` runs (each takes one order a
//! district) at the defaults.
//!
//! Checks (TPC-C's consistency conditions 1, 2, 3 and 4 in spirit, and two
//! of this schema's own): each warehouse's year to date is its districts'
//! sum; each district's next order id is one above its highest order and
//! its highest new order; every order's line count is its lines; the
//! stock's year to date is the quantity ordered since the load; every
//! customer's balance is what was delivered less what was paid.

use super::*;
use std::sync::atomic::{AtomicI64, Ordering};

const DISTRICTS: i64 = 10;
const MAX_LINES: i64 = 15;
const INITIAL_YTD: i64 = 30_000_00; // a district's; a warehouse's is 10x

fn d_key(w: i64, d: i64) -> i64 {
    w * 100 + d
}
fn c_key(dk: i64, c: i64) -> i64 {
    dk * 100_000 + c
}
fn s_key(w: i64, i: i64) -> i64 {
    w * 1_000_000 + i
}
fn o_key(dk: i64, o: i64) -> i64 {
    dk * 10_000_000 + o
}
fn ol_key(ok: i64, n: i64) -> i64 {
    ok * 100 + n
}

struct Scale {
    warehouses: i64,
    customers: i64,
    items: i64,
    orders: i64,
}

#[derive(Clone, Copy)]
struct St {
    w_tax: Stmt,
    w_name: Stmt,
    w_ytd: Stmt,
    d_next: Stmt,
    d_bump: Stmt,
    d_ytd: Stmt,
    d_name: Stmt,
    c_info: Stmt,
    c_pay: Stmt,
    c_after_pay: Stmt,
    c_deliver: Stmt,
    c_status: Stmt,
    i_get: Stmt,
    s_get: Stmt,
    s_upd: Stmt,
    o_ins: Stmt,
    no_ins: Stmt,
    ol_ins: Stmt,
    h_ins: Stmt,
    o_last: Stmt,
    ol_of: Stmt,
    no_oldest: Stmt,
    no_del: Stmt,
    o_cust: Stmt,
    o_carrier: Stmt,
    ol_deliver: Stmt,
    ol_sum: Stmt,
    stock_level: Stmt,
}

fn prepare(e: &mut dyn Engine) -> St {
    let k = e.kind();
    let one = limit(k, 1);
    let mut p = |sql: &str| e.prepare(sql).unwrap_or_else(|err| panic!("{err}"));
    St {
        w_tax: p("SELECT w_tax FROM s5_warehouses WHERE w_id = ?"),
        w_name: p("SELECT w_name FROM s5_warehouses WHERE w_id = ?"),
        w_ytd: p("UPDATE s5_warehouses SET w_ytd = w_ytd + ? WHERE w_id = ?"),
        d_next: p("SELECT d_tax, d_next_o_id FROM s5_districts WHERE d_key = ?"),
        d_bump: p("UPDATE s5_districts SET d_next_o_id = d_next_o_id + 1 WHERE d_key = ?"),
        d_ytd: p("UPDATE s5_districts SET d_ytd = d_ytd + ? WHERE d_key = ?"),
        d_name: p("SELECT d_name FROM s5_districts WHERE d_key = ?"),
        c_info: p("SELECT c_discount, c_last, c_credit FROM s5_customers WHERE c_key = ?"),
        c_pay: p(
            "UPDATE s5_customers SET c_balance = c_balance - ?, c_ytd_payment = c_ytd_payment + ?, \
             c_payment_cnt = c_payment_cnt + 1 WHERE c_key = ?",
        ),
        c_after_pay: p("SELECT c_balance, c_credit FROM s5_customers WHERE c_key = ?"),
        c_deliver: p(
            "UPDATE s5_customers SET c_balance = c_balance + ?, c_delivery_cnt = c_delivery_cnt + 1 WHERE c_key = ?",
        ),
        c_status: p("SELECT c_balance, c_last FROM s5_customers WHERE c_key = ?"),
        i_get: p("SELECT i_price, i_name, i_data FROM s5_items WHERE i_id = ?"),
        s_get: p("SELECT s_quantity, s_dist_info FROM s5_stock WHERE s_key = ?"),
        // The quantity moves in the row's own update: read-and-write in one
        // statement on every engine, so concurrent terminals lose nothing.
        s_upd: p(
            "UPDATE s5_stock SET s_quantity = CASE WHEN s_quantity - ? >= 10 THEN s_quantity - ? \
             ELSE s_quantity - ? + 91 END, s_ytd = s_ytd + ?, s_order_cnt = s_order_cnt + 1, \
             s_remote_cnt = s_remote_cnt + ? WHERE s_key = ?",
        ),
        o_ins: p("INSERT INTO s5_orders VALUES (?, ?, ?, ?, ?, ?, ?, ?)"),
        no_ins: p("INSERT INTO s5_new_orders VALUES (?, ?, ?)"),
        ol_ins: p("INSERT INTO s5_order_lines VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"),
        h_ins: p("INSERT INTO s5_history VALUES (?, ?, ?, ?, ?, ?)"),
        o_last: p(&format!(
            "SELECT o_key, o_entry_d, o_carrier_id FROM s5_orders WHERE o_c_key = ? ORDER BY o_key DESC{one}"
        )),
        ol_of: p(
            "SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM s5_order_lines \
             WHERE ol_key >= ? AND ol_key <= ?",
        ),
        no_oldest: p(&format!(
            "SELECT no_key FROM s5_new_orders WHERE no_key >= ? AND no_key <= ? ORDER BY no_key{one}"
        )),
        no_del: p("DELETE FROM s5_new_orders WHERE no_key = ?"),
        o_cust: p("SELECT o_c_key FROM s5_orders WHERE o_key = ?"),
        o_carrier: p("UPDATE s5_orders SET o_carrier_id = ? WHERE o_key = ?"),
        ol_deliver: p("UPDATE s5_order_lines SET ol_delivery_d = ? WHERE ol_key >= ? AND ol_key <= ?"),
        ol_sum: p("SELECT SUM(ol_amount) FROM s5_order_lines WHERE ol_key >= ? AND ol_key <= ?"),
        // Full names, no aliases: KDS takes a table alias only after `AS`,
        // which Oracle refuses.
        stock_level: p(
            "SELECT COUNT(DISTINCT s5_stock.s_i_id) FROM s5_order_lines JOIN s5_stock \
             ON s5_stock.s_key = s5_order_lines.ol_s_key \
             WHERE s5_order_lines.ol_key >= ? AND s5_order_lines.ol_key <= ? AND s5_stock.s_quantity < ?",
        ),
    }
}

fn create(e: &mut dyn Engine) {
    let k = e.kind();
    for t in ["s5_history", "s5_new_orders", "s5_order_lines", "s5_orders", "s5_stock", "s5_items", "s5_customers",
        "s5_districts", "s5_warehouses"]
    {
        drop_table(e, t);
    }
    use Ty::*;
    let tables = [
        Table {
            name: "s5_warehouses",
            cols: &[("w_id", I64), ("w_name", Str(16)), ("w_tax", I64), ("w_ytd", I64)],
            store: Store::Btree,
            refs: &[],
        },
        Table {
            name: "s5_districts",
            cols: &[
                ("d_key", I64),
                ("d_w_id", I64),
                ("d_name", Str(16)),
                ("d_tax", I64),
                ("d_ytd", I64),
                ("d_next_o_id", I64),
            ],
            store: Store::Btree,
            refs: &[],
        },
        Table {
            name: "s5_customers",
            cols: &[
                ("c_key", I64),
                ("c_d_key", I64),
                ("c_last", Str(16)),
                ("c_credit", Str(16)),
                ("c_discount", I64),
                ("c_balance", I64),
                ("c_ytd_payment", I64),
                ("c_payment_cnt", I64),
                ("c_delivery_cnt", I64),
                ("c_data", Str(64)),
            ],
            store: Store::Btree,
            refs: &[],
        },
        Table {
            name: "s5_items",
            cols: &[("i_id", I64), ("i_name", Str(24)), ("i_price", I64), ("i_data", Str(32))],
            store: Store::Btree,
            refs: &[],
        },
        Table {
            name: "s5_stock",
            cols: &[
                ("s_key", I64),
                ("s_w_id", I64),
                ("s_i_id", I64),
                ("s_quantity", I64),
                ("s_ytd", I64),
                ("s_order_cnt", I64),
                ("s_remote_cnt", I64),
                ("s_dist_info", Str(24)),
            ],
            store: Store::Btree,
            refs: &[],
        },
        Table {
            name: "s5_orders",
            cols: &[
                ("o_key", I64),
                ("o_d_key", I64),
                ("o_id", I64),
                ("o_c_key", I64),
                ("o_entry_d", I64),
                ("o_carrier_id", I64),
                ("o_ol_cnt", I64),
                ("o_all_local", I64),
            ],
            store: Store::Btree,
            refs: &[],
        },
        Table {
            name: "s5_order_lines",
            cols: &[
                ("ol_key", I64),
                ("ol_o_key", I64),
                ("ol_o_id", I64),
                ("ol_number", I64),
                ("ol_i_id", I64),
                ("ol_supply_w_id", I64),
                ("ol_s_key", I64),
                ("ol_delivery_d", I64),
                ("ol_quantity", I64),
                ("ol_amount", I64),
                ("ol_dist_info", Str(24)),
            ],
            store: Store::Btree,
            refs: &[],
        },
        Table {
            name: "s5_new_orders",
            cols: &[("no_key", I64), ("no_d_key", I64), ("no_o_id", I64)],
            store: Store::Btree,
            refs: &[],
        },
        Table {
            name: "s5_history",
            cols: &[
                ("h_id", I64),
                ("h_c_key", I64),
                ("h_d_key", I64),
                ("h_date", I64),
                ("h_amount", I64),
                ("h_data", Str(24)),
            ],
            store: Store::Heap,
            refs: &[],
        },
    ];
    for t in &tables {
        e.run(&create_table(k, t)).unwrap_or_else(|err| panic!("{err}"));
    }
}

fn load_all(e: &mut dyn Engine, sc: &Scale, rng: &mut Rng) {
    let w = sc.warehouses;
    load(e, "s5_warehouses", 4, (1..=w).map(|i| vec![i.into(), format!("wh{i}").into(), 500.into(), (INITIAL_YTD * DISTRICTS).into()]), 1000);
    let mut ds = Vec::new();
    for wi in 1..=w {
        for d in 1..=DISTRICTS {
            ds.push(vec![
                d_key(wi, d).into(),
                wi.into(),
                format!("d{wi}-{d}").into(),
                (100 * d).into(),
                INITIAL_YTD.into(),
                (sc.orders + 1).into(),
            ]);
        }
    }
    load(e, "s5_districts", 6, ds.into_iter(), 1000);
    let mut r2 = rng.clone();
    let customers = (1..=w).flat_map(|wi| (1..=DISTRICTS).map(move |d| d_key(wi, d))).flat_map(|dk| {
        (1..=sc.customers).map(move |c| (dk, c))
    });
    load(
        e,
        "s5_customers",
        10,
        customers.map(|(dk, c)| {
            vec![
                c_key(dk, c).into(),
                dk.into(),
                format!("last{}", c % 1000).into(),
                (if r2.chance(10) { "BC" } else { "GC" }).into(),
                r2.range(0, 5000).into(),
                (-10_00i64).into(),
                10_00i64.into(),
                1i64.into(),
                0i64.into(),
                r2.word(40).into(),
            ]
        }),
        1000,
    );
    let mut r3 = rng.clone();
    load(
        e,
        "s5_items",
        4,
        (1..=sc.items).map(|i| vec![i.into(), format!("item{i}").into(), r3.range(1_00, 100_00).into(), r3.word(26).into()]),
        1000,
    );
    let mut r4 = rng.clone();
    let stock = (1..=w).flat_map(|wi| (1..=sc.items).map(move |i| (wi, i)));
    load(
        e,
        "s5_stock",
        8,
        stock.map(|(wi, i)| {
            vec![
                s_key(wi, i).into(),
                wi.into(),
                i.into(),
                r4.range(10, 100).into(),
                0i64.into(),
                0i64.into(),
                0i64.into(),
                r4.word(24).into(),
            ]
        }),
        1000,
    );
    // Initial orders: the first 70 % delivered (carrier set, lines delivered
    // with amount 0, as TPC-C's population), the last 30 % new orders.
    let mut orders = Vec::new();
    let mut lines = Vec::new();
    let mut news = Vec::new();
    let delivered_upto = sc.orders * 7 / 10;
    for wi in 1..=w {
        for d in 1..=DISTRICTS {
            let dk = d_key(wi, d);
            for o in 1..=sc.orders {
                let ok = o_key(dk, o);
                let c = 1 + (o - 1) % sc.customers;
                let cnt = rng.range(5, MAX_LINES);
                let delivered = o <= delivered_upto;
                orders.push(vec![
                    ok.into(),
                    dk.into(),
                    o.into(),
                    c_key(dk, c).into(),
                    1_000_000i64.into(),
                    (if delivered { rng.range(1, 10) } else { 0 }).into(),
                    cnt.into(),
                    1i64.into(),
                ]);
                for n in 1..=cnt {
                    let i = rng.range(1, sc.items);
                    lines.push(vec![
                        ol_key(ok, n).into(),
                        ok.into(),
                        o.into(),
                        n.into(),
                        i.into(),
                        wi.into(),
                        s_key(wi, i).into(),
                        (if delivered { 1_000_000 } else { 0 }).into(),
                        5i64.into(),
                        (if delivered { 0 } else { rng.range(1, 9999_99) }).into(),
                        rng.word(24).into(),
                    ]);
                }
                if !delivered {
                    news.push(vec![ok.into(), dk.into(), o.into()]);
                }
            }
        }
    }
    load(e, "s5_orders", 8, orders.into_iter(), 1000);
    load(e, "s5_order_lines", 11, lines.into_iter(), 1000);
    load(e, "s5_new_orders", 3, news.into_iter(), 1000);
    // Each customer's initial payment, as TPC-C's history population.
    let hist = (1..=w).flat_map(|wi| (1..=DISTRICTS).map(move |d| d_key(wi, d))).flat_map(|dk| {
        (1..=sc.customers).map(move |c| (dk, c))
    });
    let mut hid = 0i64;
    load(
        e,
        "s5_history",
        6,
        hist.map(|(dk, c)| {
            hid += 1;
            vec![hid.into(), c_key(dk, c).into(), dk.into(), 1_000_000i64.into(), 10_00i64.into(), "initial".into()]
        }),
        1000,
    );
    e.run("CREATE INDEX s5_orders_cust ON s5_orders (o_c_key)").unwrap_or_else(|err| panic!("{err}"));
}

#[derive(PartialEq)]
enum Outcome {
    Committed,
    BusinessRollback,
}

/// One transaction with retries; the outcome, or the last refusal.
fn txn(e: &mut dyn Engine, r: &mut Recorder, mut body: impl FnMut(&mut dyn Engine) -> DbResult<Outcome>) -> Option<Outcome> {
    for attempt in 0..=5 {
        let res = e.begin().and_then(|_| body(e)).and_then(|o| {
            if o == Outcome::Committed {
                e.commit().map(|_| o)
            } else {
                e.rollback().map(|_| o)
            }
        });
        match res {
            Ok(o) => return Some(o),
            Err(err) => {
                let _ = e.rollback();
                if err.class == ErrClass::Conflict && attempt < 5 {
                    r.error("conflict-retried");
                    continue;
                }
                r.error(&format!("{:?}", err.class));
                eprintln!("s5: {err}");
                return None;
            }
        }
    }
    None
}

struct Gen {
    rng: Rng,
    home: i64,
    sc: (i64, i64, i64), // warehouses, customers, items
    hist: i64,
}

impl Gen {
    fn district(&mut self) -> i64 {
        self.rng.range(1, DISTRICTS)
    }
    fn customer(&mut self) -> i64 {
        self.rng.range(1, self.sc.1)
    }
    fn other_w(&mut self) -> i64 {
        if self.sc.0 == 1 {
            self.home
        } else {
            let mut w = self.rng.range(1, self.sc.0 - 1);
            if w >= self.home {
                w += 1;
            }
            w
        }
    }
}

fn new_order(e: &mut dyn Engine, st: &St, g: &mut Gen, r: &mut Recorder, rows: &mut Rows) -> Option<Outcome> {
    let w = g.home;
    let d = g.district();
    let dk = d_key(w, d);
    let ck = c_key(dk, g.customer());
    let cnt = g.rng.range(5, MAX_LINES);
    let rollback = g.rng.chance(1);
    let mut lines: Vec<(i64, i64, i64)> = (0..cnt)
        .map(|_| {
            let i = g.rng.range(1, g.sc.2);
            let supply = if g.rng.chance(1) { g.other_w() } else { w };
            (i, supply, g.rng.range(1, 10))
        })
        .collect();
    // Stock rows taken in key order, so two new orders cannot deadlock on them.
    lines.sort_by_key(|&(i, sw, _)| s_key(sw, i));
    if rollback {
        lines.last_mut().unwrap().0 = g.sc.2 + 1; // an item that does not exist
    }
    let all_local = lines.iter().all(|&(_, sw, _)| sw == w) as i64;
    txn(e, r, |e| {
        e.query(st.w_tax, &[w.into()], rows)?;
        e.exec(st.d_bump, &[dk.into()])?;
        e.query(st.d_next, &[dk.into()], rows)?;
        let o = rows[0][1].i() - 1;
        let ok = o_key(dk, o);
        e.query(st.c_info, &[ck.into()], rows)?;
        e.exec(st.o_ins, &[ok.into(), dk.into(), o.into(), ck.into(), 2_000_000i64.into(), 0i64.into(), cnt.into(), all_local.into()])?;
        e.exec(st.no_ins, &[ok.into(), dk.into(), o.into()])?;
        for (n, &(i, sw, q)) in lines.iter().enumerate() {
            e.query(st.i_get, &[i.into()], rows)?;
            if rows.is_empty() {
                return Ok(Outcome::BusinessRollback);
            }
            let price = rows[0][0].i();
            let sk = s_key(sw, i);
            e.query(st.s_get, &[sk.into()], rows)?;
            let dist = rows[0][1].clone();
            e.exec(st.s_upd, &[q.into(), q.into(), q.into(), q.into(), ((sw != w) as i64).into(), sk.into()])?;
            let n = n as i64 + 1;
            e.exec(
                st.ol_ins,
                &[
                    ol_key(ok, n).into(),
                    ok.into(),
                    o.into(),
                    n.into(),
                    i.into(),
                    sw.into(),
                    sk.into(),
                    0i64.into(),
                    q.into(),
                    (q * price).into(),
                    dist,
                ],
            )?;
        }
        Ok(Outcome::Committed)
    })
}

fn payment(e: &mut dyn Engine, st: &St, g: &mut Gen, terminal: usize, r: &mut Recorder, rows: &mut Rows) -> Option<Outcome> {
    let w = g.home;
    let dk = d_key(w, g.district());
    // 85 % a home customer, 15 % a remote warehouse's
    let cw = if g.rng.chance(85) { w } else { g.other_w() };
    let cdk = d_key(cw, g.rng.range(1, DISTRICTS));
    let ck = c_key(cdk, g.customer());
    let amount = g.rng.range(1_00, 5000_00);
    g.hist += 1;
    let hid = (terminal as i64 + 1) * 1_000_000_000_000 + g.hist;
    txn(e, r, |e| {
        e.exec(st.w_ytd, &[amount.into(), w.into()])?;
        e.query(st.w_name, &[w.into()], rows)?;
        e.exec(st.d_ytd, &[amount.into(), dk.into()])?;
        e.query(st.d_name, &[dk.into()], rows)?;
        e.exec(st.c_pay, &[amount.into(), amount.into(), ck.into()])?;
        e.query(st.c_after_pay, &[ck.into()], rows)?;
        e.exec(st.h_ins, &[hid.into(), ck.into(), dk.into(), 2_000_000i64.into(), amount.into(), "payment".into()])?;
        Ok(Outcome::Committed)
    })
}

fn order_status(e: &mut dyn Engine, st: &St, g: &mut Gen, r: &mut Recorder, rows: &mut Rows) -> Option<Outcome> {
    let dk = d_key(g.home, g.district());
    let ck = c_key(dk, g.customer());
    txn(e, r, |e| {
        e.query(st.c_status, &[ck.into()], rows)?;
        e.query(st.o_last, &[ck.into()], rows)?;
        if let Some(row) = rows.first() {
            let ok = row[0].i();
            e.query(st.ol_of, &[ol_key(ok, 0).into(), ol_key(ok, 99).into()], rows)?;
        }
        Ok(Outcome::Committed)
    })
}

fn delivery(e: &mut dyn Engine, st: &St, g: &mut Gen, r: &mut Recorder, rows: &mut Rows) -> Option<Outcome> {
    let w = g.home;
    let carrier = g.rng.range(1, 10);
    txn(e, r, |e| {
        for d in 1..=DISTRICTS {
            let dk = d_key(w, d);
            e.query(st.no_oldest, &[o_key(dk, 0).into(), o_key(dk, 9_999_999).into()], rows)?;
            let Some(row) = rows.first() else { continue };
            let ok = row[0].i();
            if e.exec(st.no_del, &[ok.into()])? == 0 {
                continue; // another delivery took it
            }
            e.query(st.o_cust, &[ok.into()], rows)?;
            let ck = rows[0][0].i();
            e.exec(st.o_carrier, &[carrier.into(), ok.into()])?;
            e.exec(st.ol_deliver, &[3_000_000i64.into(), ol_key(ok, 0).into(), ol_key(ok, 99).into()])?;
            e.query(st.ol_sum, &[ol_key(ok, 0).into(), ol_key(ok, 99).into()], rows)?;
            let total = match &rows[0][0] {
                Val::Null => 0,
                v => v.i(),
            };
            e.exec(st.c_deliver, &[total.into(), ck.into()])?;
        }
        Ok(Outcome::Committed)
    })
}

fn stock_level(e: &mut dyn Engine, st: &St, g: &mut Gen, r: &mut Recorder, rows: &mut Rows) -> Option<Outcome> {
    let dk = d_key(g.home, g.district());
    let threshold = g.rng.range(10, 20);
    txn(e, r, |e| {
        e.query(st.d_next, &[dk.into()], rows)?;
        let next = rows[0][1].i();
        let lo = ol_key(o_key(dk, (next - 20).max(1)), 0);
        let hi = ol_key(o_key(dk, next - 1), 99);
        e.query(st.stock_level, &[lo.into(), hi.into(), threshold.into()], rows)?;
        Ok(Outcome::Committed)
    })
}

#[derive(Clone, Copy, PartialEq)]
enum Tx {
    NewOrder,
    Payment,
    OrderStatus,
    Delivery,
    StockLevel,
}

impl Tx {
    fn name(self) -> &'static str {
        match self {
            Tx::NewOrder => "new-order",
            Tx::Payment => "payment",
            Tx::OrderStatus => "order-status",
            Tx::Delivery => "delivery",
            Tx::StockLevel => "stock-level",
        }
    }
    fn pick(rng: &mut Rng) -> Tx {
        match rng.range(1, 100) {
            1..=45 => Tx::NewOrder,
            46..=88 => Tx::Payment,
            89..=92 => Tx::OrderStatus,
            93..=96 => Tx::Delivery,
            _ => Tx::StockLevel,
        }
    }
}

fn run_one(tx: Tx, e: &mut dyn Engine, st: &St, g: &mut Gen, terminal: usize, r: &mut Recorder, rows: &mut Rows,
    counts: &[AtomicI64; 3]) {
    let t0 = Instant::now();
    let out = match tx {
        Tx::NewOrder => new_order(e, st, g, r, rows),
        Tx::Payment => payment(e, st, g, terminal, r, rows),
        Tx::OrderStatus => order_status(e, st, g, r, rows),
        Tx::Delivery => delivery(e, st, g, r, rows),
        Tx::StockLevel => stock_level(e, st, g, r, rows),
    };
    match out {
        Some(Outcome::Committed) => {
            r.add_kind(tx.name(), t0);
            if tx == Tx::NewOrder {
                counts[0].fetch_add(1, Ordering::Relaxed);
            }
        }
        Some(Outcome::BusinessRollback) => {
            r.add_kind("new-order-rollback", t0);
            counts[1].fetch_add(1, Ordering::Relaxed);
        }
        None => {
            counts[2].fetch_add(1, Ordering::Relaxed);
        }
    }
}

pub fn run(ctx: &mut Ctx) {
    let sc = Scale {
        warehouses: ctx.opt("warehouses", 2) as i64,
        customers: ctx.opt("customers", 1000) as i64,
        items: ctx.opt("items", 10_000) as i64,
        orders: ctx.opt("orders", 1000) as i64,
    };
    let terminals = ctx.opt("terminals", 4) as usize;
    let seconds = ctx.opt("seconds", 60) as f64;
    let alone = ctx.opt("alone", 300) as usize;
    let warmup = ctx.opt("warmup", 10) as usize;
    assert!(sc.orders >= 20 && sc.customers <= 99_999 && sc.items < 1_000_000 && sc.warehouses <= 99);
    // Every customer holds an order from the load, so `order-status` runs the
    // same statements on every engine whatever the mix placed before it.
    assert!(sc.orders >= sc.customers, "orders ({}) below customers ({}): a customer with no order", sc.orders, sc.customers);
    ctx.set_meta("scale", serde_json::json!({
        "warehouses": sc.warehouses, "districts_per_warehouse": DISTRICTS, "customers_per_district": sc.customers,
        "items": sc.items, "initial_orders_per_district": sc.orders, "terminals": terminals, "seconds": seconds,
        "alone_ops": alone, "warmup_per_kind": warmup,
    }));

    let mut e = ctx.connect();
    let mut rng = Rng::new(ctx.seed);
    create(e.as_mut());
    let t0 = Instant::now();
    load_all(e.as_mut(), &sc, &mut rng);
    analyze(
        e.as_mut(),
        &["s5_warehouses", "s5_districts", "s5_customers", "s5_items", "s5_stock", "s5_orders", "s5_order_lines",
            "s5_new_orders", "s5_history"],
    );
    ctx.set_meta("load_seconds", t0.elapsed().as_secs_f64());
    drop(e);

    let mut engines: Vec<Box<dyn Engine>> = (0..terminals).map(|_| ctx.connect()).collect();
    let stmts: Vec<St> = engines.iter_mut().map(|e| prepare(e.as_mut())).collect();
    let counts = [AtomicI64::new(0), AtomicI64::new(0), AtomicI64::new(0)];
    let seed = ctx.seed;
    let dims = (sc.warehouses, sc.customers, sc.items);
    let nw = sc.warehouses;
    let mk_gen = move |t: usize, salt: u64| Gen {
        rng: Rng::new(seed * 1000 + salt * 100 + t as u64 + 1),
        home: 1 + (t as i64 % nw),
        sc: dims,
        hist: salt as i64 * 100_000_000,
    };

    // Warm-up: every transaction kind `warmup` times on every terminal,
    // unrecorded - past PostgreSQL's five custom plans of each statement.
    std::thread::scope(|s| {
        for (t, e) in engines.iter_mut().enumerate() {
            let st = stmts[t];
            let counts = &counts;
            let mut g = mk_gen(t, 9);
            s.spawn(move || {
                let mut r = Recorder::new();
                let mut rows = Rows::new();
                for k in 0..5 * warmup {
                    let tx = [Tx::NewOrder, Tx::Payment, Tx::OrderStatus, Tx::Delivery, Tx::StockLevel][k % 5];
                    run_one(tx, e.as_mut(), &st, &mut g, t, &mut r, &mut rows, counts);
                }
            });
        }
    });
    for c in &counts {
        c.store(0, Ordering::Relaxed);
    }

    // Each transaction alone first, on one terminal, so every engine runs
    // them on the state the load left: after the mix a faster engine's
    // tables would be the larger.
    for (salt, tx) in [Tx::NewOrder, Tx::Payment, Tx::OrderStatus, Tx::Delivery, Tx::StockLevel].into_iter().enumerate() {
        ctx.phase(tx.name(), &mut engines[..1], |_, e, r| {
            let st = stmts[0];
            let mut g = mk_gen(0, 2 + salt as u64);
            let mut rows = Rows::new();
            for _ in 0..alone {
                run_one(tx, e, &st, &mut g, 0, r, &mut rows, &counts);
            }
        });
    }
    for c in &counts {
        c.store(0, Ordering::Relaxed);
    }

    // The mix's clock starts at its first operation, inside the phase, so
    // the profiler's snapshot and the session lookups are not in `seconds`.
    let start = std::sync::OnceLock::new();
    let mix = ctx.phase("mix", &mut engines, |t, e, r| {
        let st = stmts[t];
        let mut g = mk_gen(t, 1);
        let mut rows = Rows::new();
        let deadline = *start.get_or_init(Instant::now) + std::time::Duration::from_secs_f64(seconds);
        while Instant::now() < deadline {
            let tx = Tx::pick(&mut g.rng);
            run_one(tx, e, &st, &mut g, t, r, &mut rows, &counts);
        }
    });
    let new_orders = counts[0].load(Ordering::Relaxed);
    ctx.set_meta(
        "mix",
        serde_json::json!({
            "new_orders_committed": new_orders,
            "new_orders_per_minute": new_orders as f64 * 60.0 / mix.wall_s,
            "new_order_business_rollbacks": counts[1].load(Ordering::Relaxed),
            "transactions_failed": counts[2].load(Ordering::Relaxed),
            "note": "not tpmC: not TPC-C, not audited",
        }),
    );
    checks(ctx, engines[0].as_mut(), &sc);
}

fn scalar(e: &mut dyn Engine, sql: &str, params: &[Val]) -> i64 {
    let s = e.prepare(sql).unwrap_or_else(|err| panic!("{err}"));
    let mut rows = Rows::new();
    e.query(s, params, &mut rows).unwrap_or_else(|err| panic!("{err}"));
    match &rows[0][0] {
        Val::Null => 0,
        v => v.i(),
    }
}

fn checks(ctx: &mut Ctx, e: &mut dyn Engine, sc: &Scale) {
    let mut rows = Rows::new();
    // 1: a warehouse's year to date is its districts' sum.
    let mut bad = Vec::new();
    for w in 1..=sc.warehouses {
        let wy = scalar(e, "SELECT w_ytd FROM s5_warehouses WHERE w_id = ?", &[w.into()]);
        let dy = scalar(e, "SELECT SUM(d_ytd) FROM s5_districts WHERE d_w_id = ?", &[w.into()]);
        if wy != dy {
            bad.push(format!("w{w}: {wy} vs {dy}"));
        }
    }
    ctx.check("warehouse-ytd", bad.is_empty(), bad.join("; "));
    // 2: next order id is one above the highest order and the highest new order.
    let mut bad = Vec::new();
    let next_q = e.prepare("SELECT d_next_o_id FROM s5_districts WHERE d_key = ?").unwrap();
    let maxo_q = e.prepare("SELECT MAX(o_id) FROM s5_orders WHERE o_key >= ? AND o_key <= ?").unwrap();
    let maxno_q = e.prepare("SELECT MAX(no_o_id) FROM s5_new_orders WHERE no_key >= ? AND no_key <= ?").unwrap();
    for w in 1..=sc.warehouses {
        for d in 1..=DISTRICTS {
            let dk = d_key(w, d);
            let range = [o_key(dk, 0).into(), o_key(dk, 9_999_999).into()];
            e.query(next_q, &[dk.into()], &mut rows).unwrap();
            let next = rows[0][0].i();
            e.query(maxo_q, &range, &mut rows).unwrap();
            let maxo = rows[0][0].i();
            e.query(maxno_q, &range, &mut rows).unwrap();
            let maxno = match &rows[0][0] {
                Val::Null => maxo,
                v => v.i(),
            };
            if next - 1 != maxo || maxno != maxo {
                bad.push(format!("{dk}: next {next}, max order {maxo}, max new order {maxno}"));
            }
        }
    }
    ctx.check("district-next-order", bad.is_empty(), bad.join("; "));
    // 3: every order's line count is its lines.
    let lines = scalar(e, "SELECT COUNT(*) FROM s5_order_lines", &[]);
    let declared = scalar(e, "SELECT SUM(o_ol_cnt) FROM s5_orders", &[]);
    ctx.check("order-line-count", lines == declared, format!("{lines} lines, {declared} declared"));
    // 4: the stock's year to date is the quantity ordered since the load.
    let ytd = scalar(e, "SELECT SUM(s_ytd) FROM s5_stock", &[]);
    let ordered = scalar(e, "SELECT SUM(ol_quantity) FROM s5_order_lines WHERE ol_o_id > ?", &[sc.orders.into()]);
    let cnt = scalar(e, "SELECT SUM(s_order_cnt) FROM s5_stock", &[]);
    let lines_since = scalar(e, "SELECT COUNT(*) FROM s5_order_lines WHERE ol_o_id > ?", &[sc.orders.into()]);
    ctx.check("stock-ytd", ytd == ordered && cnt == lines_since, format!("ytd {ytd} vs ordered {ordered}; count {cnt} vs lines {lines_since}"));
    // 5: balance = delivered - paid, summed over customers.
    let balance = scalar(e, "SELECT SUM(c_balance) FROM s5_customers", &[]);
    let delivered = scalar(e, "SELECT SUM(ol_amount) FROM s5_order_lines WHERE ol_delivery_d > ?", &[0i64.into()]);
    let paid = scalar(e, "SELECT SUM(h_amount) FROM s5_history", &[]);
    ctx.check("customer-balance", balance == delivered - paid, format!("{balance} vs {delivered} - {paid}"));
}
