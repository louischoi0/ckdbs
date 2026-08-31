#!/usr/bin/env python3
"""RP7: kill -9 at every point of the cross-owner commit protocol, and what
a restart makes of it (`instructions/v2.4.0/2pc.md` §5, first bullet).

The claim under test is atomicity, stated as a count. One transaction
writes one row into each of two relations owned by two different cores.
The instance is killed at a named point of the protocol; it is restarted;
the two rows are counted. **The two counts must be equal** - that is
all-or-nothing - and which value they take is a property of the point:

    coordinator.before_prepare            0 - nobody was asked
    participant.prepare_logged_predurable 0 - a record nothing asked the
                                              device for promises nothing
    participant.prepare_durable_prereply  0 - a promise the coordinator
                                              never heard cannot be decided
    coordinator.prepared_predecide        0 - every promise made, no
                                              decision written anywhere
    coordinator.decided_presend           1 - the decision is durable and
                                              no participant heard it; the
                                              mount must publish both halves
    participant.decide_applied_preack     1 - one half committed, the other
                                              still prepared: the mount must
                                              reach the same answer twice,
                                              by redo and by resolution

The split is what makes the matrix a test rather than a sweep. In the four
expecting **0**, a count of 1 would be a transaction published that nobody
decided. In the two expecting **1**, a count of 0 would be lost data - a
decision the engine made durable and then failed to carry out. And in every
one of the six, unequal counts are a **torn** transaction, which is the
failure two-phase commit exists to make impossible.

**Both sides of the wire are threads of one process**, so a kill takes the
coordinator and its participants down together - `shipped_kill_recovery_probe.py`
made the same observation about the shipping path. That is why the matrix
is six points and not eight: "on each side" is a statement about which
core's code was executing, and the six names above cover both.

**The ordinal is per process, not per participant.** `name:2` fires on the
second *hit of that name in this process*, which is the second participant
only when the first attempt reached the point - and `cross_owner_txn`
retries the whole transaction up to forty times, so an attempt refused
before the protocol leaves the counter untouched and shifts which attempt
fires. Every ordinal cell expects the same count as its ordinal-1 sibling
for exactly that reason: no cell's verdict may depend on which attempt the
kill landed in. `txn_reply` records what the client last saw, which is what
tells the two apart after the fact.

The kill is deterministic, and it has to be: these windows are
microseconds wide, so an external killer racing them lands in one
essentially never. The process kills itself at the line, armed by
`KDS_CRASH_POINT` in its environment (`include/kds/base/crash_point.hpp`)
and by nothing else. A restart is never armed, so recovery runs on an
instance that cannot crash on purpose.

Three more kinds of cell ride the same harness, because each is a §5
bullet and each is answered by the same instrument:

  **fastpath.***  - D1's one-owner path, asserted by a kill that does not
  happen. The instance is armed at `coordinator.before_prepare` and given a
  transaction every one of whose relations its own core owns. It survives
  and answers `COMMIT`, so `PrepareAcrossOwners` was never entered and no
  prepare message was sent - §5's "count them: zero", proved by the
  strongest counter there is, a process that is still running. Two shapes:
  three cores with `placement = creating`, and `cores = 1`.

  **resolution.coordinator_stream_gone** - §5's "the coordinator's record is
  gone". The instance is killed at `coordinator.decided_presend`, the
  coordinator's WAL stream is deleted, and the mount must **refuse by name**.
  Aborting would durably contradict a coordinator that committed, and
  committing would invent a decision; the honest answer is neither, and it
  is the one behaviour here that is a refusal rather than an outcome. The
  cell records **which layer** refused, because that turned out to be the
  finding: analysis's own anchor check fires first, so the resolver's
  absent-stream arm is unreachable from a deleted file.

    python3 bench/txn_2pc_kill_matrix_probe.py [--build-dir build]
                                               [--cores 3] [--port 22800]

Exit 0 when every cell holds. A cell whose restart does not come back
inside `--mount-timeout` is reported as a **HANG**, which §5 calls a
blocking finding, and is never retried into a pass. `--repeat N` runs the
whole plan N times; every pass must hold, and the summary names any point
that reached the **asymmetric** state - the two participants dying in
different durable states, which no crash point can pin because they run on
separate cores and reach the line concurrently. It is sought, not assumed.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from multicore_benchmark import Conn, field  # noqa: E402
# XE2 (2026-08-31, against `eecda94` "Milestone KW: KWP/1 is the protocol
# the server speaks", which this worktree's branch merged in): the default
# `port` now speaks KWP/1, which installs a `result_sink`, and
# `CommandDispatcher::ShipStatement` refuses a shipped *read* to a session
# that has one (`command_dispatcher.cpp` sec.4256-4262, "a shipped read
# cannot answer a typed client" - `known-gaps.md`'s own documented gap).
# Every count this file takes is exactly such a read - `after` is a core-0
# session counting a row on a peer-owned relation - so counting over `Conn`
# now refuses instead of answering. The documented route around it is the
# **newline debug surface** the refusal message itself names
# (`debug_text_port`, `protocol.md` sec.12): no result sink is installed
# over it, so the ship answers as rendered text exactly as it always did.
# `TextConnection` is `ckdbs_cli.py`'s client for that surface.
from ckdbs_cli import TextConnection  # noqa: E402

# name -> rows expected in *each* relation after the restart.
MATRIX = [
    ('coordinator.before_prepare', 0),
    ('participant.prepare_logged_predurable', 0),
    ('participant.prepare_logged_predurable:2', 0),
    ('participant.prepare_durable_prereply', 0),
    ('participant.prepare_durable_prereply:2', 0),
    ('coordinator.prepared_predecide', 0),
    ('coordinator.decided_presend', 1),
    # The two points that can leave the transaction **partly published**,
    # which is the state in-doubt resolution exists to repair and the only
    # one in which a wrong recovery tears a transaction instead of rolling
    # it back. When it lands asymmetrically, one participant's half is
    # durably committed in its own stream while the other is still merely
    # prepared, and the restart must reach 1 by two different routes - redo
    # for the committed half, resolution against the coordinator's stream
    # for the prepared one. Whether it lands that way is the race `mixed`
    # records; the expected count is 1 either way, which is the point.
    ('participant.decide_applied_preack', 1),
    ('participant.decide_applied_preack:2', 1),
    # XE2 (`instructions/v2.7.1/workorder-xd.md`): the window XE1 opened.
    # Under D2 the ack now precedes durability - the participant's COMMIT
    # is appended and the coordinator is told before the record is synced,
    # riding the next drain instead of a park. A crash here leaves the same
    # shape `decide_applied_preack` does: a durable `TXN_PREPARE` with no
    # decision in *this* stream (the append is not yet durable, only
    # written), resolved from the coordinator's stream, which has held the
    # decision as durable since before the decide was even sent
    # (`cross-owner-txn.md` sec2c's fourth outcome). Expected count: 1,
    # same as its sibling - this is what "the wait bought no durability
    # promise" means as a testable claim, not just an argued one.
    ('participant.decide_acked_predurable', 1),
    ('participant.decide_acked_predurable:2', 1),
]

TAG = 'kill'


def write_conf(workdir, port, cores, placement='rotate'):
    conf = os.path.join(workdir, 's.conf')
    with open(conf, 'w') as f:
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\n"
                f"port = {port}\ncores = {cores}\n"
                # `rotate` never places on core 0 (core_placement.hpp), so
                # it is what puts two relations on two peers and leaves the
                # client's core free to be the coordinator. `creating` is
                # the opposite arrangement and the one the fast-path cells
                # need: every relation owned by the core the client is on.
                # `peer_listeners` is legal only beside `rotate` and only
                # above one core - the server refuses both other pairings at
                # startup, by name.
                f"placement = {placement}\npeer_listeners = "
                f"{'on' if placement == 'rotate' and cores > 1 else 'off'}\n"
                # `info` so the resolver's own verdict line is readable: a
                # cell that only counted rows would be inferring which
                # mechanism produced them.
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = info\n"
                # The counting route (see the import comment above):
                # confirmed single-listener, always core 0, never the
                # `peer_listeners`-style multi-core `SO_REUSEPORT` set, so a
                # client aimed here needs no `session_on_core` retry.
                f"debug_text_port = {debug_text_port(port)}\n")
    return conf


def debug_text_port(port):
    """The newline-surface port paired with a cell's KWP `port` - one fixed
    offset, safe because every cell in this probe tears its server down
    before the next reuses the same `port` (`stop_proc`'s own docstring)."""
    return port + 1


class TextConn(TextConnection):
    """`TextConnection` plus the `.cmd()` name this file's helpers
    (`tagged_rows`, the `writable_after` probe) already call on a `Conn`."""

    def cmd(self, line):
        return self.send_command(line)


def after_conn(args):
    """The counting connection every restart reads through: the newline
    surface, always core 0 (confirmed - `debug_text_port` is one listener,
    never `peer_listeners`'s multi-core `SO_REUSEPORT` set), so no
    `session_on_core` retry is needed the way the KWP setup connection
    needs one."""
    return TextConn('127.0.0.1', debug_text_port(args.port))


def stop_proc(proc):
    """Leave no server holding the port. Every cell in this probe binds the
    *same* port, so an instance a cell abandoned would be met by the next
    cell's `wait_up` - and it is an **armed** instance, so the cell after it
    would be reading a verdict off the wrong process."""
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)


