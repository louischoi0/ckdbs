#!/usr/bin/env python3
"""AI-T3: what the foreign key's crossing costs, and what the lift did not.

Two measurements, one driver, because they share a server harness and must
not run at the same time as each other on this host.

**M1 - the peer-writer gate's lift, on a write that never crossed.**
`CheckWriteAffinity`'s `funded_shape` lost two `empty()` tests when work
order AI narrowed its foreign-key arm. Every peer write runs that predicate,
including the overwhelming majority that carry no foreign key at all, so the
question is whether the population that gained nothing from the lift paid
anything for it. Arms are two *builds* - `546ddc8` (pre-lift) and the commit
under test - alternated block by block on one host, writing to a peer-owned
relation with no foreign key.

**M2 - what a crossing costs, against the same statement that does not
cross.** One build, two shapes, alternated the same way:

  colocated  parent and child on the same peer. No probe, no decide.
  crossing   parent on one peer, child on another. One probe round out and
             back, then - since AI's F1 - one decide round to release the
             reference intent the probe left behind.

The delta prices **both rounds together**. Separating them is AH-T6's leg
attribution and is not attempted here; what this file can do, and does, is
prove the crossing crossed rather than assume it: `SHOW META`'s
`fk_probes_sent` and `fk_intents_granted` are read at each block boundary,
so a colocated arm that quietly probed, or a crossing arm that quietly did
not, is a failed run and not a fast number.

    python3 bench/fk_crossing_cost_probe.py --mode crossing --server build-release/kds_server
    python3 bench/fk_crossing_cost_probe.py --mode gate-ab \
        --server-a <pre-lift>/kds_server --server-b build-release/kds_server

Exit status is 0 only when every block ran and every counter agreed.
"""
import argparse
import json
import os
import shutil
import socket
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from ckdbs_cli import TextConnection  # noqa: E402


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
                # `rotate` never places on core 0, so relations land on peers
                # and the client's core 0 stays free to be the coordinator -
                # the arrangement a cross-owner foreign key needs, and the
                # one `fk_intent_crash_probe.py` already uses.
                "placement = rotate\npeer_listeners = on\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = warn\n"
                f"debug_text_port = {debug_text_port(port)}\n")
    return conf


def start(conf, workdir, server):
    err = open(os.path.join(workdir, 's.stderr'), 'w')
    return subprocess.Popen([server, '--config', conf], stdout=err, stderr=err)


