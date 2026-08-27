#!/usr/bin/env python3
"""Does `cabin_optimizer_amort_windows` reach the live controller's DROP
cooldown, or only its struct? A two-arm behavioural check.

`docs/spec/physical-optimizer.md` §II.4 fuses one number into both sides of
the cost model: the admission bar is `P_rel / T_amort` and the DROP cooldown
is `2 x T_amort` decay half-lives. The key was ratified at 64 on 2026-08-10.
A results document that measures a workload at the new default is worthless
if the key is accepted at startup and then ignored, so this driver answers
the question the cheap way - by **behaviour**, not by a startup log line:

    two servers, identical in every respect except `amort_windows`,
    warmed on the same hot probe until each controller has an ACTIVE
    Cabin of its own, then left in silence while both are polled.

The control arm (`amort_windows = 1`) must go DECAYING and then DROP within
a couple of half-lives; the test arm (`amort_windows = 64`) must **not**
drop for `2 x 64` half-lives after its own DECAYING onset. Two facts fall
out of one run: the key is live, and the cooldown it produces is the
specified multiple rather than some other number.

The half-life is the affordability knob and is deliberately *not* what is
under test: at `decay_half_life = 1` the ratified cooldown is 128 s instead
of the 21 h 20 m it is at the shipped 600 s, and the ratio between the arms
- the thing `amort_windows` sets - is unchanged. Run the arms at the same
half-life or the comparison means nothing.

Nothing here is timed: there are no latencies and no percentiles, because
every output is a wall-clock instant of a state transition or a counter.
The latency question belongs to `scenario4_cabinopt_days.py`, whose
relation shape and row generator this driver imports so the two cannot
drift.

    ./build-release/kds_server ~/bench-cabinopt-cool/a1.db --port 15661 \
        --config amort1.conf &
    ./build-release/kds_server ~/bench-cabinopt-cool/a64.db --port 15662 \
        --config amort64.conf &
    ./tools/cabinopt_cooldown_check.py \
        --arm amort1:15661:1 --arm amort64:15662:64 \
        --half-life 1 --silence-seconds 200 --json cool.json

**The DECAYING onset is the second thing this driver measures**, and since
2026-08-10 it is reported against a prediction that needs no calibration.
The controller enters DECAYING when `B < theta_drop x C`, where B decays on
the R1 clock and `C = P_rel / T_amort` does not, so

    onset(T_amort) - onset(T_amort') = log2(T_amort / T_amort') half-lives

exactly - every workload-dependent term (the accumulated frequency at the
last probe, the relation's page count, theta_drop) cancels in the
difference. The arm with the smallest window is taken as the reference and
every other arm's onset is printed against that prediction. Arms may
therefore differ in `amort_windows` alone and still be a quantitative test
rather than a qualitative one; running the same window on two *binaries*
(different ports, same config) is how that becomes an A/B.

`AMORT` and the optional `COOLDOWN` field are what the arm's config sets -
the driver cannot read a key back, so both are stated. Since the cooldown
became its own key (`cabin_optimizer_cooldown_half_lives`, default 128) the
predicted DROP is that number of half-lives and no longer `2 x AMORT`;
omitting the field keeps the old expression, which is what a pre-decoupling
server does.
"""

import argparse
import datetime
import json
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ckdbs_cli import DEFAULT_HOST, ServerConnection, format_reply
from scenario4_cabinopt_days import (COLUMNS, CLUSTERED, MATCHES, board_symbol,
                                     insert_sql, make_board_rows, parse_kv,
                                     probe_sql)

HOT_SYMBOLS = 4     # the warm phase's hot set: one shape, a few values


def abort(message, reply=None):
    print(f"cabinopt_cooldown_check aborted: {message}", file=sys.stderr)
    if reply:
        print(f"  server said: {reply}", file=sys.stderr)
    sys.exit(1)


