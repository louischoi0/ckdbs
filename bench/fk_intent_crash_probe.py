#!/usr/bin/env python3
"""AH-T5: the reference intent's crash window, and whether it can commit.

A cross-owner foreign key's forward check leaves a **reference intent** on
the parent's owner: *"I am relying on `(relation, pk)` existing."* It is
memory-resident by design - under `cross-owner-txn.md` §1a an intent-only
participant writes no `TXN_PREPARE` record, so there is nothing for an
intent to ride in - and what makes that safe is an invariant rather than
an argument (AH-R5):

    a participant that restarts after granting an intent and before its
    prepare leg forces the coordinator's transaction to fail.

**A window in which the coordinator can still commit is a defect of AH,
full stop.** So this probe kills at exactly that point
(`participant.fk_intent_granted_preprepare`) and asks what the restart
finds.

**What one process can and cannot show.** Both cores are threads of one
process, so the death takes the coordinator with the participant - this
cell cannot stage a participant dying *under a surviving coordinator*.
What it can show, and what the invariant reduces to on this engine, is
that **the child row is not there afterwards**: a transaction whose
participant lost its enrolment mid-flight did not commit. That is the
falsifiable half; the surviving-coordinator half needs two processes and
is named as still owed rather than claimed.

    python3 bench/fk_intent_crash_probe.py [--cores N] [--port P]

Exit status is 0 only when every cell passes.
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from ckdbs_cli import TextConnection  # noqa: E402

POINT = 'participant.fk_intent_granted_preprepare'
TAG = 'ahT5'


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
                # `rotate` never places on core 0, so the two relations land
                # on peers and the client's core 0 is free to be the
                # coordinator - the arrangement a cross-owner FK needs.
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
            c = socket.create_connection(('127.0.0.1', port), timeout=0.3)
            c.close()
            return True, None
        except OSError:
            time.sleep(0.05)
    return False, f'no listener within {deadline_s}s'


def wait_port_free(port, deadline_s=20.0):
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


def field(reply, key):
    for token in reply.split():
        if token.startswith(key + '='):
            return token.split('=', 1)[1]
    return None


def retrying(conn, sql, tries=40):
    """A first write to a peer-owned relation meets a retryable row-id
    lease refusal until the refill grant lands - a real wait, not a flaky
    harness (`txn_2pc_kill_matrix_probe` takes the same one)."""
    r = ''
    for _ in range(tries):
        r = conn.cmd(sql)
        if not (r.startswith('ERR') and 'retryable=1' in r):
            return r
        time.sleep(0.05)
    return r


def setup(text, out):
    """Two peer-owned relations linked by a foreign key, and a parent row
    for a child to reference. Cross-owner `REFERENCES` is admitted since
    AH-T4; before it this setup was refused at the declaration."""
    if (r := text.cmd('CREATE TABLE parent (id int64, v int64) BTREE')).startswith('ERR'):
        out['error'] = f'CREATE parent: {r[:200]}'
        return False
    if (r := text.cmd('CREATE TABLE child (id int64, pid int64 REFERENCES parent, '
                      'tag varchar) BTREE')).startswith('ERR'):
        out['error'] = f'CREATE child: {r[:200]}'
        return False
    owners = {t: field(text.cmd(f'DESCRIBE {t}'), 'owner_core') for t in ('parent', 'child')}
    out['owners'] = owners
    if owners['parent'] == owners['child'] or '0' in owners.values():
        # The cell cannot pose its question: with parent and child on one
        # core nothing crosses and no intent is granted.
        out['vacuous'] = f'relations did not land on two peer cores: {owners}'
        return False
    if (r := retrying(text, "INSERT INTO parent VALUES (7)")).startswith('ERR'):
        out['error'] = f'seed parent: {r[:200]}'
        return False
    out['parent_pk'] = field(r, 'id')
    return True


def run_cell(args):
    out = {'point': POINT}
    workdir = os.path.join(args.workdir, POINT.replace('.', '_'))
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = write_conf(workdir, args.port, args.cores)
    server = os.path.join(ROOT, args.build_dir, 'kds_server')

    # ---- Setup, unarmed -------------------------------------------------
    proc = start(conf, workdir, server, arm=None)
    up, why = wait_up(args.port, proc, args.mount_timeout)
    if not up:
        out['error'] = f'first mount: {why}'
        stop_proc(proc, args.port)
        return out
    text = TextConn('127.0.0.1', debug_text_port(args.port))
    ok = setup(text, out)
    text.close()
    stop_proc(proc, args.port)
    if not ok:
        return out

    # ---- The armed child write -----------------------------------------
    #
    # Inside an explicit transaction, so the probe **enrols** the parent's
    # owner as a participant (AH-T2's third slice): that is what makes the
    # window a 2PC window rather than an autocommit statement's.
    proc = start(conf, workdir, server, arm=POINT)
    up, why = wait_up(args.port, proc, args.mount_timeout)
    if not up:
        out['error'] = f'armed mount: {why}'
        stop_proc(proc, args.port)
        return out
    try:
        text = TextConn('127.0.0.1', debug_text_port(args.port))
        text.cmd('BEGIN')
        out['insert_reply'] = retrying(
            text, f"INSERT INTO child VALUES ({out['parent_pk']}, '{TAG}')", tries=1)[:200]
        out['commit_reply'] = text.cmd('COMMIT')[:200]
        text.close()
    except (ConnectionError, OSError) as e:
        # The expected shape: the process died under the client.
        out['insert_reply'] = f'{type(e).__name__}: {e}'

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
    out['crash_line'] = f"crash point '{POINT}' fired" in stderr
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
        stop_proc(proc, args.port)
        return out
    try:
        text = TextConn('127.0.0.1', debug_text_port(args.port))
        meta = text.cmd('SHOW META')
        out['in_doubt_unresolved'] = field(meta, 'txn_in_doubt_unresolved')
        reply = text.cmd(f"SELECT id FROM child WHERE tag = '{TAG}'")
        out['child_reply'] = reply[:200]
        out['child_rows'] = 0 if reply.startswith('ERR') else reply.count('\\n')
        out['parent_reply'] = text.cmd('SELECT id FROM parent')[:200]
        text.close()
    finally:
        stop_proc(proc, args.port)
    return out


def verdict(cell):
    if 'error' in cell:
        return 'ERROR: ' + cell['error']
    if 'vacuous' in cell:
        return 'VACUOUS: ' + cell['vacuous']
    if not cell.get('crash_line'):
        return 'FAIL: the crash point was not reached'
    # **The invariant.** A participant that lost its enrolment in this
    # window must not leave a committed child row behind.
    if cell.get('child_rows', 0) != 0:
        return (f'FAIL: {cell["child_rows"]} child row(s) survived a transaction whose '
                'participant died before preparing')
    # And the restart resolved everything it had: an intent-only
    # participant writes no prepare record, so there is nothing to be in
    # doubt about.
    if cell.get('in_doubt_unresolved') not in (None, '0'):
        return f'FAIL: in-doubt residue: {cell["in_doubt_unresolved"]}'
    if cell.get('parent_reply', '').startswith('ERR'):
        return 'FAIL: the parent relation was not readable after the restart'
    return 'PASS'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cores', type=int, default=3)
    ap.add_argument('--port', type=int, default=15494)
    ap.add_argument('--build-dir', default='build-release')
    ap.add_argument('--workdir', default='/tmp/kds-fk-intent-crash')
    ap.add_argument('--mount-timeout', type=float, default=30.0)
    ap.add_argument('--passes', type=int, default=3)
    ap.add_argument('--json', default=None)
    args = ap.parse_args()

    cells = []
    for p in range(args.passes):
        cell = run_cell(args)
        cell['pass'] = p + 1
        cell['verdict'] = verdict(cell)
        cells.append(cell)
        print(f"pass {p + 1}: {cell['verdict']}")

    if args.json:
        with open(args.json, 'w') as f:
            json.dump(cells, f, indent=2)

    failed = [c for c in cells if not c['verdict'].startswith('PASS')]
    print(f"\n{len(cells) - len(failed)}/{len(cells)} passes PASS")
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
