//! Oracle through the `oracle` crate (ODPI-C over the Oracle client
//! libraries): one `Statement` per scenario statement, built once and
//! executed with bound values - OCI's own statement handle, so no execution
//! parses again. (The server's hard parse is at a statement's first execute,
//! so a scenario's untimed warm-up is what keeps it out of a phase.)
//! Autocommit is on outside an explicit transaction, which makes OCI commit
//! within the execute's own round trip; `begin` turns it off with no round
//! trip, since an Oracle transaction opens at its first statement.
//!
//! **Fetch sizing** (EO-Q4): the fetch array and the prefetch are both
//! `FETCH_ROWS`, so a result up to that many rows comes back in the
//! execute's round trip, as KWP's 64 KiB batch and PostgreSQL's stream do.
//!
//! **Binds** (`OraP`): a string binds as `VARCHAR2(4000)`. The crate's own
//! `&str` binds `NVARCHAR2(len)`, which makes the server convert the column
//! (`SYS_OP_C2C`) and gives up an index on it - a full scan where the other
//! engines probe (the S1 review's finding 1, measured on `eq-ora`). One
//! constant size keeps one child cursor per statement.

use crate::engine::*;
use oracle::sql_type::{OracleType, ToSql};
use oracle::{Connection, Row, RowValue, SqlValue, Statement};
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

/// One bound value: a NUMBER, a VARCHAR2 or a NULL.
struct OraP<'a>(&'a Val);

impl ToSql for OraP<'_> {
    fn oratype(&self, _: &Connection) -> oracle::Result<OracleType> {
        Ok(match self.0 {
            Val::Str(_) => OracleType::Varchar2(4000),
            _ => OracleType::Number(0, 0),
        })
    }
    fn to_sql(&self, v: &mut SqlValue) -> oracle::Result<()> {
        match self.0 {
            Val::I64(i) => i.to_sql(v),
            Val::Str(s) => s.as_str().to_sql(v),
            Val::Null => v.set_null(),
        }
    }
}

/// A row decoded from the fetch buffers it borrows: a NUMBER that holds an
/// integer is `I64`, anything else its text. An unscaled NUMBER (an
/// aggregate's) is an integer when it holds one, as PostgreSQL's NUMERIC is.
struct Decoded(Vec<Val>);

impl RowValue for Decoded {
    fn get(row: &Row) -> oracle::Result<Decoded> {
        let vals = row.sql_values();
        let mut r = Vec::with_capacity(vals.len());
        for v in vals {
            if v.is_null()? {
                r.push(Val::Null);
                continue;
            }
            let numeric = matches!(v.oracle_type()?, OracleType::Number(_, _) | OracleType::Int64);
            r.push(match numeric.then(|| v.get::<i64>()) {
                Some(Ok(i)) => Val::I64(i),
                _ => Val::Str(v.get::<String>()?),
            });
        }
        Ok(Decoded(r))
    }
}

impl Ora {
    pub fn connect(user: &str, password: &str, connect: &str) -> DbResult<Ora> {
        let mut conn = Connection::connect(user, password, connect).map_err(err_of)?;
        conn.set_autocommit(true);
        Ok(Ora { conn, stmts: Vec::new(), decode_ns: 0 })
    }
}

fn ctx(sql: &str) -> impl FnOnce(oracle::Error) -> DbError + '_ {
    move |e| {
        let mut d = err_of(e);
        d.msg = format!("{} [{sql}]", d.msg);
        d
    }
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
            .map_err(ctx(&text))?;
        self.stmts.push(s);
        Ok(Stmt(self.stmts.len() - 1))
    }
    fn query(&mut self, s: Stmt, params: &[Val], out: &mut Rows) -> DbResult<()> {
        out.clear();
        let ps: Vec<OraP> = params.iter().map(OraP).collect();
        let refs: Vec<&dyn ToSql> = ps.iter().map(|p| p as &dyn ToSql).collect();
        let rs = self.stmts[s.0].query_as::<Decoded>(&refs).map_err(err_of)?;
        // The rows are in the prefetch buffers once the execute returns
        // (up to FETCH_ROWS), so the iteration is the decode.
        let t0 = Instant::now();
        for row in rs {
            out.push(row.map_err(err_of)?.0);
        }
        self.decode_ns += t0.elapsed().as_nanos() as u64;
        Ok(())
    }
    fn exec(&mut self, s: Stmt, params: &[Val]) -> DbResult<u64> {
        let ps: Vec<OraP> = params.iter().map(OraP).collect();
        let refs: Vec<&dyn ToSql> = ps.iter().map(|p| p as &dyn ToSql).collect();
        let stmt = &mut self.stmts[s.0];
        stmt.execute(&refs).map_err(err_of)?;
        stmt.row_count().map_err(err_of)
    }
    fn run(&mut self, sql: &str) -> DbResult<()> {
        self.conn.execute(sql, &[]).map(|_| ()).map_err(ctx(sql))
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