def start(conf, workdir, server, arm=None):
    env = dict(os.environ)
    env.pop('KDS_CRASH_POINT', None)
    if arm:
        env['KDS_CRASH_POINT'] = arm
    err = open(os.path.join(workdir, 's.stderr'), 'a')
    return subprocess.Popen([server, '--config', conf], stdout=err,
                            stderr=subprocess.STDOUT, env=env)


def wait_up(port, proc, deadline_s):
    """A port that opens, or the reason it did not. Never an unbounded wait:
    a mount that does not finish is the hang §5 calls a blocking finding."""
    end = time.time() + deadline_s
    while time.time() < end:
        if proc.poll() is not None:
            return False, f'exited rc={proc.returncode} before listening'
        try:
            c = Conn(port)
            c.close()
            return True, None
        except OSError:
            time.sleep(0.05)
    return False, f'no listener within {deadline_s}s'


def session_on_core(port, core, spare, tries=128):
    for _ in range(tries):
        c = Conn(port)
        if field(c.cmd('SHOW META'), 'core') == core:
            return c
        spare.append(c)
    raise RuntimeError(f'no session landed on core {core}')


def indoubt_health(port, owner_cores, spare):
    """H-XE3's falsifier, read rather than assumed: `shipped_enrolment_expiries`
    (a coordinator abandoned an enrolled participant) and
    `txn_in_doubt_unresolved` (a row still held for a coordinator this
    core can no longer ask) must be 0 on every participant core after the
    restart. Both fields are local to whichever core answers `SHOW META`,
    not shipped, so an ordinary `session_on_core` landing (no relation
    read, so no KWP refusal) answers them directly. `txn_in_doubt_unresolved`
    is printed only when non-zero (`command_dispatcher.cpp`'s "absent
    rather than zeroed" rule for that block) - its absence from the reply
    *is* the 0 reading, not a missing one."""
    out = {}
    for core in sorted(set(owner_cores)):
        c = session_on_core(port, core, spare)
        meta = c.cmd('SHOW META')
        expiries = None
        for tok in meta.split():
            if tok.startswith('shipped_enrolment_expiries='):
                expiries = int(tok.split('=', 1)[1])
        unresolved = 0
        for tok in meta.split():
            if tok.startswith('txn_in_doubt_unresolved='):
                unresolved = int(tok.split('=', 1)[1])
        out[str(core)] = {'shipped_enrolment_expiries': expiries,
                          'txn_in_doubt_unresolved': unresolved}
    return out


