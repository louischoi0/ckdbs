//! Oracle through the `oracle` crate (ODPI-C over the Oracle client
//! libraries): one `Statement` per scenario statement, built once and
//! executed with bound values - OCI's own statement handle, so no execution
//! parses again. Autocommit is on outside an explicit transaction, which
//! makes OCI commit within the execute's own round trip; `begin` turns it
//! off with no round trip, since an Oracle transaction opens at its first
//! statement.
//!
//! **Fetch sizing** (EN-Q4): the fetch array and the prefetch are both
//! `FETCH_ROWS`, so a result up to that many rows comes back in the
//! execute's round trip, as KWP's 64 KiB batch and PostgreSQL's stream do.

use crate::engine::*;
use oracle::sql_type::{OracleType, ToSql};
use oracle::{Connection, Statement};
use std::time::Instant;

pub const FETCH_ROWS: u32 = 10_000;

pub struct Ora {
    conn: Connection,
    stmts: Vec<Statement>,
    decode_ns: u64,
}

fn err_of(e: oracle::Error) -> DbError {
    let code = e.db_error().map(|d| d.code());
    let class = match code {
        Some(60) | Some(8177) => ErrClass::Conflict,
        Some(1) => ErrClass::Unique,
        Some(2291) | Some(2292) | Some(2290) | Some(1400) => ErrClass::Constraint,
        _ => ErrClass::Other,
    };
    DbError { class, msg: format!("ORA: {e}") }
}

impl Ora {
    pub fn connect(user: &str, password: &str, connect: &str) -> DbResult<Ora> {
        let mut conn = Connection::connect(user, password, connect).map_err(err_of)?;
        conn.set_autocommit(true);
        Ok(Ora { conn, stmts: Vec::new(), decode_ns: 0 })
    }
}

fn to_sql(params: &[Val]) -> Vec<Box<dyn ToSql>> {
    params
        .iter()
        .map(|p| -> Box<dyn ToSql> {
            match p {
                Val::I64(v) => Box::new(*v),
                Val::Str(s) => Box::new(s.clone()),
                Val::Null => Box::new(Option::<i64>::None),
            }
        })
        .collect()
}

impl Engine for Ora {
    fn kind(&self) -> Kind {
        Kind::Ora
    }
    fn prepare(&mut self, sql: &str) -> DbResult<Stmt> {
        let text = rewrite_placeholders(sql, ':');
        let s = self
            .conn
            .statement(&text)
            .fetch_array_size(FETCH_ROWS)
            .prefetch_rows(FETCH_ROWS)
            .build()
            .map_err(|e| {
                let mut d = err_of(e);
                d.msg = format!("{} [prepare: {text}]", d.msg);
                d
            })?;
        self.stmts.push(s);
        Ok(Stmt(self.stmts.len() - 1))
    }
    fn query(&mut self, s: Stmt, params: &[Val], out: &mut Rows) -> DbResult<()> {
        out.clear();
        let hold = to_sql(params);
        let refs: Vec<&dyn ToSql> = hold.iter().map(|b| b.as_ref()).collect();
        let stmt = &mut self.stmts[s.0];
        let rs = stmt.query(&refs).map_err(err_of)?;
        let mut ints: Option<Vec<bool>> = None;
        let mut dec = 0u64;
        for row in rs {
            let row = row.map_err(err_of)?;
            let t0 = Instant::now();
            let vals = row.sql_values();
            let ints = ints.get_or_insert_with(|| {
                vals.iter()
                    .map(|v| matches!(v.oracle_type(), Ok(OracleType::Number(_, 0)) | Ok(OracleType::Int64)))
                    .collect()
            });
            let mut r = Vec::with_capacity(vals.len());
            for (v, &is_int) in vals.iter().zip(ints.iter()) {
                if v.is_null().map_err(err_of)? {
                    r.push(Val::Null);
                } else if is_int {
                    r.push(Val::I64(v.get::<i64>().map_err(err_of)?));
                } else {
                    // An unscaled NUMBER (an aggregate's) is an integer when
                    // it holds one; text otherwise, as PostgreSQL's NUMERIC.
                    match v.get::<i64>() {
                        Ok(i) => r.push(Val::I64(i)),
                        Err(_) => r.push(Val::Str(v.get::<String>().map_err(err_of)?)),
                    }
                }
            }
            out.push(r);
            dec += t0.elapsed().as_nanos() as u64;
        }
        self.decode_ns += dec;
        Ok(())
    }
    fn exec(&mut self, s: Stmt, params: &[Val]) -> DbResult<u64> {
        let hold = to_sql(params);
        let refs: Vec<&dyn ToSql> = hold.iter().map(|b| b.as_ref()).collect();
        let stmt = &mut self.stmts[s.0];
        stmt.execute(&refs).map_err(err_of)?;
        stmt.row_count().map_err(err_of)
    }
    fn run(&mut self, sql: &str) -> DbResult<()> {
        self.conn.execute(sql, &[]).map(|_| ()).map_err(|e| {
            let mut d = err_of(e);
            d.msg = format!("{} [{sql}]", d.msg);
            d
        })
    }
    fn begin(&mut self) -> DbResult<()> {
        self.conn.set_autocommit(false);
        Ok(())
    }
    fn commit(&mut self) -> DbResult<()> {
        let r = self.conn.commit().map_err(err_of);
        self.conn.set_autocommit(true);
        r
    }
    fn rollback(&mut self) -> DbResult<()> {
        let r = self.conn.rollback().map_err(err_of);
        self.conn.set_autocommit(true);
        r
    }
    fn decode_ns(&self) -> u64 {
        self.decode_ns
    }
    fn session_key(&mut self) -> DbResult<i64> {
        let row = self.conn.query_row("SELECT SYS_CONTEXT('USERENV','SID') FROM dual", &[]).map_err(err_of)?;
        let s: String = row.get(0).map_err(err_of)?;
        s.trim().parse().map_err(|_| other("SID not a number"))
    }
}
