//! KWP/1 over one TCP connection, written for the harness after the
//! reference client `tools/kwp.py` and `docs/spec/protocol.md` §2-§9.
//!
//! Only what a scenario needs: the handshake with no authentication, a
//! named statement parsed once (`C_PARSE` + `C_SYNC`, so EA's kept parse
//! serves every execution), and one write per execution - `C_BIND`,
//! `C_EXECUTE`, `C_CLOSE`, `C_SYNC` - read to `S_READY`. Transactions are
//! `C_TXN_BEGIN` / `C_TXN_COMMIT` / `C_TXN_ABORT` frames.
//!
//! A bind never carries a NULL: a NULL bind keeps no parse (`protocol.md`
//! §5), so a scenario writes a NULL as SQL text instead.

use crate::engine::*;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Instant;

const MAGIC: u32 = 0x3150_574B; // 'KWP1' little-endian
const VERSION: u16 = 1;

const C_HELLO: u8 = 1;
const C_PARSE: u8 = 2;
const C_BIND: u8 = 3;
const C_EXECUTE: u8 = 4;
const C_CLOSE: u8 = 7;
const C_SYNC: u8 = 8;
const C_TXN_BEGIN: u8 = 9;
const C_TXN_COMMIT: u8 = 10;
const C_TXN_ABORT: u8 = 11;
const C_TERMINATE: u8 = 14;

const S_HELLO: u8 = 1;
const S_READY: u8 = 2;
const S_PARSE_OK: u8 = 3;
const S_BIND_OK: u8 = 4;
const S_ROW_DESC: u8 = 5;
const S_ROW_BATCH: u8 = 6;
const S_PORTAL_SUSPENDED: u8 = 7;
const S_COMPLETE: u8 = 8;
const S_TXN_OK: u8 = 9;
const S_ERROR: u8 = 10;
const S_NOTICE: u8 = 12;

const T_INT8: u32 = 1;
const T_INT16: u32 = 2;
const T_INT32: u32 = 3;
const T_INT64: u32 = 4;
const T_UINT64: u32 = 5;
const T_DECIMAL: u32 = 7;
const T_BOOL: u32 = 8;
const T_VARCHAR: u32 = 9;
const T_CHAR: u32 = 10;
const T_DATE: u32 = 11;
const T_TIMESTAMP: u32 = 12;
const T_DECIMAL_WIDE: u32 = 13;

// Error categories (include/kds/wire/kwp.hpp `ErrorCategory`).
const CAT_ALREADY_EXISTS: u32 = 2;
const CAT_TXN_CONFLICT: u32 = 10;
const CAT_FK_VIOLATION: u32 = 16;
const CAT_ASSERTION_VIOLATION: u32 = 17;
const CAT_TRANSITION_VIOLATION: u32 = 18;

const PORTAL: &str = "q";

/// Durability rung for `C_TXN_BEGIN`: 0 is the session's default.
pub struct Kwp {
    sock: TcpStream,
    buf: Vec<u8>,
    start: usize,
    end: usize,
    out: Vec<u8>,
    stmts: Vec<String>,
    session_id: u64,
    sys_session: i64,
    durability: u8,
    decode_ns: u64,
    field_types: Vec<u32>,
}

fn put_u16(b: &mut Vec<u8>, v: u16) {
    b.extend_from_slice(&v.to_le_bytes());
}
fn put_u32(b: &mut Vec<u8>, v: u32) {
    b.extend_from_slice(&v.to_le_bytes());
}
fn put_str(b: &mut Vec<u8>, s: &str) {
    put_u16(b, s.len() as u16);
    b.extend_from_slice(s.as_bytes());
}
fn put_text(b: &mut Vec<u8>, s: &str) {
    put_u32(b, s.len() as u32);
    b.extend_from_slice(s.as_bytes());
}

/// Appends a frame whose payload `body` writes.
fn frame(out: &mut Vec<u8>, ftype: u8, body: impl FnOnce(&mut Vec<u8>)) {
    let at = out.len();
    out.extend_from_slice(&[0, 0, 0, 0, ftype, 0, 0, 0]);
    body(out);
    let len = (out.len() - at - 4) as u32;
    out[at..at + 4].copy_from_slice(&len.to_le_bytes());
}