def cross_owner_txn(conn, a, b, tag, tries=40):
    """One cross-owner transaction, retried as a unit. A retryable refusal
    inside an explicit transaction poisons it (the R6-8 review's rule), so
    the retry is a fresh BEGIN and not a re-send of the statement."""
    last = ('none', 'not attempted')
    for _ in range(tries):
        r = conn.cmd('BEGIN')
        if r.startswith('ERR'):
            last = ('BEGIN', r)
            time.sleep(0.05)
            continue
        failed = None
        for s in (f"INSERT INTO {a} VALUES ('{tag}', 1)",
                  f"INSERT INTO {b} VALUES ('{tag}', 2)", 'COMMIT'):
            r = conn.cmd(s)
            if r.startswith('ERR'):
                failed = (s, r)
                break
        if failed is None:
            return ('COMMIT', r)
        last = failed
        conn.cmd('ROLLBACK')
        time.sleep(0.05)
    return last


def tagged_rows(conn, table, tag):
    reply = conn.cmd(f"SELECT COUNT(*) FROM {table} WHERE tag = '{tag}'")
    if reply.startswith('ERR'):
        return None, reply[:200]
    for part in reversed(reply.replace('\\n', '\n').split('\n')):
        if part.strip().isdigit():
            return int(part.strip()), reply[:200]
    return None, reply[:200]