class Arm:
    def __init__(self, name, host, port, amort, timeout, cooldown=None):
        self.name = name
        self.amort = amort
        # Whole decay half-lives of DECAYING dwell before a DROP. Its own
        # server key since 2026-08-10; `None` means "read it the old way",
        # i.e. the `2 x T_amort` expression a pre-decoupling build fuses.
        self.cooldown = cooldown
        try:
            self.conn = ServerConnection(host, port, timeout=timeout)
        except OSError as e:
            abort(f"arm '{name}': could not connect to {host}:{port}: {e}")
        self.errors = 0
        self.first_error = None
        self.t_active = None        # wall s from warm start
        self.t_decaying = None      # wall s from silence start
        self.t_drop = None          # wall s from silence start
        self.trace = []

    def __call__(self, command):
        reply = format_reply(self.conn.send_command(command))
        if reply.startswith("ERR"):
            self.errors += 1
            if self.first_error is None:
                self.first_error = f"{command}  ->  {reply}"
        return reply

    def observe(self, t_rel):
        """One `SHOW CABIN_OPTIMIZER` capture, appended to the trace."""
        reply = self("SHOW CABIN_OPTIMIZER")
        lines = reply.splitlines()
        header = parse_kv(lines[0])
        entries = [parse_kv(ln) for ln in lines[1:] if ln.strip()]
        cap = {"t": round(t_rel, 2), "header": lines[0],
               "entries": [ln for ln in lines[1:] if ln.strip()]}
        self.trace.append(cap)
        return header, entries