struct Rd<'a> {
    d: &'a [u8],
    at: usize,
}
impl<'a> Rd<'a> {
    fn take(&mut self, n: usize) -> DbResult<&'a [u8]> {
        if self.at + n > self.d.len() {
            return Err(other("truncated KWP payload"));
        }
        let s = &self.d[self.at..self.at + n];
        self.at += n;
        Ok(s)
    }
    fn u8(&mut self) -> DbResult<u8> {
        Ok(self.take(1)?[0])
    }
    fn u16(&mut self) -> DbResult<u16> {
        Ok(u16::from_le_bytes(self.take(2)?.try_into().unwrap()))
    }
    fn u32(&mut self) -> DbResult<u32> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    fn u64(&mut self) -> DbResult<u64> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    fn s(&mut self) -> DbResult<&'a [u8]> {
        let n = self.u16()? as usize;
        self.take(n)
    }
    fn text(&mut self) -> DbResult<Option<String>> {
        let n = self.u32()?;
        if n == u32::MAX {
            return Ok(None);
        }
        Ok(Some(String::from_utf8_lossy(self.take(n as usize)?).into_owned()))
    }
}

fn error_of(payload: &[u8]) -> DbError {
    let mut r = Rd { d: payload, at: 0 };
    let code = r.u32().unwrap_or(0);
    let _retryable = r.u8();
    let _severity = r.u8();
    let msg = r.text().ok().flatten().unwrap_or_default();
    let class = match code >> 16 {
        CAT_TXN_CONFLICT => ErrClass::Conflict,
        CAT_ALREADY_EXISTS => ErrClass::Unique,
        CAT_FK_VIOLATION | CAT_ASSERTION_VIOLATION | CAT_TRANSITION_VIOLATION => ErrClass::Constraint,
        _ => ErrClass::Other,
    };
    DbError { class, msg: format!("KDS {}: {msg}", code >> 16) }
}

fn int_le(raw: &[u8], signed: bool) -> i64 {
    let mut b = [0u8; 8];
    let n = raw.len().min(8);
    b[..n].copy_from_slice(&raw[..n]);
    if signed && n < 8 && n > 0 && raw[n - 1] & 0x80 != 0 {
        for x in b.iter_mut().skip(n) {
            *x = 0xFF;
        }
    }
    i64::from_le_bytes(b)
}

fn decode(oid: u32, raw: &[u8]) -> Val {
    match oid {
        T_INT8 | T_INT16 | T_INT32 | T_INT64 | T_DATE | T_TIMESTAMP | T_DECIMAL => Val::I64(int_le(raw, true)),
        T_UINT64 | T_BOOL => Val::I64(int_le(raw, false)),
        T_DECIMAL_WIDE => {
            let mut b = [0u8; 16];
            let n = raw.len().min(16);
            b[..n].copy_from_slice(&raw[..n]);
            Val::Str(i128::from_le_bytes(b).to_string())
        }
        T_VARCHAR | T_CHAR => Val::Str(String::from_utf8_lossy(raw).into_owned()),
        _ => Val::Str(String::from_utf8_lossy(raw).into_owned()),
    }
}

impl Kwp {
    pub fn connect(host: &str, port: u16, durability: u8) -> DbResult<Kwp> {
        let sock = TcpStream::connect((host, port)).map_err(|e| other(format!("connect {host}:{port}: {e}")))?;
        sock.set_nodelay(true).map_err(|e| other(e.to_string()))?;
        let mut k = Kwp {
            sock,
            buf: vec![0; 1 << 17],
            start: 0,
            end: 0,
            out: Vec::with_capacity(4096),
            stmts: Vec::new(),
            session_id: 0,
            sys_session: -1,
            durability,
            decode_ns: 0,
            field_types: Vec::new(),
        };
        k.out.clear();
        frame(&mut k.out, C_HELLO, |b| {
            put_u32(b, MAGIC);
            put_u16(b, VERSION);
            put_u16(b, VERSION);
            b.extend_from_slice(&0u64.to_le_bytes());
            b.push(0);
            put_str(b, "eqbench");
        });
        k.flush()?;
        let (t, p) = k.recv()?;
        if t == S_ERROR {
            return Err(error_of(&p));
        }
        if t != S_HELLO {
            return Err(other(format!("expected S_HELLO, got {t}")));
        }
        let mut r = Rd { d: &p, at: 0 };
        let _v = r.u16()?;
        let _caps = r.u64()?;
        k.session_id = r.u64()?;
        let (t, _) = k.recv()?;
        if t != S_READY {
            return Err(other(format!("expected S_READY, got {t}")));
        }
        Ok(k)
    }

    fn flush(&mut self) -> DbResult<()> {
        let r = self.sock.write_all(&self.out);
        self.out.clear();
        r.map_err(|e| other(format!("KWP write: {e}")))
    }

