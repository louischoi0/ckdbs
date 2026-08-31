# Ratification KW-D — the six protocol decisions

Ratified by the operator 2026-08-31 against `main` at `c8e3d31`
(`v2.2.1-153-gc8e3d31`). Addendum to `kw-kwp-protocol.md`.

Standing qualifier on every item the operator marked **"운영 가능한
수준일 것"** — operable, not placeholder. Where a value is named below it
is named to be shipped, and where CLA cannot yet defend one it says so
rather than picking a number to fill the slot.

## KW-D1 — the stub layer is skipped

CLA's proposal accepted. P08 binds to the real dispatcher and P11 to the
real `TxnController`; **P17 is struck** — it existed only to swap stubs
the engine no longer needs, since M1-M3 have landed.

**What the operator's qualifier costs here.** The workplan's stub was
what made P08's and P11's tests hermetic: a canned executor cannot fail
for a reason the protocol did not cause. Binding to the real engine
means a P08 failure can now originate anywhere below it. Two
requirements follow:

- P08's and P11's tests **fail for protocol reasons only**. Where a test
  needs a stable result it uses a fixed relation and a fixed statement,
  not an arbitrary one — the determinism the stub provided moves into
  the fixture.
- **P17's acceptance checklist is not lost with P17.** It listed
  spec §15-5's crash-injection half: acked D1/D2 commits survive, the D3
  window is bounded. That belongs to P11 now. Carry it, or the one thing
  P17 was going to prove goes unproven.

## KW-D2 — `kMaxFrame` = 16 MiB

**Recorded as CLA's choice, not as an accepted proposal.** CLA reported
that §34 marks this `[OPEN]` and offered no number; the value is chosen
here.

16 MiB — already the value in `include/kds/wire/kwp.hpp:50`, so this
ratifies what the header holds rather than moving it.

Derivation, since every constant in this tree carries one: `kMaxFrame`
is a **sanity ceiling on `length`**, not a sizing target. It exists so a
corrupt or hostile `length` is refused before the decoder allocates
against it, so it wants to sit far above any legitimate frame and far
below anything that would let one connection exhaust the server. 16 MiB
does both — the largest legitimate frame is a row batch, whose target is
≤64 KiB (§9's builder), making the ceiling 256× the largest thing the
server intends to send, and a per-connection preallocated buffer at this
bound is affordable.

**One interaction worth stating**: the cross-core ring's reply cap is 992
bytes, so `kMaxFrame` bounds nothing a *shipped* statement can produce.
It bounds a locally-answered result batch only.

## KW-D3 — portal-idle timeout = 60 s

**Also CLA's choice rather than an accepted proposal**, same reason.

60 seconds, on the injected clock, behind a named constant as P10
requires.

Derivation: this tree already has a 60 s reply deadline in the two
places that wait on a human-or-network-scale event —
`kIndexBuildReplyDeadlineNs` (`index_build_service.hpp:156`) and
`kAssertionBuildReplyDeadlineNs` (`assertion_build_service.hpp:152`) —
against 10 s for the machine-scale waits,
`kShippedStatementDeadlineNs` and `kTxnPhaseDeadlineNs`. **An idle
portal is waiting on a client**, which is the first class, so it takes
the first number. Reusing an existing value rather than inventing a
third is deliberate.

**What this constant is not.** Not a statement timeout and not a
transaction timeout. It bounds how long an *unread* portal holds its
executor cursor, which is why P10's pin-release note exists: a timed-out
portal releases its cursor through the seam.

**Measure before shipping, and be willing to move it.** No workload has
ever held a portal open in this engine, because portals do not exist
yet. 60 s is defensible, not measured; P16's conformance run is the
first place a real number could come from.

## KW-D4 — `STOP` as an admin statement: deferred

Registered as a later task, not built in KW. `STOP` stays today's
unauthenticated line command for the duration of this milestone.

**Consequence, recorded so it is not discovered later.** §106's claim
that the protocol gives *"one surface, one auth story"* is **not true at
the end of KW** — an unauthenticated administrative command survives
beside an authenticated protocol. Amend §106 to say so rather than
leaving a claim the build does not meet.

**This interacts with KW-D6.** Cutting the text port to off-by-default
while `STOP` lives on it means `STOP` becomes reachable only on the
debug port. Establish during P13 whether that is acceptable
operationally or whether `STOP` needs an interim home; if the latter,
this deferral has to be revisited inside KW rather than after it.

## KW-D5 — `COMPRESSION` capability bit: deferred

CLA's proposal accepted. Excluded from v1. Adding it later is not a
version break — that is what the capability bit is for — and the spec's
own framing is *"if ever needed"*. The bit stays reserved in the enum;
nothing is removed.

## KW-D6 — the text protocol is cut over at once

**The operator's decision, and the opposite of CLA's proposal.** CLA
proposed running both surfaces through P16 and flipping the default
afterwards; the operator directs a single cut-over.

Recorded with the cost CLA raised, because the decision does not remove
it: **P16's golden byte-sessions are what would catch a KWP regression,
and a cut-over before P16 exists makes the first real user of the new
protocol the test suite that was meant to validate it.**

**So the sequence changes — this is the adaptation the decision
requires, not a re-argument of it.** P16's golden-session half moves
before P13:

| Order | Rows |
|---|---|
| 1 | P01-P04 |
| 2 | P07, P09-remainder, P12 — independent of each other |
| 3 | P08, P10, P11 — bound to the real engine per KW-D1 |
| 4 | **P16 first half** — golden byte-sessions against the endpoint **in-process**, before any port moves |
| 5 | P13 — the cut |
| 6 | P14, P15, P16 second half — wired into `test.sh`, regression-mandatory |

Row 4 is not in `protocol-wp.md`. P16 as written needs P13 and P15,
which is true of its *socket-level* half only; its golden-session half
needs neither.

**The paired benchmark reading is mandatory.** Every number in `bench/`
is a newline-protocol number, and `42fca65` has just made the baseline
this repository's own last run. Take one scenario before and after the
cut, named with `git describe --tags`. If the protocol moves it, every
prior baseline is a baseline for a different client surface and the
results files must say so.

**What "at once" does and does not mean.** The text protocol is not
deleted — §12 keeps it as a documented loopback debug surface. What
happens at once is that it stops being the default and every test,
benchmark harness and `tools/ckdbs_cli.py` moves in the same change.
That is a large single commit by this repository's standards; splitting
it into "endpoint speaks KWP" and "harnesses follow" is not running two
surfaces and does not contradict this decision.

## Deferred by this ratification, for whoever opens the follow-up

- `STOP` as a capability-gated admin statement (KW-D4)
- `COMPRESSION` capability bit (KW-D5)
- SCRAM parameters — §14, `[OPEN]`, untouched here
- Credit/window flow control — §83, explicitly *"if ever needed"* behind
  a capability bit
- Smart-routing topology extension — §91, must arrive as a capability
  bit, never a version break
