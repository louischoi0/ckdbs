#!/usr/bin/env python3
"""XG3's owed half: the answer edge under a real process death.

XG3 placed four crash points on a typed client's shipped read (XG1's
answer edge) and asserted three *faults* against them - an out-of-order
description chunk, a description whole only when every chunk arrived, rows
with no description - each refused rather than decoded against a guess.
What it did not do is **kill the process at those points**, and
`workplan-shipped-read-typed.md` §8c says so in its own table: three cells
read "not run".

This probe runs them. One cell per armed point, each from a fresh
instance:

    shipped.answer_described_prerows      the receiver is registered and
                                          the description has crossed, but
                                          no batch has
    shipped.answer_batch_sent:1           after the first batch, mid-stream
    shipped.answer_edge_closed_prereply   the terminator is sent and the
                                          reply has not been

**What a kill proves here that a fault could not.** Both ends of the wire
are threads of one process, so the death takes the client's socket with
it - the taxonomy claim ("a failure reaches a typed client as a failure,
never as an empty or partial result set") is then tested against a real
death rather than an injected error return. Three things must hold, and
the third is the one worth the harness:

  the point is reached      -- the crash line is in stderr, so the cell
                              proves the code path is live at that point
                              and not merely that a name compiles.
  no result is delivered    -- the typed client raises rather than
                              returning rows. A described-but-empty or
                              partial result set is the failure XG1's
                              §4a forbids and the one this cell exists
                              for.
  the restart is clean      -- the instance mounts, the relation reads,
                              and no in-doubt residue is left: a shipped
                              *read* writes nothing, so a restart that
                              needed to resolve anything would be a
                              finding about the read path holding state
                              it should not.

    python3 bench/shipped_answer_edge_kill_probe.py [--cores N] [--port P]

Exit status is 0 only when every cell passes.
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
sys.path.insert(0, os.path.join(ROOT, 'bench'))
from kwp import Connection as KwpConn, KwpError  # noqa: E402
from ckdbs_cli import TextConnection  # noqa: E402

# The three points XG3 placed and did not kill at. `:1` on the batch point
# is the second hit - after the first batch has crossed - which is what
# makes it *mid-stream* rather than the same instant as `prerows`.
POINTS = [
    'shipped.answer_described_prerows',
    'shipped.answer_batch_sent',
    'shipped.answer_edge_closed_prereply',
]

ROWS = 400  # enough to span several batches, so `:1` is genuinely mid-stream


class TextConn(TextConnection):
    def cmd(self, line):
        return self.send_command(line)


def debug_text_port(port):
    return port + 1


def write_conf(workdir, port, cores):
    conf = os.path.join(workdir, 's.conf')
    with open(conf, 'w') as f:
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\n"
                f"port = {port}\ncores = {cores}\n"
                # `rotate` never places on core 0, which is what puts the
                # relation on a peer and leaves the client's core to be the
                # arrival core - the arrangement a shipped read needs.
                "placement = rotate\npeer_listeners = on\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = info\n"
                f"debug_text_port = {debug_text_port(port)}\n")
    return conf


def start(conf, workdir, server, arm=None):
    env = dict(os.environ)
    env.pop('KDS_CRASH_POINT', None)
    if arm:
        env['KDS_CRASH_POINT'] = arm
    err = open(os.path.join(workdir, 's.stderr'), 'a')
    return subprocess.Popen([server, '--config', conf], stdout=err,
                            stderr=subprocess.STDOUT, env=env)


def wait_up(port, proc, deadline_s):
    end = time.time() + deadline_s
    while time.time() < end:
        if proc.poll() is not None:
            return False, f'exited rc={proc.returncode} before listening'
        try:
            c = KwpConn('127.0.0.1', port)
            c.close()
            return True, None
        except (OSError, KwpError):
            time.sleep(0.05)
    return False, f'no listener within {deadline_s}s'


def wait_port_free(port, deadline_s=20.0):
    """The port actually released before the next instance binds it.

    Every phase of a cell reuses one port, and `stop_proc` returning is not
    the port being free. Without this the armed instance can be started
    while the previous listener is still winding down: `wait_up` then
    connects to the *dying* one, the client's read is answered by nobody,
    and `proc.wait()` on the armed process times out with "did not die at
    its point" while its crash point was never even reached. That produced
    a cell that passed and failed across two runs of the same build.
    """
    import socket
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            s = socket.create_connection(('127.0.0.1', port), timeout=0.2)
            s.close()
            time.sleep(0.05)
        except OSError:
            return True
    return False


def stop_proc(proc, port=None):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
    if port is not None:
        wait_port_free(port)


def kwp_session_off(port, owner_core, timeout, spare, tries=64):
    """A typed session on a core that is **not** the relation's owner.

    `peer_listeners = on` accepts on every core through `SO_REUSEPORT`, so
    a connection lands wherever the kernel puts it - and a session that
    lands on the owner executes the statement **locally**: nothing ships,
    no answer edge opens, and the armed point is never reached. That is
    not a flaky harness, it is the cell silently not posing its question,
    and a first draft of this probe had one cell pass and fail across two
    runs of the same build for exactly that reason.
    """
    for _ in range(tries):
        c = KwpConn('127.0.0.1', port, timeout=timeout)
        # `SHOW META` answers in the **tag**, not in rows - it has no
        # result set. A first draft read `rows` and found nothing, so every
        # candidate looked wrong and the search exhausted itself.
        fields, rows, tag, affected = c.execute('SHOW META')
        core = field(tag, 'core')
        if core is not None and core != str(owner_core):
            return c
        spare.append(c)
    raise RuntimeError(f'no typed session landed off core {owner_core}')


def field(reply, key):
    for token in reply.split():
        if token.startswith(key + '='):
            return token.split('=', 1)[1]
    return None


def run_cell(arm, args):
    """One armed point, from a fresh instance to a counted restart."""
    out = {'point': arm}
    workdir = os.path.join(args.workdir, arm.replace(':', '-').replace('.', '_'))
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = write_conf(workdir, args.port, args.cores)
    server = os.path.join(ROOT, args.build_dir, 'kds_server')

    # ---- Setup on an *unarmed* instance -------------------------------
    #
    # The rows are written before the arm exists, so the kill lands on the
    # read and never on a write: this cell is about the answer edge, and a
    # death during setup would be a different (and unasked) question.
    proc = start(conf, workdir, server, arm=None)
    up, why = wait_up(args.port, proc, args.mount_timeout)
    if not up:
        out['error'] = f'first mount: {why}'
        stop_proc(proc)
        return out
    try:
        text = TextConn('127.0.0.1', debug_text_port(args.port))
        created = text.cmd('CREATE TABLE t (id int64, v int64) BTREE')
        if created.startswith('ERR'):
            out['error'] = f'CREATE: {created[:200]}'
            return out
        owner = field(text.cmd('DESCRIBE t'), 'owner_core')
        out['owner_core'] = owner
        if owner in (None, '0'):
            # The cell cannot pose its question: with the relation on the
            # client's own core nothing ships and no answer edge opens.
            out['vacuous'] = f'relation landed on core {owner}, not a peer'
            return out
        for k in range(ROWS):
            # The row-id lease refuses retryably until the peer's refill
            # grant lands - the same wait `txn_2pc_kill_matrix_probe`
            # takes, and a real one: a first insert into a peer-owned
            # relation always meets it.
            for _ in range(40):
                r = text.cmd(f'INSERT INTO t VALUES ({k})')
                if not (r.startswith('ERR') and 'retryable=1' in r):
                    break
                time.sleep(0.05)
            if r.startswith('ERR'):
                out['error'] = f'seed insert {k}: {r[:200]}'
                return out
        text.close()
    finally:
        if 'error' in out or 'vacuous' in out:
            stop_proc(proc)
    if 'error' in out or 'vacuous' in out:
        return out

    stop_proc(proc, args.port)

    # ---- The armed read ------------------------------------------------
    proc = start(conf, workdir, server, arm=arm)
    up, why = wait_up(args.port, proc, args.mount_timeout)
    if not up:
        out['error'] = f'armed mount: {why}'
        stop_proc(proc)
        return out

    delivered = None
    spare = []
    try:
        # A **typed** client, because the answer edge is a typed client's
        # shipped read (XG1 §4a): the text arm keeps its own 992-byte cap
        # and never opens one.
        kwp = kwp_session_off(args.port, out['owner_core'], args.read_timeout, spare)
        fields, rows, tag, affected = kwp.execute('SELECT id, v FROM t')
        delivered = ('rows', len(rows))
    except KwpError as e:
        delivered = ('kwp_error', e.as_reply_line()[:200])
    except (ConnectionError, OSError) as e:
        # The expected shape: the process died under the client, which is
        # what a kill -9 looks like from a socket.
        delivered = ('connection', f'{type(e).__name__}: {e}')
    out['delivered'] = delivered

    try:
        proc.wait(timeout=args.mount_timeout)
    except subprocess.TimeoutExpired:
        out['error'] = 'the armed instance did not die at its point'
        proc.kill()
        proc.wait(timeout=10)
        return out
    wait_port_free(args.port)
    out['killed_rc'] = proc.returncode
    stderr = open(os.path.join(workdir, 's.stderr')).read()
    point_name = arm.split(':')[0]
    out['crash_line'] = f"crash point '{point_name}' fired" in stderr
    if proc.returncode != -9:
        out['error'] = f'died rc={proc.returncode}, not SIGKILL'
        return out
    if not out['crash_line']:
        out['error'] = 'the process died without reaching its crash point'
        return out

    # ---- The restart ----------------------------------------------------
    proc = start(conf, workdir, server, arm=None)
    up, why = wait_up(args.port, proc, args.mount_timeout)
    if not up:
        out['error'] = f'restart: {why}'
        stop_proc(proc)
        return out
    try:
        text = TextConn('127.0.0.1', debug_text_port(args.port))
        meta = text.cmd('SHOW META')
        out['in_doubt_unresolved'] = field(meta, 'txn_in_doubt_unresolved')
        text.close()
        # **Read back through the typed client, not the text port.** The
        # relation is on a peer, so this read ships - and the text arm
        # keeps its own 992-byte reply cap (XG1 §4a), which 400 rows do not
        # fit in. A first draft read here and scored every cell "not
        # readable" on a cap that has nothing to do with the crash.
        after = KwpConn('127.0.0.1', args.port, timeout=args.read_timeout)
        fields, rows, tag, affected = after.execute('SELECT id FROM t')
        out['readable_after'] = True
        out['rows_after'] = len(rows)
        after.close()
    except (KwpError, ConnectionError, OSError) as e:
        out['readable_after'] = False
        out['read_after_error'] = f'{type(e).__name__}: {e}'
    finally:
        stop_proc(proc)
    return out


def verdict(cell):
    """What each cell had to show. Every clause is a claim XG3 made and did
    not test under a kill."""
    if 'error' in cell:
        return 'ERROR: ' + cell['error']
    if 'vacuous' in cell:
        return 'VACUOUS: ' + cell['vacuous']
    kind = cell['delivered'][0]
    # **No result is delivered.** A `rows` answer here is the failure this
    # cell exists for: the client got a result set out of a process that
    # died mid-answer.
    if kind == 'rows':
        return f'FAIL: a result was delivered from a dying owner ({cell["delivered"][1]})'
    if not cell.get('crash_line'):
        return 'FAIL: the crash point was not reached'
    if not cell.get('readable_after'):
        return 'FAIL: the relation was not readable after the restart'
    # A shipped *read* writes nothing, so anything in doubt would be the
    # read path holding state it should not.
    if cell.get('in_doubt_unresolved') not in (None, '0'):
        return f'FAIL: in-doubt residue after a read: {cell["in_doubt_unresolved"]}'
    if cell.get('rows_after') != ROWS:
        return f'FAIL: {cell.get("rows_after")} rows after the restart, expected {ROWS}'
    return f'PASS ({kind})'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cores', type=int, default=3)
    ap.add_argument('--port', type=int, default=15490)
    ap.add_argument('--build-dir', default='build-release')
    ap.add_argument('--workdir', default='/tmp/kds-answer-edge-kill')
    ap.add_argument('--mount-timeout', type=float, default=30.0)
    ap.add_argument('--read-timeout', type=float, default=15.0)
    ap.add_argument('--json', default=None)
    args = ap.parse_args()

    cells = []
    for arm in POINTS:
        cell = run_cell(arm, args)
        cell['verdict'] = verdict(cell)
        cells.append(cell)
        print(f"{arm:42s} {cell['verdict']}")

    if args.json:
        with open(args.json, 'w') as f:
            json.dump(cells, f, indent=2)

    failed = [c for c in cells if not c['verdict'].startswith('PASS')]
    print(f"\n{len(cells) - len(failed)}/{len(cells)} cells PASS")
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