def arm_and_kill(arm, args, out):
    """The first half every killed cell shares: a fresh armed instance, two
    peer-owned relations with a seed row each, and the cross-owner
    transaction that walks into the armed point. Returns `(workdir, conf,
    server)`; `out` carries the finding when there is one."""
    workdir = os.path.join(args.workdir, arm.replace(':', '-'))
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = write_conf(workdir, args.port, args.cores)
    server = os.path.join(ROOT, args.build_dir, 'kds_server')
    spare = []
    proc = start(conf, workdir, server, arm=arm)
    try:
        up, why = wait_up(args.port, proc, args.mount_timeout)
        if not up:
            out['error'] = f'first mount: {why}'
            return None
        c0 = session_on_core(args.port, 0, spare)
        for t in ('t0', 't1'):
            created = c0.cmd(f'CREATE TABLE {t} (id int64, tag varchar, n int64) BTREE')
            if created.startswith('ERR'):
                out['error'] = f'CREATE {t}: {created[:200]}'
                return None
        owners = {t: field(c0.cmd(f'DESCRIBE {t}'), 'owner_core') for t in ('t0', 't1')}
        out['owners'] = owners
        if owners['t0'] == owners['t1'] or 0 in owners.values():
            # Not a finding about the protocol: the cell cannot pose the
            # question, so it says so rather than passing vacuously.
            out['vacuous'] = f'relations did not land on two peer cores: {owners}'
            return None

        # A row per relation written *before* the transaction, so the
        # restart has something to find even when the killed transaction
        # leaves nothing - a relation that reads 0 because it was lost
        # whole is a different failure from one that rolled back.
        for t in ('t0', 't1'):
            for _ in range(40):
                r = c0.cmd(f"INSERT INTO {t} VALUES ('before', 0)")
                if not (r.startswith('ERR') and 'retryable=1' in r):
                    break
                time.sleep(0.05)
            if r.startswith('ERR'):
                out['error'] = f'seed insert into {t}: {r[:200]}'
                return None

        out['txn_reply'] = cross_owner_txn(c0, 't0', 't1', TAG)
    except (ConnectionError, OSError) as e:
        # The expected shape for every armed point: the process died under
        # the client, which is what a kill -9 looks like from a socket.
        out['txn_reply'] = ('connection', f'{type(e).__name__}: {e}')
    finally:
        for c in spare:
            c.close()
        # Every `return None` above records its finding first, and each one
        # leaves an armed instance still listening: without this the next
        # cell binds a port this one still holds, or worse connects to it.
        if 'error' in out or 'vacuous' in out:
            stop_proc(proc)

    try:
        proc.wait(timeout=args.mount_timeout)
    except subprocess.TimeoutExpired:
        out['error'] = 'the armed instance did not die at its point'
        proc.kill()
        proc.wait(timeout=10)
        return None
    out['killed_rc'] = proc.returncode
    stderr = open(os.path.join(workdir, 's.stderr')).read()
    point_name = arm.split(':')[0]
    out['crash_line'] = f"crash point '{point_name}' fired" in stderr
    if proc.returncode != -9:
        out['error'] = f'died rc={proc.returncode}, not SIGKILL'
        return None
    if not out['crash_line']:
        out['error'] = 'the process died without reaching its crash point'
        return None
    return workdir, conf, server


def run_cell(arm, expected, args):
    """One point of the matrix, from a fresh instance to a counted restart."""
    out = {'point': arm, 'expected_rows': expected, 'kind': 'kill'}
    killed = arm_and_kill(arm, args, out)
    if killed is None:
        return out
    workdir, conf, server = killed
    spare = []

    # ---- The restart, unarmed --------------------------------------------
    t_mount = time.time()
    proc = start(conf, workdir, server, arm=None)
    try:
        up, why = wait_up(args.port, proc, args.mount_timeout)
        out['mount_s'] = round(time.time() - t_mount, 2)
        if not up:
            out['hang'] = f'restart: {why}'
            return out
        after = after_conn(args)
        spare.append(after)  # closed by the `finally` below either way
        counts, raws = {}, {}
        for t in ('t0', 't1'):
            counts[t], raws[t] = tagged_rows(after, t, TAG)
        out['counts'] = counts
        out['count_replies'] = raws
        seed = {}
        for t in ('t0', 't1'):
            seed[t], _ = tagged_rows(after, t, 'before')
        out['seed_counts'] = seed
        # H-XE3's falsifier, checked rather than assumed: both counters
        # must read 0 on every participant core after the restart.
        out['indoubt_health'] = indoubt_health(args.port, out.get('owners', {}).values(),
                                               spare)

        # **Which mechanism produced those rows**, read rather than
        # inferred: the resolver logs one line per prepared transaction it
        # resolved, naming the coordinator whose stream decided it. A cell
        # that counted rows alone could not tell a resolution from a redo
        # that never needed one.
        try:
            log = open(os.path.join(workdir, 's.log')).read()
            out['resolutions'] = [ln.split('recovery]')[-1].strip()
                                  for ln in log.splitlines() if 'resolves to' in ln]
        except OSError:
            out['resolutions'] = []
        # **Did the two participants die in *different* durable states?**
        # Exactly one resolution line means one core had a prepare to
        # resolve and the other did not: at `decide_applied_preack` that is
        # one half redone from its own commit record and one resolved
        # against the coordinator's stream; at the prepare points it is one
        # promise durable and one not. Either way it is the asymmetric case,
        # and it is where a wrong recovery *tears* a transaction rather than
        # rolling it back - the two counts must still agree.
        #
        # **No crash point can pin it.** The participants run on separate
        # cores and reach these lines concurrently, so "A past it and B not"
        # is a race, not a position. Recorded rather than required: the
        # cell's verdict is the count either way, and `--repeat` is how the
        # state is sought rather than assumed.
        out['mixed'] = 0 < len(out['resolutions']) < 2

        # Nothing left half-held: a relation whose owner is still holding an
        # in-doubt transaction's rows would refuse this under D5's ceiling.
        writable = {}
        for t in ('t0', 't1'):
            for _ in range(60):
                r = after.cmd(f"INSERT INTO {t} VALUES ('after', -1)")
                if not (r.startswith('ERR') and 'retryable=1' in r):
                    break
                time.sleep(0.05)
            writable[t] = (not r.startswith('ERR'), r[:160])
        out['writable_after'] = writable
        after.cmd('STOP')
        try:
            proc.wait(timeout=args.mount_timeout)
        except subprocess.TimeoutExpired:
            out['hang'] = 'STOP did not settle'
            proc.kill()
            proc.wait(timeout=10)
    except (ConnectionError, OSError) as e:
        # A restarted instance that dies while being counted is **this
        # cell's** finding, not the run's. Without this the exception left
        # `run_cell` and took the other nine cells with it, which is the one
        # way a matrix can report less than it knows.
        out['error'] = f'the restarted instance died under the count: {type(e).__name__}: {e}'
    finally:
        for c in spare:
            c.close()
        stop_proc(proc)
    return out


