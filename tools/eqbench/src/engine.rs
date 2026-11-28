//! The one interface every scenario drives, and the three things it is
//! implemented over (EO-Q4, `instructions/v3.2.0/workorder-eo-three-engine-equal-client.md`).
//!
//! **What "equal client" means here.** A scenario prepares each statement
//! once per connection and executes it with bound values; every returned
//! value is decoded into a [`Val`]; autocommit is the default and
//! `begin`/`commit` bracket an explicit transaction. The decode is timed
//! apart from the round trip (`decode_ns`), so a phase can say what the
//! client spent turning bytes into values on each engine.
//!
//! Statements are written once, with `?` placeholders; each engine rewrites
//! them to its own spelling (`$n`, `:n`) outside string literals. Anything a
//! dialect spells differently - a `LIMIT`, a type name, a table kind - is the
//! scenario's to choose through [`Kind`].

use std::fmt;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Kds,
    Pg,
    Ora,
}

impl Kind {
    pub fn name(self) -> &'static str {
        match self {
            Kind::Kds => "kds",
            Kind::Pg => "pg",
            Kind::Ora => "oracle",
        }
    }
    pub fn parse(s: &str) -> Option<Kind> {
        match s {
            "kds" => Some(Kind::Kds),
            "pg" | "postgres" => Some(Kind::Pg),
            "ora" | "oracle" => Some(Kind::Ora),
            _ => None,
        }
    }
}

/// One decoded value. Integers of every width and booleans are `I64`; text
/// is `Str`. **A scenario's columns are integers and strings only** (`Ty`):
/// money in minor units, days and instants as integers. A DECIMAL, DATE or
/// TIMESTAMP column would decode differently on the three engines (KDS's
/// unscaled integer, PostgreSQL's text at its scale, Oracle's number), so
/// none is declared; an aggregate over integers decodes to `I64` on all
/// three.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Val {
    Null,
    I64(i64),
    Str(String),
}

impl Val {
    pub fn i(&self) -> i64 {
        match self {
            Val::I64(v) => *v,
            Val::Str(s) => s.trim().parse().unwrap_or_else(|_| panic!("not an integer: {s:?}")),
            Val::Null => panic!("NULL where an integer was expected"),
        }
    }
    pub fn s(&self) -> &str {
        match self {
            Val::Str(s) => s,
            _ => panic!("not a string: {self:?}"),
        }
    }
}

impl From<i64> for Val {
    fn from(v: i64) -> Val {
        Val::I64(v)
    }
}
impl From<&str> for Val {
    fn from(v: &str) -> Val {
        Val::Str(v.to_string())
    }
}
impl From<String> for Val {
    fn from(v: String) -> Val {
        Val::Str(v)
    }
}

/// How a refusal is classed, so a scenario's retry loop reads one thing on
/// every engine.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ErrClass {
    /// A write conflict, serialization failure or deadlock victim: retry.
    Conflict,
    /// A unique key's duplicate.
    Unique,
    /// A declared invariant refused the write (foreign key, check,
    /// assertion, transition).
    Constraint,
    Other,
}

#[derive(Clone, Debug)]
pub struct DbError {
    pub class: ErrClass,
    pub msg: String,
}

impl fmt::Display for DbError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}: {}", self.class, self.msg)
    }
}

pub type DbResult<T> = Result<T, DbError>;

pub fn other(msg: impl Into<String>) -> DbError {
    DbError { class: ErrClass::Other, msg: msg.into() }
}

/// A statement prepared on one connection.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Stmt(pub usize);

pub type Rows = Vec<Vec<Val>>;

pub trait Engine: Send {
    fn kind(&self) -> Kind;
    /// Prepares `sql` (with `?` placeholders) once on this connection.
    fn prepare(&mut self, sql: &str) -> DbResult<Stmt>;
    /// Runs a prepared statement and decodes every row into `out`
    /// (cleared first).
    fn query(&mut self, s: Stmt, params: &[Val], out: &mut Rows) -> DbResult<()>;
    /// Runs a prepared statement that returns no rows; the rows affected.
    fn exec(&mut self, s: Stmt, params: &[Val]) -> DbResult<u64>;
    /// Runs text with no parameters, unprepared: DDL and setup only.
    fn run(&mut self, sql: &str) -> DbResult<()>;
    fn begin(&mut self) -> DbResult<()>;
    fn commit(&mut self) -> DbResult<()>;
    fn rollback(&mut self) -> DbResult<()>;
    /// Nanoseconds spent turning result bytes into values, cumulative.
    fn decode_ns(&self) -> u64;
    /// The engine's own name for this session, for the profiler: KDS's
    /// `session_id`, PostgreSQL's backend pid, Oracle's SID.
    fn session_key(&mut self) -> DbResult<i64>;
}

/// `?` -> `$1`, `$2`, ... or `:1`, `:2`, ..., outside single-quoted literals.
pub fn rewrite_placeholders(sql: &str, prefix: char) -> String {
    let mut out = String::with_capacity(sql.len() + 8);
    let mut n = 0;
    let mut in_str = false;
    for c in sql.chars() {
        if c == '\'' {
            in_str = !in_str;
            out.push(c);
        } else if c == '?' && !in_str {
            n += 1;
            out.push(prefix);
            out.push_str(&n.to_string());
        } else {
            out.push(c);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn placeholders() {
        assert_eq!(rewrite_placeholders("a = ? AND b = '?' AND c = ?", '$'), "a = $1 AND b = '?' AND c = $2");
    }
}
