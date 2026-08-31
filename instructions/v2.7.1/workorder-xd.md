# Work order XE — the third leg's ack, moved to the append (D2 only)

Drafted 2026-08-31 by CLA against `main` at `00b65ff`
(`v2.7.0-13-g00b65ff`). This is the enactment of ratification ask
`instructions/v2.7.1/ratification-xd1.md`, whose three questions the
operator answered **accepted** by directing this order (2026-08-31,
per CLA's recommendation on all three). If any of the three answers is
in fact different, this order does not proceed and says so:

1. A cross-owner transaction's durability point is the **coordinator's
   decision record**; a participant's own terminal record is a redo
   shortcut, not part of the client's D1/D2 promise. Accepted — to be
   stated in `cross-owner-txn.md` §2 and §4.
2. The retention obligation (no recycling of a decision-bearing segment
   until every participant's terminal record is durable; a pre-durable
   ack does not discharge it) lands in the spec **now**, ahead of any
   retention policy. Accepted.
3. D1 `strict` keeps three syncs and is out of scope. Accepted.

## Background

The participant's decide handler makes its own COMMIT durable **before**
acking (`shipped_statement_executor.cpp:839-841` via `DispatchAsync`,
"parked on `IsDurable` before this callback ran") — the third of the
three serialized device syncs XD0 counted (3.0088/booking cross-owner
under group, `bench/v2.7.0/results-xd-commit-decomposition-v2.7.0-2-g951a91a.md`
§2) and XD3 priced (~1,002 µs per leg at b=1 on this device, additive
model within 8.2%). The ratification ask's verdict, source-read at
`3c42d74` and re-confirmed at `951a91a`: the wait buys nothing the
protocol promises — the coordinator answers the client from the
decision record alone even with zero acks
(`command_dispatcher.cpp:493-505`), every crash point re-resolves to
the same outcome by redo-or-resolution (`prepared_resolver.cpp:55,
:76-90, :132-135`), the checkpoint cannot strand the prepare (the
`CHECKPOINT_END` sync necessarily carries the earlier COMMIT — a
property that holds today, not one this order introduces), and under D2
the append already registers `pending_group_commits_` so the next
`DrainOnce` syncs it whether or not anyone parks
(`wal/manager.cpp:203, :225-231`). What is removed is a
serialization, not a durability.

## Conclusions (decided; the build enacts)

1. **D2 `group` only.** Under D1 the sync is inline in `Commit` before
   it returns (`wal/manager.cpp:194-200`) — there is no wait after the
   append to move, and buying one would change what `strict` means. D3
   already does not take this wait (XD0's 2.00 count). The change is a
   D2 branch, and the other two classes must be byte-identical in
   behaviour before and after.
2. **The spec lands first.** Both amendments of the ratification are
   XE0's and nothing in XE1+ lands before them.
3. One-owner transactions and `cores = 1` are untouched — the changed
   site is inside the decide handler, reached only by an enrolled
   participant. XE4 verifies rather than assumes.
4. The reactor-blocked-in-`fdatasync` finding (XD4's 88% unaccounted)
   is **named and out of scope**: moving sync execution off the reactor
   is a separate design question against XD0's corrected picture (the
   writer thread takes D3's tick alone), not a rider on this order.
5. No constant is introduced. Anything wanting one stops and reports.

## Hypotheses

- **H-XE1 (the saving at b=1).** With the ack at the append,
  syncs-per-booking on the critical path falls 3 → 2 and commit p50
  falls by about one leg: from ~2,930 µs toward ~1,930 µs on this
  device, saving ≥ 500 µs. **Falsifier:** saving under 500 µs — which
  would mean the third leg was already riding something XD3's additive
  read missed.
  (Total `wal_syncs`/booking may stay near 3.00 — the participant's
  COMMIT still syncs on the next drain; what changes is whether the
  booking *waits* for it. The counter to watch is latency, with the
  sync count explaining where durability went, not whether.)
- **H-XE2 (the saving shrinks with load).** At b=8 the drain-sharing
  discount already absorbs part of leg 3 (2.45-2.50 syncs/booking,
  XD2), so the b=8 saving is real but smaller than b=1's. Measured,
  not assumed; no falsifier — this row sizes, it does not test.
- **H-XE3 (no correctness regression).** The kill −9 matrix's equal-
  counts oracle holds at every crash point including the new
  pre-durable-ack window; `shipped_enrolment_expiries` and
  `txn_in_doubt_unresolved` stay 0 in every measured cell; a crash
  between ack and the drain re-resolves to COMMIT by redo or by the
  resolver, never to a torn pair. **Falsifier:** any unequal count or
  any runtime-unresolvable transaction.

## Rows

**XE0 — the two spec amendments.**
(a) `cross-owner-txn.md` §2: the decide step's text gains the stated
contract — the decision's durability point is the coordinator's record;
a participant acks at its COMMIT **append** under D2, and its own
record's durability rides the next drain; §4 (what a client sees) gains
the restatement that the client's answer never depended on participant
acks, now stated rather than implied.
(b) `cross-owner-txn.md` §2c (and an index line in `wal.md` §15's open
retention item): the retention obligation, verbatim in force from this
commit — a coordinator's stream may not recycle a segment holding a
decision until every participant of that transaction has made its own
terminal record durable; a pre-durable ack does not discharge this;
either the ack carries the participant's durable point or retention is
floored by something else. Cross-cite `ratification-xd1.md`.

**XE1 — the code change.** In the decide path's commit arm, under D2
only: send the ack after the COMMIT append succeeds, before the
`IsDurable` park; the park itself is deleted for this arm (the drain
syncs regardless — `pending_group_commits_`), not skipped-but-kept.
D1's inline path and D3's are untouched by construction, and the diff
must make that visible (a class branch, not a rewrite of the shared
path). The `participant.decide_applied_preack` crash point stays where
it is; add `participant.decide_acked_predurable` between the ack send
and the drain's sync so XE2 can crash inside the new window.

**XE2 — the crash matrix, extended.** `bench/txn_2pc_kill_matrix_probe.py`
gains cells at the new crash point, both ordinals, three passes, same
equal-counts oracle. One targeted cell beyond the matrix's shape: crash
after ack, before the drain, with a checkpoint forced in between — the
window §2's checkpoint argument covers — expecting COMMIT on both
relations at mount. Note for the runner: the simulation corpus mounts
core 0 alone and cannot host this; the kill matrix runs real cores and
is the right instrument (the ratification's own observation).

**XE3 — unit coverage.** `txn_2pc_protocol_test.cpp`: the ack precedes
durability under D2 and does not under D1; the in-doubt bookkeeping is
unmoved (an acked participant is out of `in_doubt_` exactly as before).
`shipped_statement_executor_test.cpp`: the context erase and counter
increments happen at the same points as before relative to the ack.
Suite green before XE4 runs anything.

**XE4 — the A/B.** Build-release, same host, same driver and guards as
XD (`bench/wal_sync_decomposition_probe.py`, `--require-shipped` /
`-rate 0.97`), fresh files, three repeats on every claimed cell:

- group, pl, b=1 and b=8: commit percentiles and syncs/booking, before
  (main at this order's base) and after (XE1) — H-XE1/H-XE2's numbers;
- group, **nopl**, b=1 and b=8: must be unchanged within the noise
  floor (conclusion 3's verification);
- strict, pl, b=1, one repeat: must be unchanged (conclusion 1's);
- relaxed, pl, b=8, one repeat: must be unchanged (already 2.00).

Noise floors stated per shape from this session's own repeats before
any delta is read; XD5 put the cross-owner p50 floor at ~7%, so a b=8
claim under that is not made.

**XE5 — docs closure.** `ratification-xd1.md` marked ratified-and-
enacted with the commit; the XD results file gains a forward note;
`CLAUDE.md`'s milestone row one line; `client-manual.md`'s `wal_syncs`
row gains the one reading change (a cross-owner booking's *waited*
syncs are 2 under group, total unchanged).

## Measurement

Every claim tagged measured (invocation, `git describe --tags`) or
source-read (path:line at commit). Results to
`bench/v2.7.1/results-xe-ack-at-append-<describe>.md` if the operator
names a v2.7.1 version of record by then, else `bench/v2.7.0/` per the
2026-08-25 filing rule. Baseline is XD's own cells at `951a91a`.
Rule 4b: b=1 and b=8 are the host's meaningful extremes for this shape;
the 8-logical/4-physical ceiling is stated where it binds.

## Improvement

What this order buys: the shipped default class's cross-owner commit
chain goes from three serialized device syncs to two, worth about a
third of the b=1 chain (~1 ms on this device) and a measured, smaller
amount under load; the durability contract the protocol already
practiced becomes the one the spec states; and the retention hole a
future policy would have inherited is closed before that policy exists.
What it does not buy: D1 and D3 are unchanged by design; the sync
still executes on the reactor (conclusion 4's separate question); the
booking is still cross-owner at all — the routing/affinity track is
untouched; and nothing here revisits R6-B's numbers, which XD already
reconciled.