def run_fastpath_cell(name, cores, args):
    """D1's one-owner path, asserted by a kill that does not happen.

    `placement = creating` puts every relation on the core that created it,
    so a client on that core writes both of them locally, enrols no
    participant, and `HandleCommit`'s fork never reaches
    `PrepareAcrossOwners`. The instance is armed at
    `coordinator.before_prepare` throughout: surviving *is* the assertion
    that zero prepare messages were sent."""
    out = {'point': name, 'kind': 'fastpath', 'cores': cores}
    workdir = os.path.join(args.workdir, name)
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = write_conf(workdir, args.port, cores, placement='creating')
    server = os.path.join(ROOT, args.build_dir, 'kds_server')
    spare = []
    proc = start(conf, workdir, server, arm='coordinator.before_prepare')
    try:
        up, why = wait_up(args.port, proc, args.mount_timeout)
        if not up:
            out['error'] = f'mount: {why}'
            return out
        c0 = session_on_core(args.port, 0, spare)
        for t in ('t0', 't1'):
            created = c0.cmd(f'CREATE TABLE {t} (id int64, tag varchar, n int64) BTREE')
            if created.startswith('ERR'):
                out['error'] = f'CREATE {t}: {created[:200]}'
                return out
        owners = {t: field(c0.cmd(f'DESCRIBE {t}'), 'owner_core') for t in ('t0', 't1')}
        out['owners'] = owners
        if set(owners.values()) != {0}:
            out['vacuous'] = f'the client core does not own both relations: {owners}'
            return out
        out['txn_reply'] = cross_owner_txn(c0, 't0', 't1', TAG)
        counts = {}
        for t in ('t0', 't1'):
            counts[t], _ = tagged_rows(c0, t, TAG)
        out['counts'] = counts
        out['alive'] = proc.poll() is None
        # STOP over the newline surface, not `c0` (KWP): a KWP `STOP` is
        # known-unreachable (the import comment's milestone,
        # `known-gaps.md`'s "STOP is reachable only on the debug port"),
        # and taking that as a `ConnectionError` here would clobber a
        # `txn_reply`/`counts` this try block already recorded correctly -
        # exactly the false ENTERED this line used to produce.
        try:
            stopper = after_conn(args)
            stopper.cmd('STOP')
            stopper.close()
        except (ConnectionError, OSError):
            pass
        try:
            proc.wait(timeout=args.mount_timeout)
        except subprocess.TimeoutExpired:
            out['hang'] = 'STOP did not settle'
    except (ConnectionError, OSError) as e:
        # The failure this cell exists to catch: the one-owner path reached
        # the coordinator's prepare and the armed point killed it.
        out['alive'] = False
        out['txn_reply'] = ('connection', f'{type(e).__name__}: {e}')
    finally:
        for c in spare:
            c.close()
        stop_proc(proc)
    stderr = open(os.path.join(workdir, 's.stderr')).read()
    out['crash_line'] = "crash point 'coordinator.before_prepare' fired" in stderr
    return out