    /// The next frame's type, and its payload copied out. The payload copy
    /// is one memcpy of what the socket gave; `recv_into` avoids it on the
    /// row path.
    fn recv(&mut self) -> DbResult<(u8, Vec<u8>)> {
        let (t, a, b) = self.next_frame()?;
        Ok((t, self.buf[a..b].to_vec()))
    }

    /// Reads until one whole frame is buffered; its type and payload range.
    fn next_frame(&mut self) -> DbResult<(u8, usize, usize)> {
        loop {
            let have = self.end - self.start;
            if have >= 8 {
                let len = u32::from_le_bytes(self.buf[self.start..self.start + 4].try_into().unwrap()) as usize;
                if have >= 4 + len {
                    let t = self.buf[self.start + 4];
                    let a = self.start + 8;
                    let b = self.start + 4 + len;
                    self.start = b;
                    return Ok((t, a, b));
                }
                if 4 + len > self.buf.len() {
                    let mut nb = vec![0; (4 + len).next_power_of_two()];
                    nb[..have].copy_from_slice(&self.buf[self.start..self.end]);
                    self.buf = nb;
                    self.start = 0;
                    self.end = have;
                }
            }
            if self.start > 0 && (self.end == self.buf.len() || self.start == self.end) {
                self.buf.copy_within(self.start..self.end, 0);
                self.end -= self.start;
                self.start = 0;
            }
            let n = self.sock.read(&mut self.buf[self.end..]).map_err(|e| other(format!("KWP read: {e}")))?;
            if n == 0 {
                return Err(other("KDS closed the connection"));
            }
            self.end += n;
        }
    }

    fn bind_tail(&mut self, stmt: &str, params: &[Val]) {
        frame(&mut self.out, C_BIND, |b| {
            put_str(b, PORTAL);
            put_str(b, stmt);
            put_u16(b, params.len() as u16);
            for p in params {
                match p {
                    Val::I64(v) => {
                        put_u32(b, T_INT64);
                        put_u32(b, 0);
                        put_u32(b, 8);
                        b.extend_from_slice(&v.to_le_bytes());
                    }
                    Val::Str(s) => {
                        put_u32(b, T_VARCHAR);
                        put_u32(b, 0);
                        put_u32(b, s.len() as u32);
                        b.extend_from_slice(s.as_bytes());
                    }
                    Val::Null => panic!("a KWP bind never carries NULL (protocol.md §5): write it in the text"),
                }
            }
        });
        frame(&mut self.out, C_EXECUTE, |b| {
            put_str(b, PORTAL);
            put_u32(b, 0);
        });
        frame(&mut self.out, C_CLOSE, |b| {
            b.push(2);
            put_str(b, PORTAL);
        });
        frame(&mut self.out, C_SYNC, |_| {});
    }

    /// Reads one statement's answer to `S_READY`: rows into `out` when given,
    /// the rows affected, and the first refusal.
    fn read_answer(&mut self, mut out: Option<&mut Rows>) -> DbResult<u64> {
        let mut affected = 0;
        let mut err = None;
        loop {
            let (t, a, b) = self.next_frame()?;
            match t {
                S_PARSE_OK | S_BIND_OK | S_NOTICE => {}
                S_ROW_DESC => {
                    let t0 = Instant::now();
                    let mut r = Rd { d: &self.buf[a..b], at: 0 };
                    let n = r.u16()? as usize;
                    self.field_types.clear();
                    for _ in 0..n {
                        r.s()?;
                        let oid = r.u32()?;
                        r.take(2 + 2 + 4)?;
                        self.field_types.push(oid);
                    }
                    self.decode_ns += t0.elapsed().as_nanos() as u64;
                }
                S_ROW_BATCH => {
                    let t0 = Instant::now();
                    let d = &self.buf[a..b];
                    let count = u16::from_le_bytes([d[0], d[1]]) as usize;
                    let mut at = 2;
                    for _ in 0..count {
                        let mut row = Vec::with_capacity(self.field_types.len());
                        for &oid in &self.field_types {
                            if at + 4 > d.len() {
                                return Err(other("truncated row batch"));
                            }
                            let n = i32::from_le_bytes(d[at..at + 4].try_into().unwrap());
                            at += 4;
                            if n < 0 {
                                row.push(Val::Null);
                            } else {
                                let n = n as usize;
                                if at + n > d.len() {
                                    return Err(other("truncated row batch"));
                                }
                                row.push(decode(oid, &d[at..at + n]));
                                at += n;
                            }
                        }
                        if let Some(o) = out.as_deref_mut() {
                            o.push(row);
                        }
                    }
                    self.decode_ns += t0.elapsed().as_nanos() as u64;
                }
                S_PORTAL_SUSPENDED => return Err(other("portal suspended with max_rows 0")),
                S_COMPLETE => {
                    let mut r = Rd { d: &self.buf[a..b], at: 0 };
                    r.text()?;
                    affected = r.u64()?;
                }
                S_ERROR => {
                    if err.is_none() {
                        err = Some(error_of(&self.buf[a..b]));
                    }
                }
                S_READY => break,
                _ => return Err(other(format!("unexpected KWP frame {t}"))),
            }
        }
        match err {
            Some(e) => Err(e),
            None => Ok(affected),
        }
    }

