//! PostgreSQL through the `postgres` crate: the extended protocol, one
//! prepared statement per scenario statement, values bound in binary and
//! every result value decoded from its binary form by `AnyVal`, so the
//! decode the harness times is this module's code as KWP's is `kwp.rs`'s.

use crate::engine::*;
use postgres::types::{FromSql, ToSql, Type};
use postgres::{Client, NoTls, Statement};
use std::error::Error as StdError;
use std::time::Instant;

pub struct Pg {
    client: Client,
    stmts: Vec<Statement>,
    decode_ns: u64,
}

/// Any column, decoded from PostgreSQL's binary format.
struct AnyVal(Val);

fn be_i64(raw: &[u8]) -> i64 {
    match raw.len() {
        1 => raw[0] as i8 as i64,
        2 => i16::from_be_bytes([raw[0], raw[1]]) as i64,
        4 => i32::from_be_bytes(raw.try_into().unwrap()) as i64,
        8 => i64::from_be_bytes(raw.try_into().unwrap()),
        _ => 0,
    }
}

/// NUMERIC's binary form: ndigits, weight, sign, dscale, then base-10000
/// digits. An integer value decodes to `I64`, any other to its text.
fn numeric(raw: &[u8]) -> Val {
    let rd = |i: usize| i16::from_be_bytes([raw[i], raw[i + 1]]);
    let ndigits = rd(0) as usize;
    let weight = rd(2) as i32;
    let sign = u16::from_be_bytes([raw[4], raw[5]]);
    let dscale = rd(6) as usize;
    if sign == 0xC000 {
        return Val::Str("NaN".into());
    }
    let digits: Vec<i32> = (0..ndigits).map(|k| rd(8 + 2 * k) as i32).collect();
    if dscale == 0 {
        let mut v: i128 = 0;
        for (k, d) in digits.iter().enumerate() {
            let _ = k;
            v = v * 10000 + *d as i128;
        }
        let shift = weight + 1 - ndigits as i32;
        for _ in 0..shift.max(0) {
            v *= 10000;
        }
        let v = if sign == 0x4000 { -v } else { v };
        return match i64::try_from(v) {
            Ok(x) => Val::I64(x),
            Err(_) => Val::Str(v.to_string()),
        };
    }
    // Non-integer: integer part, then dscale fractional digits.
    let mut int_part = String::new();
    for w in 0..=weight {
        let d = digits.get(w as usize).copied().unwrap_or(0);
        if int_part.is_empty() {
            int_part = d.to_string();
        } else {
            int_part.push_str(&format!("{d:04}"));
        }
    }
    if int_part.is_empty() {
        int_part.push('0');
    }
    let mut frac = String::new();
    let mut k = (weight + 1).max(0) as i32;
    let mut lead = weight + 1;
    while lead < 0 {
        frac.push_str("0000");
        lead += 1;
    }
    while frac.len() < dscale {
        let d = digits.get(k as usize).copied().unwrap_or(0);
        frac.push_str(&format!("{d:04}"));
        k += 1;
    }
    frac.truncate(dscale);
    let s = format!("{}{}.{}", if sign == 0x4000 { "-" } else { "" }, int_part, frac);
    Val::Str(s)
}

impl<'a> FromSql<'a> for AnyVal {
    fn from_sql(ty: &Type, raw: &'a [u8]) -> Result<Self, Box<dyn StdError + Sync + Send>> {
        let v = match *ty {
            Type::INT2 | Type::INT4 | Type::INT8 | Type::OID => Val::I64(be_i64(raw)),
            Type::BOOL => Val::I64(raw[0] as i64),
            // days and microseconds since 2000-01-01, moved to the Unix
            // epoch as KWP's DATE and TIMESTAMP are.
            Type::DATE => Val::I64(be_i64(raw) + 10_957),
            Type::TIMESTAMP | Type::TIMESTAMPTZ => Val::I64(be_i64(raw) + 946_684_800_000_000),
            Type::NUMERIC => numeric(raw),
            _ => Val::Str(String::from_utf8_lossy(raw).into_owned()),
        };
        Ok(AnyVal(v))
    }
    fn from_sql_null(_: &Type) -> Result<Self, Box<dyn StdError + Sync + Send>> {
        Ok(AnyVal(Val::Null))
    }
    fn accepts(_: &Type) -> bool {
        true
    }
}

fn err_of(e: postgres::Error) -> DbError {
    let class = match e.code().map(|c| c.code()) {
        Some("40001") | Some("40P01") => ErrClass::Conflict,
        Some("23505") => ErrClass::Unique,
        Some(c) if c.starts_with("23") => ErrClass::Constraint,
        _ => ErrClass::Other,
    };
    let msg = match e.as_db_error() {
        Some(d) => format!("PG {}: {}", d.code().code(), d.message()),
        None => format!("PG: {e}"),
    };
    DbError { class, msg }
}