def run_checkpoint_gap_cell(args):
    """XE2's one targeted cell beyond the matrix's shape: the same
    `participant.decide_acked_predurable` point, with the checkpointer
    forced to cycle continuously around it, testing the spec's checkpoint
    argument (`cross-owner-txn.md` sec2c) rather than assuming it: a
    checkpoint's `CHECKPOINT_END` necessarily carries whatever was appended
    before it, because `Checkpointer::Complete()` durability-waits on it
    through `WalManager::EnsureDurable`, which is the *same* `Sync()` an
    ordinary drain calls (`wal/manager.cpp:153` vs `:225-231`) - not a
    second, weaker mechanism.

    **What this cell can and cannot show, said plainly rather than
    implied.** The literal window the order names - a checkpoint
    interposed between the ack and the drain, before the kill - is empty
    by construction on this reactor, confirmed by source read rather than
    assumed: under `kAtAppend` the append
    (`WalManager::Commit`'s `++pending_group_commits_`, a plain increment,
    no I/O, `wal/manager.cpp:203-205`) and the crash
    (`shipped_statement_executor.cpp:889-900`, ack then immediately
    `CrashPointHit`) run inside one `CoroTask::Poll()` with no `co_await`
    between them - `on_done_` fires synchronously in the same `Poll()`
    that ran the coroutine to completion (`include/kds/sched/coro.hpp`,
    the `if (!handle_.done()) ... if (on_done_) on_done_(...)` sequence).
    Nothing - ordinary drain or checkpoint alike - can run between two
    calls in the same synchronous stack frame; a fuzzy checkpoint is
    furthermore multi-tick by design (`checkpointer.hpp`: "spreads across
    reactor iterations"), so it could not complete inside a zero-width gap
    even if one existed. So this cell does not, and structurally cannot,
    race a checkpoint into that exact window - no facility XE1 added makes
    that constructible without a second crash point the order did not ask
    for. What it *does* test, empirically: with the checkpointer actively
    cycling throughout the whole scenario (`checkpoint_interval_ms` short
    enough for several cycles to land on both peer cores before the
    targeted transaction even begins, confirmed from the log), the outcome
    at this crash point is unchanged - still COMMIT on both relations,
    same as the plain `decide_acked_predurable` cell with no checkpointer
    running at all. That the two agree is the corroborating evidence the
    source argument predicts; it is not a substitute for the source
    argument, which is what actually proves the gap is safe.
    """
    out = {'point': 'checkpoint.decide_acked_predurable_gap', 'kind': 'kill',
           'expected_rows': 1}
    arm = 'participant.decide_acked_predurable'
    workdir = os.path.join(args.workdir, 'checkpoint-gap')
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = os.path.join(workdir, 's.conf')
    checkpoint_interval_ms = 20
    pre_txn_delay_s = 0.5
    with open(conf, 'w') as f:
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\n"
                f"port = {args.port}\ncores = {args.cores}\n"
                f"placement = rotate\npeer_listeners = on\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = debug\n"
                f"checkpoint_interval_ms = {checkpoint_interval_ms}\n"
                f"debug_text_port = {debug_text_port(args.port)}\n")
    out['checkpoint_interval_ms'] = checkpoint_interval_ms
    server = os.path.join(ROOT, args.build_dir, 'kds_server')
    spare = []
    proc = start(conf, workdir, server, arm=arm)
    try:
        up, why = wait_up(args.port, proc, args.mount_timeout)
        if not up:
            out['error'] = f'first mount: {why}'
            return out
        c0 = session_on_core(args.port, 0, spare)
        for t in ('t0', 't1'):
            created = c0.cmd(f'CREATE TABLE {t} (id int64, tag varchar, n int64) BTREE')
            if created.startswith('ERR'):
                out['error'] = f'CREATE {t}: {created[:200]}'
                return out
        owners = {t: field(c0.cmd(f'DESCRIBE {t}'), 'owner_core') for t in ('t0', 't1')}
        out['owners'] = owners
        if owners['t0'] == owners['t1'] or 0 in owners.values():
            out['vacuous'] = f'relations did not land on two peer cores: {owners}'
            return out
        for t in ('t0', 't1'):
            for _ in range(40):
                r = c0.cmd(f"INSERT INTO {t} VALUES ('before', 0)")
                if not (r.startswith('ERR') and 'retryable=1' in r):
                    break
                time.sleep(0.05)
            if r.startswith('ERR'):
                out['error'] = f'seed insert into {t}: {r[:200]}'
                return out
        # Several checkpoint cycles on both peer cores before the targeted
        # transaction even begins - not to place one inside the (empty,
        # per the docstring) gap, but so the checkpointer is genuinely live
        # and active around the crash rather than merely configured.
        time.sleep(pre_txn_delay_s)
        out['pre_txn_delay_s'] = pre_txn_delay_s
        out['txn_reply'] = cross_owner_txn(c0, 't0', 't1', TAG)
    except (ConnectionError, OSError) as e:
        out['txn_reply'] = ('connection', f'{type(e).__name__}: {e}')
    finally:
        for c in spare:
            c.close()
        if 'error' in out or 'vacuous' in out:
            stop_proc(proc)
    if 'error' in out or 'vacuous' in out:
        return out

    try:
        proc.wait(timeout=args.mount_timeout)
    except subprocess.TimeoutExpired:
        out['error'] = 'the armed instance did not die at its point'
        proc.kill()
        proc.wait(timeout=10)
        return out
    out['killed_rc'] = proc.returncode
    stderr = open(os.path.join(workdir, 's.stderr')).read()
    out['crash_line'] = f"crash point '{arm}' fired" in stderr
    if proc.returncode != -9:
        out['error'] = f'died rc={proc.returncode}, not SIGKILL'
        return out
    if not out['crash_line']:
        out['error'] = 'the process died without reaching its crash point'
        return out

    # The empirical half: how many checkpoints this core's checkpointer
    # completed and published *before* the kill. Preserved to a separate
    # file first, since the restart reopens the same log path.
    prelog_path = os.path.join(workdir, 's.log')
    prelog = open(prelog_path).read() if os.path.exists(prelog_path) else ''
    if os.path.exists(prelog_path):
        shutil.copy(prelog_path, os.path.join(workdir, 's.log.prerestart'))
    anchors_before_kill = [ln for ln in prelog.splitlines() if 'anchor published' in ln]
    out['checkpoints_before_kill'] = len(anchors_before_kill)
    out['checkpoints_before_kill_sample'] = anchors_before_kill[:3]

    # ---- The restart, unarmed - exactly `run_cell`'s second half --------
    t_mount = time.time()
    proc = start(conf, workdir, server, arm=None)
    spare = []
    try:
        up, why = wait_up(args.port, proc, args.mount_timeout)
        out['mount_s'] = round(time.time() - t_mount, 2)
        if not up:
            out['hang'] = f'restart: {why}'
            return out
        after = after_conn(args)
        spare.append(after)  # closed by the `finally` below either way
        counts, raws = {}, {}
        for t in ('t0', 't1'):
            counts[t], raws[t] = tagged_rows(after, t, TAG)
        out['counts'] = counts
        out['count_replies'] = raws
        seed = {}
        for t in ('t0', 't1'):
            seed[t], _ = tagged_rows(after, t, 'before')
        out['seed_counts'] = seed
        out['indoubt_health'] = indoubt_health(args.port, out.get('owners', {}).values(),
                                               spare)
        writable = {}
        for t in ('t0', 't1'):
            for _ in range(60):
                r = after.cmd(f"INSERT INTO {t} VALUES ('after', -1)")
                if not (r.startswith('ERR') and 'retryable=1' in r):
                    break
                time.sleep(0.05)
            writable[t] = (not r.startswith('ERR'), r[:160])
        out['writable_after'] = writable
        after.cmd('STOP')
        try:
            proc.wait(timeout=args.mount_timeout)
        except subprocess.TimeoutExpired:
            out['hang'] = 'STOP did not settle'
            proc.kill()
            proc.wait(timeout=10)
    except (ConnectionError, OSError) as e:
        out['error'] = f'the restarted instance died under the count: {type(e).__name__}: {e}'
    finally:
        for c in spare:
            c.close()
        stop_proc(proc)
    return out


