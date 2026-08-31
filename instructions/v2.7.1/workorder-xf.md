# Work order XF — the bill XE left: the shipped read's typed delivery, and three smaller debts

Drafted 2026-08-31 by CLA against `main` at `04403a1`
(`v2.7.0-22-g04403a1`). Source of record:
`bench/v2.7.0/results-xe-ack-at-append-v2.7.0-17-ge310f8e.md` (§3, §4.1,
§6) and `docs/inflight/known-gaps.md`'s shipped-read and portal entries.
The operator set the priority order (2026-08-31): (1) the shipped-read
closure, (2) the b=8 confirming repeat, (3) the per-leg timers, (4) the
portal question's referral. The rows below keep that order; XF3 is the
one row cheap enough to interleave anywhere.

## Background

XE's session surfaced two defects while measuring, both real, neither
part of what it was asked:

**(1) The shipped-read refusal now bites production and measurement
alike.** Since the KWP/1 cut (`eecda94`), every session carries a result
sink, and `CommandDispatcher::ShipStatement` refuses a shipped **read**
to a typed client — the ship wire carries rendered text, and a typed
client cannot accept it (`known-gaps.md`, the milestone's own recorded
limitation). Scenario 2's booking opens with a read, so under
`peer_listeners = on` every booker refuses at its first statement —
confirmed empirically on three binaries before concluding (XE §4.1). The
consequence is double: peer listeners are unusable for any typed
workload that reads foreign data (production), and the engine's own
realistic cross-owner benchmark shape is unrunnable (measurement) — XE
had to answer its question with a pure-write substitute and left "a real
booking's cross-owner cost under the new ack timing" explicitly open.

**(2) The portal leak, worked around at the wrong layer.**
`kwp_session.cpp:389-391` discards every non-`C_SYNC` frame while
`skipping_to_sync_` holds — including a `C_CLOSE` bundled in the same
batch as the statement that just errored, which is exactly when the
close matters. The portal leaks; at `kMaxSessionPortals` (64) the
session refuses every further statement, permanently. The cut commit's
own comment claimed "closed on both arms"; the behaviour contradicts
it. XE's session fixed the *client* (`tools/kwp.py`: `C_CLOSE` always
its own frame after the statement's `S_READY`), which unblocked the
cells at the price of **+11-12 µs per statement on every success path**
(measured, XE §3) — a tax every future client pays for a server
behaviour, and the reason XE's absolute latencies are quarantined from
XD's.

Alongside, two measurement debts: the b=8 result (−25.9% p50, +33.3%
TPS) reversed H-XE2's prediction and the results file itself asks for
one confirming repeat before it is treated as settled; and the per-leg
timer gap has now been billed by three files (R6-B §8, XD §8, XE §6) —
every mechanism claim about the commit chain is currently an inference
from totals.

## Conclusions (standing)

1. XE1 (ack-at-append) **stays** — b=1 neutral inside the floor, b=8 a
   large win, 45/45 correctness. Nothing in this order revisits it.
2. The shipped-read closure is a **wire and sink change, so spec and
   ratification precede code** (XF0 gates XF1). The text protocol's
   behaviour and the `cores = 1` path must be untouched — Guideline 2's
   zero-overhead reading applies to the sink seam too.
3. The portal question is **referred, not decided** here (XF5 drafts
   the ask). The client workaround stays in place until the operator
   rules; if the ruling removes the tax, the re-baseline is XF5's
   follow-on, not silently folded into other measurements.
4. No constant is chosen. Batch sizes, caps, and timer bucket bounds
   that want choosing stop and report.

## Hypotheses

- **H-XF1 (typed delivery restores the shape).** With typed shipped
  reads delivered, scenario 2 runs under `peer_listeners = on` on a
  typed client, and its booking cost decomposes as XD measured plus the
  now-cheaper commit (XE) — no new cost category appears. Falsifier:
  a cost the decomposition cannot attribute.
- **H-XF2 (the b=8 saving reproduces).** A fourth independent repeat of
  XE §4.3's `group, pl, b=8` pair lands within the established floors
  (base 12.0%, xe1 8.9%) of the recorded medians. Falsifier: outside
  either floor — which reopens the mechanism question rather than
  settling it.
- **H-XF3 (the timers separate the two mechanisms).** Per-leg wall
  times attribute XE's b=8 saving: if parking discipline dominates,
  the decide-to-ack leg's own time barely moves while commit total
  falls; if sync-sharing dominates, the leg time itself falls. The
  timers make XE §5's "offered, not proven" reading decidable.

## Rows

**XF0 — the typed shipped read: survey and spec.** Source-read first,
with path:line: where the ship wire renders text today (the statement
ship service and the remote step reply path), what size cap the reply
leg carries, and what the `result_sink` seam (`eecda94`'s
`result_sink.hpp`, four emission points) already abstracts. Then the
spec draft: the remote executor emits through a **typed batch sink**
(the same row codec KWP speaks, the 64 KiB batch target already
ratified for it), the ship reply carries those bytes opaquely, and the
session-side sink forwards frames without re-encoding; a text client
keeps the rendered-text arm unchanged. Name the capability/version
story (an old peer and a new session must fail closed, by refusal, not
by misparse), the cancellation story mid-batch, and the memory story
(a batch in flight is bounded by the existing target, not a new
constant). Deliverable: amendment drafts for `protocol.md` /
`crosscore.md` §4 and a ratification ask. **Nothing in XF1 lands
before the ask is answered.**

