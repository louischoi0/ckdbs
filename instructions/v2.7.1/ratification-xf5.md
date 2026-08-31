# Ratification ask XF5 — the portal a failed statement leaves behind

Drafted 2026-08-31 by CLA on the worktree `xf` at `04403a1`
(`v2.7.0-22-g04403a1`), as row **XF5** of work order XF
(`instructions/v2.7.1/workorder-xf.md`). **Nothing is built under this
ask, and no option is preferred.** The order's conclusion 3 refers this
rather than deciding it; the client-side workaround stays in place until
the operator rules.

## The defect, located exactly

A statement that fails leaves its portal in the session's table.
`OnStatementComplete`'s error arm resets the sink and clears the running
name, and **does not erase the portal**
(`src/server/kwp_session.cpp:709-715`). The client's own `C_CLOSE` cannot
fix it if the close was pipelined into the same batch: `Refuse` arms
`skipping_to_sync_` (`:330-338`), and the skip loop discards every
non-`C_SYNC` frame — "discarded, silently" (`:388-394`). So the close is
dropped on exactly the statements that most needed it.

The cut commit's own comment claimed portals were "closed on both arms".
They are not, on the arm that errors.

## Three corrections to how XE stated it, because each changes the price

XE reported this as "the session then refuses every further statement,
permanently"
(`bench/v2.7.0/results-xe-ack-at-append-v2.7.0-17-ge310f8e.md` §3). The
observation was real — it stalled every cell of that file's first attempt
— but three details are not what the code does, and the ask is worse if it
is priced on the description rather than the behaviour.

1. **The session is not closed.** `ErrorFromStatus` sets
   `Severity::kError` and the comment beside it is explicit: "an engine
   failure never closes the connection (§11)"
   (`src/wire/error_registry.cpp:90-93`). What is refused is the
   *statement*.
2. **It is not permanent.** The portal-idle sweep runs and is wired —
   `TcpServer` submits it every `kPortalIdleTimeoutNs / 4` = 15 s
   (`src/server/tcp_server.cpp:180-187`) and it erases any portal idle for
   60 s (`kwp_session.cpp:964-971`). A leaked portal's `idle_since` is
   never refreshed, so it ages out.
3. **What actually happens is worse than a refusal and better than a
   wedge**, and is worth naming because it is what a client sees: once 64
   portals are held, `C_BIND` is refused before it inserts
   (`:549-556`), so the table stops growing and the sweep frees roughly
   one slot per statement per 60 s. A tight retry loop therefore runs at
   **one statement per portal-lifetime**, indefinitely. That is why it
   read as permanent.

**A fourth detail decides the sibling question below.** The refusal is
`ResourceExhausted` with `retryable = 0` — `IsRetryable` is one code wide
by construction, `code == kTxnConflict` and nothing else
(`include/kds/base/status.hpp:156`, the rule `docs/spec/protocol.md` §11
states). So today's behaviour is *the statement refused
non-retryably*, which is neither of the two the order's sibling question
offers.

## What the workaround costs, and who pays it

XE fixed the **client**: `tools/kwp.py` never bundles `C_CLOSE`, sending
it as its own frame after the statement's `S_READY`
(`tools/kwp.py:387-397`, `:446`). Measured cost on the success path, in
XE's own isolation cell (`durability = relaxed`, `cores = 1`, 2,000
single-row `INSERT`s, `e310f8e`, that session's host): round trip
**28.1 µs p50 / 30.0 µs mean → 39.2 µs p50 / 42.3 µs mean**, i.e.
**+11.1 µs p50, +12.3 µs mean per statement**. Every client that
reproduces this workaround pays it on every statement, forever, for a
server behaviour.

That number is XE's, taken on a different host from this worktree's, and
it is quoted as the order's own baseline rather than re-measured here.

## The options, priced, none preferred

### (a) `C_CLOSE` survives skip-to-sync

The skip loop admits `C_CLOSE` alongside `C_SYNC` and dispatches it.

- **Diff:** `kwp_session.cpp:388-394` gains a branch, plus whatever
  `OnClose`'s own refusal path (a malformed `C_CLOSE`, `:889-891` and
  `:903-905`) means while already skipping — a refusal inside a skip is a
  second `S_ERROR` for one batch, which §5's contract does not describe.
- **Cost:** it widens what "skipping" means, and the spec must then say
  **why `C_CLOSE` is safe to honour for a poisoned batch when nothing else
  is**. The honest argument is that `C_CLOSE` names a resource rather than
  advancing the statement, so honouring it cannot change what the failed
  batch did — but that argument has to be written, not assumed.
- **The order calls this "the smallest server diff". It is not** — see (b).

### (b) A statement error auto-closes the portal it was executing

`OnStatementComplete`'s error arm erases the portal instead of only
resetting its sink.

- **Diff: one line** at `kwp_session.cpp:712`, beside
  `portal->sink.Reset()`, which already has the portal in hand. This is
  the smallest of the three.
- It matches the practice the cut commit believed it had, and makes a
  later `C_CLOSE` of that portal a no-op — which it already is, since
  `portals_.erase` on an absent name is silent (`:893`).
- **Cost:** a portal lifecycle change, and `protocol.md`'s portal section
  must state it: a portal ceases to exist when its statement fails,
  so `C_DESCRIBE` or `C_CONTINUE` of it afterwards is "no such portal"
  rather than "a portal holding nothing". A client that today distinguishes
  those two would see the answer change.

### (c) Status quo — the tax is the price, stated as one

Every client reproduces XE's workaround and pays ~11-12 µs per statement.

- **Diff:** none, and the newline arm and golden sessions do not move.
- **Cost:** the price is paid by every driver ever written against this
  server, for a defect in the server, and `protocol.md` §5 must then say
  plainly that a `C_CLOSE` pipelined behind a statement is dropped if that
  statement fails — because today nothing says so and the code comment
  that touches it says the opposite.

## The sibling question, restated to match the code

The order asks whether portal exhaustion should "refuse the *session*
permanently (today's behaviour) or the *statement* retryably". Neither is
today's behaviour (§"corrections", point 4). The question that is actually
open:

**Should `ResourceExhausted` at the portal limit carry `retryable = 1`?**

It cannot simply be flipped. `IsRetryable` is one code wide by design —
`docs/spec/protocol.md` §11 — so a retryable portal-limit refusal needs
one of:

- **(i)** amend §11's one-code rule so the bit is per-refusal rather than
  per-code, which touches every client retry loop's premise;
- **(ii)** leave the bit at 0 and rely on the message, which is what
  happens today: `"this session already holds 64 portals; close one before
  binding another"` (`:551-554`) is accurate and actionable;
- **(iii)** make the question moot by fixing the leak — (a) or (b) — after
  which reaching 64 portals means a client genuinely holding 64, which is
  a client defect and correctly non-retryable.

**Note that (iii) is not a third answer to the sibling question so much as
a reason it may not need one.** Stated because choosing (c) above is the
one branch on which the sibling question stays live.

## What CLA does with each ruling

- **(a) or (b):** implement behind the ruling, revert `tools/kwp.py`'s
  workaround, and **re-baseline the ~11-12 µs against XD's client in one
  results file** — the order requires this rather than letting the tax
  disappear silently into a later measurement's absolutes.
- **(c):** amend `protocol.md` §5 to state the drop, and register the tax
  in `known-gaps.md` as a stated price.
- Either way, `known-gaps.md` gains the entry this defect never had — it
  is registered nowhere today, which is why XE found it by being stopped
  by it.