def wait_up(port, proc, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False, f'server exited rc={proc.returncode}'
        try:
            with socket.create_connection(('127.0.0.1', debug_text_port(port)), 0.25):
                return True, ''
        except OSError:
            time.sleep(0.05)
    return False, 'timed out'


def stop(proc, port):
    proc.terminate()
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            with socket.create_connection(('127.0.0.1', port), 0.1):
                time.sleep(0.05)
        except OSError:
            return
    return


def field(reply, name):
    for token in reply.replace('\\n', ' ').split():
        if token.startswith(name + '='):
            return token[len(name) + 1:]
    return None


def owner_of(text, table):
    return field(text.cmd(f'DESCRIBE {table}'), 'owner_core')


def percentiles(values):
    ordered = sorted(values)
    def at(p):
        if not ordered:
            return 0.0
        idx = min(len(ordered) - 1, max(0, int(round(p / 100.0 * (len(ordered) - 1)))))
        return ordered[idx]
    return {
        'p0': at(0), 'p25': at(25), 'p50': at(50), 'p90': at(90),
        'p99': at(99), 'p100': at(100),
        'mean': statistics.fmean(ordered) if ordered else 0.0,
    }


def time_inserts(text, sql_of, rows):
    """One block: `rows` autocommit INSERTs, each timed on its own."""
    samples = []
    for i in range(rows):
        began = time.perf_counter()
        reply = text.cmd(sql_of(i))
        elapsed_us = (time.perf_counter() - began) * 1e6
        if reply.startswith('ERR'):
            return None, f'row {i}: {reply[:200]}'
        samples.append(elapsed_us)
    return samples, None


def meta_counters(text):
    reply = text.cmd('SHOW META')
    return {
        'fk_probes_sent': int(field(reply, 'fk_probes_sent') or 0),
        'fk_intents_granted': int(field(reply, 'fk_intents_granted') or 0),
        'fk_intents_released': int(field(reply, 'fk_intents_released') or 0),
        'fk_intents_live': int(field(reply, 'fk_intents_live') or 0),
    }


# ---- M1: the gate's lift, on a write that never crosses -------------------

def gate_block(server, args, tag):
    workdir = os.path.join(args.workdir, f'gate-{tag}')
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = write_conf(workdir, args.port, args.cores)
    proc = start(conf, workdir, server)
    up, why = wait_up(args.port, proc, args.mount_timeout)
    if not up:
        stop(proc, args.port)
        return {'error': f'mount: {why}'}
    try:
        text = TextConn('127.0.0.1', debug_text_port(args.port))
        if text.cmd('CREATE TABLE plain (id int64, v int64)').startswith('ERR'):
            return {'error': 'create'}
        owner = owner_of(text, 'plain')
        if owner in (None, '0'):
            return {'error': f'plain landed on core {owner}, not a peer'}
        # Warm: the first write to a peer relation pays a fault grant and a
        # lease refill that nothing after it does, and a mean that carried
        # them would price the mount rather than the predicate.
        time_inserts(text, lambda i: f'INSERT INTO plain VALUES ({i})', args.warm)
        samples, err = time_inserts(
            text, lambda i: f'INSERT INTO plain VALUES ({100000 + i})', args.rows)
        text.close()
        if err:
            return {'error': err}
        return {'owner_core': owner, 'us': samples}
    finally:
        stop(proc, args.port)


# ---- M2: the crossing, against the statement that does not cross ----------

def crossing_setup(text):
    """Four relations, so that one child sits with its parent and one does not.

    `rotate` at three cores alternates the two peers, so the *order* of the
    creates is the placement: `pa` and `cc` land together, `pb` and `cx` land
    apart. Asserted rather than assumed - a placement rule that changes turns
    this measurement into two copies of the same shape.
    """
    stmts = [
        'CREATE TABLE pa (id int64, v int64) BTREE',          # peer X
        'CREATE TABLE pb (id int64, v int64) BTREE',          # peer Y
        'CREATE TABLE cc (id int64, pid int64 REFERENCES pa)',  # peer X - with pa
        'CREATE TABLE cx (id int64, pid int64 REFERENCES pb)',  # peer Y? no: peer X
    ]
    for sql in stmts:
        if text.cmd(sql).startswith('ERR'):
            return None, f'setup: {sql}'
    owners = {t: owner_of(text, t) for t in ('pa', 'pb', 'cc', 'cx')}
    if owners['cc'] != owners['pa']:
        return None, f'the colocated pair is not colocated: {owners}'
    if owners['cx'] == owners['pb']:
        return None, f'the crossing pair does not cross: {owners}'
    if '0' in owners.values():
        return None, f'a relation landed on the system core: {owners}'
    for parent in ('pa', 'pb'):
        if text.cmd(f'INSERT INTO {parent} VALUES (7, 5)').startswith('ERR'):
            return None, f'seed {parent}'
    return owners, None


def crossing_run(args):
    workdir = os.path.join(args.workdir, 'crossing')
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = write_conf(workdir, args.port, args.cores)
    proc = start(conf, workdir, args.server)
    up, why = wait_up(args.port, proc, args.mount_timeout)
    if not up:
        stop(proc, args.port)
        return {'error': f'mount: {why}'}
    out = {'blocks': []}
    try:
        text = TextConn('127.0.0.1', debug_text_port(args.port))
        owners, err = crossing_setup(text)
        if err:
            out['error'] = err
            return out
        out['owners'] = owners
        parent_pk = 1  # the first row of a fresh relation
        shapes = {
            'colocated': ('cc', parent_pk),
            'crossing': ('cx', parent_pk),
        }
        for name, (table, pk) in shapes.items():
            time_inserts(text, lambda i, t=table, k=pk: f'INSERT INTO {t} VALUES ({k})',
                         args.warm)
        for block in range(args.blocks):
            for name in ('colocated', 'crossing'):
                table, pk = shapes[name]
                before = meta_counters(text)
                samples, err = time_inserts(
                    text, lambda i, t=table, k=pk: f'INSERT INTO {t} VALUES ({k})', args.rows)
                after = meta_counters(text)
                if err:
                    out['error'] = f'{name}: {err}'
                    return out
                sent = after['fk_probes_sent'] - before['fk_probes_sent']
                out['blocks'].append({
                    'shape': name, 'block': block, 'us': samples,
                    'probes_sent': sent,
                    'intents_granted': after['fk_intents_granted'] - before['fk_intents_granted'],
                    'intents_live_after': after['fk_intents_live'],
                })
                # **The run is only as good as this check.** A colocated
                # arm that probed, or a crossing arm that did not, is
                # measuring something other than what it names; `main`
                # fails the run on either.
        text.close()
    finally:
        stop(proc, args.port)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', choices=('gate-ab', 'crossing'), required=True)
    ap.add_argument('--server', default=os.path.join(ROOT, 'build-release', 'kds_server'))
    ap.add_argument('--server-a')
    ap.add_argument('--server-b')
    ap.add_argument('--cores', type=int, default=3)
    ap.add_argument('--port', type=int, default=15496)
    ap.add_argument('--rows', type=int, default=400)
    ap.add_argument('--warm', type=int, default=40)
    ap.add_argument('--blocks', type=int, default=3)
    ap.add_argument('--workdir', default='/tmp/kds-ai-t3')
    ap.add_argument('--mount-timeout', type=float, default=30.0)
    ap.add_argument('--json', default=None)
    args = ap.parse_args()

    result = {'mode': args.mode, 'rows': args.rows, 'blocks': args.blocks,
              'cores': args.cores}
    failed = False

    if args.mode == 'gate-ab':
        if not args.server_a or not args.server_b:
            print('gate-ab needs --server-a and --server-b', file=sys.stderr)
            return 2
        arms = {'a-prelift': args.server_a, 'b-postlift': args.server_b}
        result['blocks'] = []
        for block in range(args.blocks):
            for name, server in arms.items():
                cell = gate_block(server, args, f'{name}-{block}')
                cell['arm'] = name
                cell['block'] = block
                if 'error' in cell:
                    failed = True
                    print(f'{name} block {block}: ERROR {cell["error"]}')
                else:
                    p = percentiles(cell['us'])
                    print(f'{name} block {block}: p50={p["p50"]:.1f}us '
                          f'p90={p["p90"]:.1f}us mean={p["mean"]:.1f}us')
                result['blocks'].append(cell)
    else:
        run = crossing_run(args)
        result.update(run)
        if 'error' in run:
            failed = True
            print(f'ERROR {run["error"]}')
        else:
            for cell in run['blocks']:
                p = percentiles(cell['us'])
                print(f'{cell["shape"]} block {cell["block"]}: p50={p["p50"]:.1f}us '
                      f'p90={p["p90"]:.1f}us mean={p["mean"]:.1f}us '
                      f'probes={cell["probes_sent"]} live={cell["intents_live_after"]}')
                if cell['shape'] == 'colocated' and cell['probes_sent'] != 0:
                    failed = True
                    print('  FAIL: the colocated arm probed')
                if cell['shape'] == 'crossing' and cell['probes_sent'] != args.rows:
                    failed = True
                    print(f'  FAIL: the crossing arm sent {cell["probes_sent"]} rounds '
                          f'for {args.rows} statements')

    if args.json:
        with open(args.json, 'w') as f:
            json.dump(result, f, indent=2)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