**XF1 — the build, after ratification.** The sink arm, the ship reply
carriage, the session forward path, and the removal of
`ShipStatement`'s read refusal for typed sessions (the refusal stays
for whatever the spec still excludes, named). Tests: golden
byte-sessions gain a shipped-read session (`kwp_golden.txt`'s
discipline); the text arm byte-identical suite must not move; a
`cores = 1` and a nopl guard cell each, unchanged within floor.

**XF2 — the reopened measurement.** With XF1 landed: the scenario-2
peer-listener matrix rerun on the typed client at XD's scale — the
"real booking under the new ack timing" question XE §6 left open.
Baseline: XD's cells at `951a91a` for shape, XE's §4.3 for the commit
leg — with the client-tax caveat (§3) carried forward until XF5
resolves it. Three repeats, floors first, rule 4b extremes as the host
allows.

**XF3 — the confirming repeat (independent; may run any time the host
is quiet).** One additional full repeat of XE §4.3's `group, pl, b=8`
pair, same binaries by hash, same harness, fresh files. Read against
H-XF2's criterion and append the verdict to the XE results file as a
dated addendum — the file asked for this itself.

**XF4 — the per-leg timers.** Coordinator-side wall-clock spans, read
where the coroutine already suspends and resumes (no new atomics, no
cross-core reads — each span is measured and aggregated on the
coordinator's own core): prepare-sent→all-settled,
local-commit→decision-durable, decide-sent→all-acked, and the whole
`Finish`. Participant-side: prepare-append→durable and
decide-append→(ack, durable) — the pair XF3's question needs. Exposure
per core in `SHOW META`, absent until the first cross-owner commit
(the absent-not-zeroed rule); aggregation shape (histogram vs
count/sum/max) is the builder's proposal in the PR, not a constant
chosen here. Cost guard: a one-owner commit must not read the clock
even once more than today — the spans live inside the enrolled path
only. With the timers in, rerun one `group, pl, b=8` cell and write
H-XF3's verdict.

**XF5 — the portal referral.** Draft the ratification ask with the
options priced, deciding none: (a) `C_CLOSE` survives skip-to-sync
(the skip loop admits close frames — smallest server diff, but widens
what "skipping" means and must state why `C_CLOSE` is safe to honor
for a poisoned batch while nothing else is); (b) a statement error
auto-closes the portal it was executing (matches the practice the cut
comment believed it had; makes `C_CLOSE`-after-error a no-op; the
lifecycle change must be stated in `protocol.md`'s portal section);
(c) status quo — every client pays the measured 11-12 µs per statement
forever, stated as a price, not a default that happened. Include the
sibling question the leak exposed: whether portal exhaustion should
refuse the *session* permanently (today's behaviour) or the *statement*
retryably. If the operator rules (a) or (b): implement behind the
ruling, revert the client workaround, and re-baseline the ~11-12 µs
against XD's client in the same results file.

**XF6 — docs closure.** `known-gaps.md`: the shipped-read entry closed
or narrowed by XF1's actual coverage; the portal entry updated with the
ruling; XE results file gains XF3's addendum and a forward pointer
here; `CLAUDE.md` one line per landed row.

## Measurement

Build-release only; `git describe --tags` on every number; fresh
server and data file per cell; three repeats on any claimed cell (XF3
is itself the fourth of an existing three); floors before deltas;
baselines are our own prior files (XD at `951a91a`, XE at `e310f8e`),
never cross-client absolutes until XF5's re-baseline exists. Results
to `bench/v2.7.0/` per the 2026-08-25 filing rule unless the operator
names a version of record.

## Improvement

What this order buys: peer listeners become usable by typed clients
whose transactions read foreign data — the production limitation and
the measurement blocker close together; the engine's one realistic
cross-owner benchmark shape runs again and prices the ack-at-append
change on real bookings; the b=8 reversal is either confirmed or
reopened rather than assumed; mechanism claims about the commit chain
gain an instrument after three files billed for it; and the portal
workaround's per-statement tax becomes an operator decision instead of
an accident. What it does not buy: the session↔owner mismatch is
untouched (routing/affinity stays its own track), the two-sync floor of
the chain stands, and nothing here revisits DA2 or the mover.

---

## Row status (CLA, appended as rows land)

| row | status |
|---|---|
| XF0 | **Done 2026-08-31** on the `xf` worktree — survey and spec draft at `docs/inflight/blocked/workplan-shipped-read-typed.md`, ask at `instructions/v2.7.1/ratification-xf0.md`. Two corrections to the sketch above are in the ask's "one correction" section and the workplan §2/§3c |
| XF1 | **Blocked** on the XF0 ask |
| XF2 | **Blocked** on XF1 |
| XF3 | see the addendum in `bench/v2.7.0/results-xe-ack-at-append-v2.7.0-17-ge310f8e.md` |
| XF4 | — |
| XF5 | **Done 2026-08-31** — ask at `instructions/v2.7.1/ratification-xf5.md` |
| XF6 | partial: `known-gaps.md` gains the portal-leak entry XE found and nothing had registered |