def load(arm, table, rows, seed):
    rng = random.Random(seed)
    reply = arm(f"CREATE TABLE {table} ({COLUMNS}) {CLUSTERED}")
    if reply.startswith("ERR"):
        abort(f"CREATE TABLE {table} failed on {arm.name}", reply)
    pending = 0
    arm("BEGIN")
    for row in make_board_rows("a", rows, rng):
        arm(insert_sql(table, row))
        pending += 1
        if pending >= 500:
            arm("COMMIT")
            arm("BEGIN")
            pending = 0
    arm("COMMIT")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--arm", action="append", default=[],
                    metavar="NAME:PORT:AMORT[:COOLDOWN]",
                    help="one server per arm; AMORT is what its config sets "
                         "cabin_optimizer_amort_windows to (the DECAYING-onset "
                         "prediction reads it), and the optional COOLDOWN is "
                         "cabin_optimizer_cooldown_half_lives - omitted, the "
                         "predicted DROP falls back to the pre-decoupling "
                         "2 x AMORT")
    ap.add_argument("--half-life", type=float, required=True,
                    help="the servers' decay_half_life in seconds; both arms "
                         "must share it or the comparison is meaningless")
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--suffix", default="c")
    ap.add_argument("--seed", type=int, default=20260810)
    ap.add_argument("--warm-seconds", type=float, default=20.0,
                    help="upper bound on the warm phase; it ends early once "
                         "every arm holds an ACTIVE managed entry")
    ap.add_argument("--warm-block", type=int, default=60,
                    help="probes per arm per interleave block while warming")
    ap.add_argument("--silence-seconds", type=float, default=200.0)
    ap.add_argument("--poll-seconds", type=float, default=1.0)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    arms = []
    for spec in args.arm:
        parts = spec.split(":")
        if len(parts) not in (3, 4):
            abort(f"--arm wants NAME:PORT:AMORT[:COOLDOWN], got '{spec}'")
        arms.append(Arm(parts[0], args.host, int(parts[1]), int(parts[2]),
                        args.timeout,
                        int(parts[3]) if len(parts) == 4 else None))
    if len(arms) < 2:
        abort("two arms minimum: the control is the measurement")

    table = f"cool_{args.suffix}"
    domain = args.rows // MATCHES
    hot = [board_symbol("a", i) for i in range(HOT_SYMBOLS)]

    run_t0 = time.time()
    for arm in arms:
        load(arm, table, args.rows, args.seed)
        reply = arm("SET CABIN_OPTIMIZER ON")
        if not reply.startswith("OK"):
            abort(f"SET CABIN_OPTIMIZER ON refused on {arm.name}", reply)

    # ---- warm: interleaved hot probes until every arm is ACTIVE -----------
    rng = random.Random(args.seed + 1)
    warm_t0 = time.perf_counter()
    while time.perf_counter() - warm_t0 < args.warm_seconds:
        draw = [rng.choice(hot) for _ in range(args.warm_block)]
        for arm in arms:
            for sym in draw:
                arm(probe_sql(table, sym))
        t = time.perf_counter() - warm_t0
        done = True
        for arm in arms:
            _, entries = arm.observe(t)
            active = any(e.get("state") == "ACTIVE" for e in entries)
            if active and arm.t_active is None:
                arm.t_active = round(t, 2)
            if not active:
                done = False
        if done:
            break
    for arm in arms:
        if arm.t_active is None:
            abort(f"arm '{arm.name}' never reached ACTIVE inside "
                  f"{args.warm_seconds}s of warm-up - nothing to time")

    # ---- silence: no probe touches the relation again ---------------------
    sil_t0 = time.perf_counter()
    while time.perf_counter() - sil_t0 < args.silence_seconds:
        time.sleep(args.poll_seconds)
        t = time.perf_counter() - sil_t0
        for arm in arms:
            header, entries = arm.observe(t)
            if any(e.get("state") == "DECAYING" for e in entries) \
                    and arm.t_decaying is None:
                arm.t_decaying = round(t, 2)
            if int(header.get("drops", 0)) > 0 and arm.t_drop is None:
                arm.t_drop = round(t, 2)
        if all(a.t_drop is not None for a in arms):
            break

    # ---- the report -------------------------------------------------------
    print(f"cooldown check: {args.rows}-row relation, decay_half_life="
          f"{args.half_life}s, silence up to {args.silence_seconds}s")
    print(f"{'arm':14s} {'amort':>7s} {'ACTIVE@warm':>12s} {'DECAYING@sil':>13s} "
          f"{'DROP@sil':>9s} {'observed cd':>12s} {'predicted cd':>13s}")
    rows_out = []
    for arm in arms:
        cooldown_hl = arm.cooldown if arm.cooldown is not None else 2 * arm.amort
        predicted = cooldown_hl * args.half_life
        observed = (round(arm.t_drop - arm.t_decaying, 2)
                    if arm.t_drop is not None and arm.t_decaying is not None
                    else None)
        print(f"{arm.name:14s} {arm.amort:7d} {arm.t_active:11.2f}s "
              f"{'-' if arm.t_decaying is None else f'{arm.t_decaying:12.2f}'}s "
              f"{'-' if arm.t_drop is None else f'{arm.t_drop:8.2f}'}s "
              f"{'-' if observed is None else f'{observed:11.2f}'}s "
              f"{predicted:12.1f}s")
        rows_out.append({"arm": arm.name, "amort_windows": arm.amort,
                         "cooldown_half_lives": cooldown_hl,
                         "half_life_s": args.half_life,
                         "t_active_warm_s": arm.t_active,
                         "t_decaying_silence_s": arm.t_decaying,
                         "t_drop_silence_s": arm.t_drop,
                         "observed_cooldown_s": observed,
                         "predicted_cooldown_s": predicted,
                         "final": arm.trace[-1] if arm.trace else None,
                         "errors": arm.errors})

    # ---- the DECAYING onset, against the calibration-free prediction ------
    #
    # B decays and C does not, so widening the window by a factor k pushes
    # the onset back by exactly log2(k) half-lives. The reference arm is
    # the narrowest window; everything workload-dependent cancels in the
    # difference, so no fitted constant appears anywhere below.
    ref = min((a for a in arms if a.t_decaying is not None),
              key=lambda a: (a.amort, a.name), default=None)
    if ref is not None:
        import math
        print(f"\nDECAYING onset vs the reference arm '{ref.name}' "
              f"(amort={ref.amort}, onset {ref.t_decaying:.2f}s) - the model "
              f"says log2(amort/{ref.amort}) half-lives later:")
        print(f"{'arm':14s} {'amort':>7s} {'onset':>8s} {'d observed':>11s} "
              f"{'d predicted':>12s} {'error':>8s}")
        for arm in arms:
            if arm.t_decaying is None:
                print(f"{arm.name:14s} {arm.amort:7d} {'-':>8s}")
                continue
            d_obs = arm.t_decaying - ref.t_decaying
            d_pred = math.log2(arm.amort / ref.amort) * args.half_life
            print(f"{arm.name:14s} {arm.amort:7d} {arm.t_decaying:7.2f}s "
                  f"{d_obs:10.2f}s {d_pred:11.2f}s {d_obs - d_pred:7.2f}s")
            for row in rows_out:
                if row["arm"] == arm.name:
                    row["onset_delta_observed_s"] = round(d_obs, 2)
                    row["onset_delta_predicted_s"] = round(d_pred, 2)
                    row["onset_reference_arm"] = ref.name

    # The verdict is a comparison, not a threshold: the control must drop
    # and the test arm must not have dropped before its predicted cooldown.
    # It is only *asked* when the silence window is long enough for the
    # shortest predicted cooldown to land inside it - an onset-only run
    # (every arm's cooldown longer than the silence) has no cooldown
    # evidence to give, and reporting NO for it would be reporting the
    # driver's own pacing as a failure.
    control = min(arms, key=lambda a: a.amort)
    test = max(arms, key=lambda a: a.amort)
    shortest_cd = min((a.cooldown if a.cooldown is not None else 2 * a.amort)
                      for a in arms) * args.half_life
    cooldown_reachable = shortest_cd < args.silence_seconds
    live = (control.t_drop is not None and
            (test.t_drop is None or test.t_drop > control.t_drop))
    if cooldown_reachable:
        print(f"\nkey reaches the live cooldown: "
              f"{'YES' if live else 'NO'}  "
              f"(control amort={control.amort} dropped at "
              f"{control.t_drop}s; test amort={test.amort} at {test.t_drop}s)")
    else:
        live = None
        print(f"\ncooldown verdict: NOT ASKED - the shortest predicted "
              f"cooldown is {shortest_cd:.0f}s and the silence window is "
              f"{args.silence_seconds:.0f}s, so no DROP was reachable. This "
              f"run measures the DECAYING onset only.")

    if args.json:
        payload = {"meta": {"driver": "cabinopt_cooldown_check.py",
                            "rows": args.rows, "half_life_s": args.half_life,
                            "silence_seconds": args.silence_seconds,
                            "poll_seconds": args.poll_seconds,
                            "warm_seconds": args.warm_seconds,
                            "seed": args.seed, "suffix": args.suffix,
                            "started_utc": datetime.datetime.utcfromtimestamp(run_t0)
                            .strftime("%Y-%m-%d %H:%M:%S"),
                            "elapsed_s": round(time.time() - run_t0, 1),
                            "key_reaches_live_cooldown": live},
                   "arms": rows_out,
                   "trace": {a.name: a.trace for a in arms}}
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2, default=str)
        print(f"json -> {args.json}")

    errors = sum(a.errors for a in arms)
    for arm in arms:
        arm.conn.close()
    if errors:
        print(f"\nERRORS: {errors}, first: "
              f"{next(a.first_error for a in arms if a.first_error)}",
              file=sys.stderr)
    # `live is None` means the cooldown question was not asked; the run
    # still succeeds if every arm reached DECAYING, which is what an
    # onset-only invocation came for.
    ok = (all(a.t_decaying is not None for a in arms) if live is None else live)
    sys.exit(0 if ok and not errors else 1)


if __name__ == "__main__":
    main()