def run_stream_gone_cell(args):
    """§5's "the coordinator's record is gone", at the mount.

    Killed at `coordinator.decided_presend` - so two participants hold
    durable prepares and the only copy of the decision is in core 0's
    stream - and then that stream is deleted. Neither outcome is readable
    from what is left, and the mount must say so rather than pick one."""
    out = {'point': 'resolution.coordinator_stream_gone', 'kind': 'resolution'}
    killed = arm_and_kill('coordinator.decided_presend', args, out)
    if killed is None:
        return out
    workdir, conf, server = killed
    coordinator_stream = os.path.join(workdir, 's.db.wal', 'wal-0-0.log')
    if not os.path.exists(coordinator_stream):
        out['error'] = f'no coordinator stream at {coordinator_stream}'
        return out
    os.remove(coordinator_stream)
    out['removed'] = os.path.basename(coordinator_stream)

    proc = start(conf, workdir, server, arm=None)
    try:
        up, why = wait_up(args.port, proc, args.mount_timeout)
        out['mounted'] = up
        out['why'] = why
    finally:
        stop_proc(proc)
    stderr = open(os.path.join(workdir, 's.stderr')).read()
    # **Which layer refused is the finding, not a detail.** Deleting the
    # stream whole is caught by core 0's own analysis - its stream now ends
    # below the durable point its checkpoint anchor was published with -
    # before `CoordinatorStreamResolver` is ever asked. So the resolver's
    # own absent-stream refusal is unreachable by this route, and stays
    # proved where it is provable: `prepared_recovery_test.cpp`'s
    # `AnAbsentCoordinatorStreamRefusesRatherThanAborting`. What this cell
    # establishes is the §5 property itself - the decision cannot be read,
    # and the instance says so instead of picking one.
    if 'cross-owner recovery' in stderr:
        out['refused_by'] = 'resolver'
    elif 'startup failed: recovery' in stderr:
        out['refused_by'] = 'analysis (anchor check, one layer above the resolver)'
    else:
        out['refused_by'] = None
    out['stderr_tail'] = stderr[-400:]
    return out