    fn txn(&mut self, ftype: u8, durability: Option<u8>) -> DbResult<()> {
        frame(&mut self.out, ftype, |b| {
            if let Some(d) = durability {
                b.push(d);
            }
        });
        self.flush()?;
        let (t, p) = self.recv()?;
        if t == S_TXN_OK {
            return Ok(());
        }
        if t == S_ERROR {
            let e = error_of(&p);
            frame(&mut self.out, C_SYNC, |_| {});
            self.flush()?;
            loop {
                let (t, _) = self.recv()?;
                if t == S_READY {
                    break;
                }
            }
            return Err(e);
        }
        Err(other(format!("expected S_TXN_OK, got {t}")))
    }
}

impl Engine for Kwp {
    fn kind(&self) -> Kind {
        Kind::Kds
    }

    fn prepare(&mut self, sql: &str) -> DbResult<Stmt> {
        let name = format!("s{}", self.stmts.len());
        frame(&mut self.out, C_PARSE, |b| {
            put_str(b, &name);
            put_text(b, sql);
        });
        frame(&mut self.out, C_SYNC, |_| {});
        self.flush()?;
        self.read_answer(None).map_err(|e| DbError { class: e.class, msg: format!("{} [prepare: {sql}]", e.msg) })?;
        self.stmts.push(name);
        Ok(Stmt(self.stmts.len() - 1))
    }

    fn query(&mut self, s: Stmt, params: &[Val], out: &mut Rows) -> DbResult<()> {
        out.clear();
        let name = std::mem::take(&mut self.stmts[s.0]);
        self.bind_tail(&name, params);
        self.stmts[s.0] = name;
        self.flush()?;
        self.read_answer(Some(out)).map(|_| ())
    }

    fn exec(&mut self, s: Stmt, params: &[Val]) -> DbResult<u64> {
        let name = std::mem::take(&mut self.stmts[s.0]);
        self.bind_tail(&name, params);
        self.stmts[s.0] = name;
        self.flush()?;
        self.read_answer(None)
    }

    fn run(&mut self, sql: &str) -> DbResult<()> {
        frame(&mut self.out, C_PARSE, |b| {
            put_str(b, "");
            put_text(b, sql);
        });
        self.bind_tail("", &[]);
        self.flush()?;
        self.read_answer(None).map(|_| ()).map_err(|e| DbError { class: e.class, msg: format!("{} [{sql}]", e.msg) })
    }

    fn begin(&mut self) -> DbResult<()> {
        let d = self.durability;
        self.txn(C_TXN_BEGIN, Some(d))
    }
    fn commit(&mut self) -> DbResult<()> {
        self.txn(C_TXN_COMMIT, None)
    }
    fn rollback(&mut self) -> DbResult<()> {
        self.txn(C_TXN_ABORT, None)
    }
    fn decode_ns(&self) -> u64 {
        self.decode_ns
    }
    /// `sys.session_state`'s `session_id` for this connection: the wire's
    /// `session_id` is the cancel identity and another number. The row this
    /// connection's own read sees running is its own (no other harness
    /// connection runs a statement while one is opened).
    fn session_key(&mut self) -> DbResult<i64> {
        if self.sys_session < 0 {
            let s = self.prepare("SELECT session_id, state FROM sys.session_state")?;
            let mut rows = Rows::new();
            self.query(s, &[], &mut rows)?;
            let mine: Vec<i64> =
                rows.iter().filter(|r| r[1] == Val::Str("ON_CPU".into())).map(|r| r[0].i()).collect();
            if mine.len() != 1 {
                return Err(other(format!("cannot tell this session in sys.session_state: {rows:?}")));
            }
            self.sys_session = mine[0];
        }
        Ok(self.sys_session)
    }
}

impl Drop for Kwp {
    fn drop(&mut self) {
        self.out.clear();
        frame(&mut self.out, C_TERMINATE, |_| {});
        let _ = self.sock.write_all(&self.out);
    }
}