impl Pg {
    pub fn connect(params: &str) -> DbResult<Pg> {
        let client = Client::connect(params, NoTls).map_err(err_of)?;
        Ok(Pg { client, stmts: Vec::new(), decode_ns: 0 })
    }

    /// Each value as the type the server inferred for its parameter.
    fn bind<'b>(stmt: &Statement, params: &'b [Val]) -> Vec<Box<dyn ToSql + Sync + 'b>> {
        let mut hold: Vec<Box<dyn ToSql + Sync + 'b>> = Vec::with_capacity(params.len());
        for (p, ty) in params.iter().zip(stmt.params()) {
            let b: Box<dyn ToSql + Sync + 'b> = match (p, ty) {
                (Val::I64(v), &Type::INT2) => Box::new(*v as i16),
                (Val::I64(v), &Type::INT4) => Box::new(*v as i32),
                (Val::I64(v), &Type::BOOL) => Box::new(*v != 0),
                (Val::I64(v), &Type::TEXT) | (Val::I64(v), &Type::VARCHAR) => Box::new(v.to_string()),
                (Val::I64(v), _) => Box::new(*v),
                (Val::Str(s), _) => Box::new(s.as_str()),
                (Val::Null, _) => Box::new(Option::<i64>::None),
            };
            hold.push(b);
        }
        hold
    }
}

impl Engine for Pg {
    fn kind(&self) -> Kind {
        Kind::Pg
    }
    fn prepare(&mut self, sql: &str) -> DbResult<Stmt> {
        let text = rewrite_placeholders(sql, '$');
        let s = self.client.prepare(&text).map_err(|e| {
            let mut d = err_of(e);
            d.msg = format!("{} [prepare: {text}]", d.msg);
            d
        })?;
        self.stmts.push(s);
        Ok(Stmt(self.stmts.len() - 1))
    }
    fn query(&mut self, s: Stmt, params: &[Val], out: &mut Rows) -> DbResult<()> {
        out.clear();
        let stmt = &self.stmts[s.0];
        let hold = Pg::bind(stmt, params);
        let refs: Vec<&(dyn ToSql + Sync)> = hold.iter().map(|b| b.as_ref() as &(dyn ToSql + Sync)).collect();
        let rows = self.client.query(stmt, &refs).map_err(err_of)?;
        let t0 = Instant::now();
        for row in &rows {
            let mut r = Vec::with_capacity(row.len());
            for i in 0..row.len() {
                let v: AnyVal = row.try_get(i).map_err(err_of)?;
                r.push(v.0);
            }
            out.push(r);
        }
        self.decode_ns += t0.elapsed().as_nanos() as u64;
        Ok(())
    }
    fn exec(&mut self, s: Stmt, params: &[Val]) -> DbResult<u64> {
        let stmt = &self.stmts[s.0];
        let hold = Pg::bind(stmt, params);
        let refs: Vec<&(dyn ToSql + Sync)> = hold.iter().map(|b| b.as_ref() as &(dyn ToSql + Sync)).collect();
        self.client.execute(stmt, &refs).map_err(err_of)
    }
    fn run(&mut self, sql: &str) -> DbResult<()> {
        self.client.batch_execute(sql).map_err(|e| {
            let mut d = err_of(e);
            d.msg = format!("{} [{sql}]", d.msg);
            d
        })
    }
    fn begin(&mut self) -> DbResult<()> {
        self.client.batch_execute("BEGIN").map_err(err_of)
    }
    fn commit(&mut self) -> DbResult<()> {
        self.client.batch_execute("COMMIT").map_err(err_of)
    }
    fn rollback(&mut self) -> DbResult<()> {
        self.client.batch_execute("ROLLBACK").map_err(err_of)
    }
    fn decode_ns(&self) -> u64 {
        self.decode_ns
    }
    fn session_key(&mut self) -> DbResult<i64> {
        let r = self.client.query_one("SELECT pg_backend_pid()", &[]).map_err(err_of)?;
        Ok(r.get::<_, i32>(0) as i64)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn numeric_decodes() {
        // 12345678 = digits [1234, 5678], weight 1
        let raw = [0, 2, 0, 1, 0, 0, 0, 0, 0x04, 0xD2, 0x16, 0x2E];
        assert_eq!(numeric(&raw), Val::I64(12345678));
        // 10000 = [1], weight 1
        let raw = [0, 1, 0, 1, 0, 0, 0, 0, 0, 1];
        assert_eq!(numeric(&raw), Val::I64(10000));
        // 1.5 dscale 1: digits [1, 5000], weight 0
        let raw = [0, 2, 0, 0, 0, 0, 0, 1, 0, 1, 0x13, 0x88];
        assert_eq!(numeric(&raw), Val::Str("1.5".into()));
    }
}