def verdict(cell):
    # The three that mean the same thing whatever the cell was asking, taken
    # once above the fork: a cell that could not pose its question, one that
    # never came back, and one that broke. Hoisted rather than repeated -
    # the resolution arm had no HANG check while it carried its own copy of
    # the other two, so a mount that hung there reported UNNAMED.
    if 'vacuous' in cell:
        return 'VACUOUS'
    if 'hang' in cell:
        return 'HANG'
    if 'error' in cell:
        return 'ERROR'
    kind = cell.get('kind')
    if kind == 'fastpath':
        if cell.get('crash_line') or not cell.get('alive'):
            # The armed point fired, so the one-owner commit entered the
            # protocol: D1's gate, failing.
            return 'ENTERED'
        if cell.get('txn_reply', ('', ''))[0] != 'COMMIT':
            return 'ERROR'
        if (cell.get('counts') or {}) != {'t0': 1, 't1': 1}:
            return 'WRONG'
        return 'PASS'
    if kind == 'resolution':
        if cell.get('mounted'):
            # It came up, which means it resolved a decision it could not
            # read - the guess §5 forbids.
            return 'GUESSED'
        if not cell.get('refused_by'):
            # It died without saying why, which is not a refusal.
            return 'UNNAMED'
        return 'PASS'
    return kill_verdict(cell)


def kill_verdict(cell):
    counts = cell.get('counts') or {}
    if None in counts.values() or len(counts) != 2:
        return 'ERROR'
    a, b = counts['t0'], counts['t1']
    if a != b:
        return 'TORN'          # the atomicity failure this probe exists for
    if a != cell['expected_rows']:
        return 'WRONG'         # atomic, but the wrong outcome for the point
    # **Both relations**, because the counts above cannot tell a rolled-back
    # transaction from a relation lost whole: a `t1` that came back empty
    # reads as the expected 0 on every point but the last. The seed row is
    # what separates them, so it is asserted on each side.
    seed = cell.get('seed_counts') or {}
    if any(seed.get(t) != 1 for t in ('t0', 't1')):
        return 'ERROR'
    if not all(ok for ok, _ in (cell.get('writable_after') or {}).values()):
        return 'STUCK'
    # H-XE3's falsifier: an abandoned enrolment or a row nothing at runtime
    # can finish, on any checked core.
    for health in (cell.get('indoubt_health') or {}).values():
        if health.get('shipped_enrolment_expiries') not in (0, None):
            return 'INDOUBT'
        if health.get('txn_in_doubt_unresolved') != 0:
            return 'INDOUBT'
    return 'PASS'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', default='build')
    ap.add_argument('--cores', type=int, default=3)
    ap.add_argument('--port', type=int, default=22800)
    ap.add_argument('--mount-timeout', type=float, default=90.0)
    ap.add_argument('--workdir', default='/tmp/kds-2pc-kill-matrix')
    ap.add_argument('--json', default=None)
    ap.add_argument('--only', default=None, help='run one point by name')
    ap.add_argument('--repeat', type=int, default=1,
                    help='run the plan N times; every pass must hold, and a '
                         'state that is a race (see `mixed`) is sought rather '
                         'than assumed')
    args = ap.parse_args()

    plan = [(arm, lambda a=arm, e=exp: run_cell(a, e, args)) for arm, exp in MATRIX]
    plan.append(('resolution.coordinator_stream_gone', lambda: run_stream_gone_cell(args)))
    plan.append(('checkpoint.decide_acked_predurable_gap',
                 lambda: run_checkpoint_gap_cell(args)))
    plan += [('fastpath.local_only', lambda: run_fastpath_cell('fastpath.local_only',
                                                               args.cores, args)),
             ('fastpath.cores1', lambda: run_fastpath_cell('fastpath.cores1', 1, args))]
    if args.only is not None:
        plan = [(n, f) for n, f in plan if n.startswith(args.only)]
        if not plan:
            # A mistyped `--only` selected nothing, and "0 of 0 cells" must
            # not exit 0: an empty gate is not a passed one.
            print(f'--only {args.only!r} matched no cell')
            return 1
    results = []
    failed = 0
    for attempt in range(max(1, args.repeat)):
        for name, run in plan:
            cell = run()
            cell['verdict'] = verdict(cell)
            cell['pass_no'] = attempt + 1
            results.append(cell)
            label = name if args.repeat == 1 else f'{name}  (pass {attempt + 1})'
            print(f"{cell['verdict']:8s} {label}")
            for key in ('owners', 'txn_reply', 'killed_rc', 'counts', 'seed_counts',
                        'indoubt_health', 'mount_s', 'resolutions', 'mixed',
                        'writable_after', 'alive', 'crash_line', 'removed', 'mounted',
                        'why', 'refused_by', 'checkpoint_interval_ms', 'pre_txn_delay_s',
                        'checkpoints_before_kill', 'checkpoints_before_kill_sample',
                        'error', 'hang', 'vacuous'):
                if key in cell:
                    print(f'    {key} = {cell[key]}')
            if cell['verdict'] != 'PASS':
                failed += 1
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(results, f, indent=2)
    print()
    mixed = sorted({c['point'] for c in results if c.get('mixed')})
    if mixed:
        # Named, because a pass that reached it tested what a symmetric
        # one cannot: two participants in different durable states, which
        # is where a wrong recovery tears rather than rolls back.
        print(f'asymmetric participant states reached at: {", ".join(mixed)}')
    print(f'{len(results) - failed}/{len(results)} cells PASS')
    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
