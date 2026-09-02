#!/usr/bin/env python3
"""AI-T3: what the foreign key's crossing costs, and what the lift did not.

Two measurements, one driver, because they share a server harness and must
not run at the same time as each other on this host.

**M1 - the peer-writer gate's lift, on a write that never crosses.**
`CheckWriteAffinity`'s `funded_shape` lost two `empty()` tests when work
order AI narrowed its foreign-key arm. *Every* peer write runs that
predicate, including the overwhelming majority carrying no foreign key at
all, so the question is whether the population that gained nothing from the
lift paid anything for it. Arms are two **builds** - `546ddc8` (pre-lift)
and the commit under test - alternated block by block on one host, writing
to a peer-owned relation with no foreign key.

**M2 - what a crossing costs, against the same statement that does not
cross.** One build, two shapes, alternated the same way:

  colocated  child and parent on the same peer. No probe, no decide.
  crossing   child on one peer, parent on the other. One probe round out
             and back, then - since AI's F1 - one decide round to release
             the reference intent the probe left behind.

The delta prices **both rounds together**. Separating them is AH-T6's leg
attribution and is not attempted here. What this file does instead is prove
the crossing crossed rather than assume it: `SHOW META`'s `fk_probes_sent`
is read on the child's owner and `fk_intents_granted` on the parent's, at
every block boundary. A colocated arm that quietly probed, or a crossing arm
that quietly did not, fails the run - a wrong shape is never a fast number.

**Why sessions on named cores.** The counters are per core and the block is
a per-core fact: the probe is sent by the core that *owns the child*, and the
intent is granted by the core that owns the parent. The kernel decides which
core a connection lands on (SO_REUSEPORT), so the only way to ask a chosen
core anything is to open connections until one lands there - which is what
`multicore_benchmark.collect_connections` exists for.

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
from multicore_benchmark import (  # noqa: E402
    Conn, collect_connections, field, is_retryable, session_core,
)


def write_conf(workdir, port, cores, durability=None):
    conf = os.path.join(workdir, 's.conf')
    with open(conf, 'w') as f:
        if durability:
            # **Two runs, and the second is what separates the ring from the
            # device.** Under the shipped default a write is fsync-bound, so
            # a round trip that costs tens of microseconds hides inside a
            # millisecond of device - which is the honest headline number
            # for a user and a useless one for sizing the protocol. XD's
            # precedent: price the leg again where the device is not in it.
            f.write(f"durability = {durability}\n")
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\n"
                f"port = {port}\ncores = {cores}\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = warn\n")
        if cores > 1:
            # `rotate` never places on core 0, so relations land on peers
            # and core 0 stays the client's - the arrangement a cross-owner
            # foreign key needs. `peer_listeners` is what lets a session be
            # opened *on* a named core, which is the only way to read that
            # core's counters, and it is refused at one core because there
            # is no peer to listen.
            f.write("placement = rotate\npeer_listeners = on\n")
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
            with socket.create_connection(('127.0.0.1', port), 0.25):
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


def owner_of(conn, table):
    return field(conn.cmd(f'DESCRIBE {table}'), 'owner_core')


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


def retrying(conn, sql, tries=200):
    """A setup statement, through the refusals that mean "again, later".

    A peer's row-id and transaction-id leases refill by asking core 0, and
    the ask is answered on a later tick - so the first statements against a
    fresh peer relation legitimately answer `TXN_CONFLICT retryable=1`.
    That is a wait, not a failure, and a driver that treated it as one would
    report a broken engine every time it started a server.
    """
    for _ in range(tries):
        reply = conn.cmd(sql)
        if not is_retryable(reply):
            return reply
        time.sleep(0.002)
    return reply


def time_inserts(conn, sql, rows):
    """One block: `rows` autocommit INSERTs, each timed on its own.

    **A retried statement contributes no sample.** A lease refill is a wait
    on another core's tick, not a cost of the shape under test, and folding
    one into the distribution would put a millisecond in a microsecond
    percentile. They are counted instead, and the count is reported beside
    the numbers so a block that spent its time waiting is visible rather
    than fast.
    """
    samples = []
    retries = 0
    for i in range(rows):
        while True:
            began = time.perf_counter()
            reply = conn.cmd(sql)
            elapsed_us = (time.perf_counter() - began) * 1e6
            if not reply.startswith('ERR'):
                samples.append(elapsed_us)
                break
            if not is_retryable(reply):
                return None, retries, f'row {i}: {reply[:200]}'
            retries += 1
            if retries > 20 * rows + 200:
                return None, retries, f'row {i}: retried forever: {reply[:200]}'
            time.sleep(0.002)
    return samples, retries, None


def values_of(table, pk, per_statement):
    """One INSERT naming `per_statement` child rows, all against one parent.

    **All against one parent on purpose** (H-AH2). The extraction pass
    deduplicates by (parent relation, pk) before it groups by owner, so a
    hundred rows naming one parent are one probe round - which is the
    hypothesis, and a statement whose rows named a hundred different parents
    would measure the cap instead.
    """
    tuples = ','.join(['(%s)' % pk] * per_statement)
    return f'INSERT INTO {table} VALUES {tuples}'


def fk_counters(conn):
    reply = conn.cmd('SHOW META')
    return {name: int(field(reply, name) or 0)
            for name in ('fk_probes_sent', 'fk_intents_granted',
                         'fk_intents_released', 'fk_intents_live')}


# ---- M1: the gate's lift, on a write that never crosses -------------------

def gate_block(server, args, tag):
    workdir = os.path.join(args.workdir, f'gate-{tag}')
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = write_conf(workdir, args.port, args.cores, args.durability)
    proc = start(conf, workdir, server)
    up, why = wait_up(args.port, proc, args.mount_timeout)
    if not up:
        stop(proc, args.port)
        return {'error': f'mount: {why}'}
    conns = None
    try:
        conns, _ = collect_connections(args.port, {0: 1}, args.max_connects)
        client = conns[0][0]
        if client.cmd('CREATE TABLE plain (id int64, v int64)').startswith('ERR'):
            return {'error': 'create'}
        owner = owner_of(client, 'plain')
        if owner in (None, '0'):
            return {'error': f'plain landed on core {owner}, not a peer'}
        # Warm: the first writes to a peer relation pay a fault grant and a
        # lease refill that nothing after them does, and a mean carrying
        # those would price the mount rather than the predicate.
        time_inserts(client, 'INSERT INTO plain VALUES (1)', args.warm)
        samples, retries, err = time_inserts(client, 'INSERT INTO plain VALUES (2)', args.rows)
        if err:
            return {'error': err}
        return {'owner_core': owner, 'us': samples, 'retries': retries}
    finally:
        if conns:
            for group in conns.values():
                for c in group:
                    c.close()
        stop(proc, args.port)


# ---- H-AH1: the hoist, on the colocated path it must not slow -------------

def hoist_block(server, args, tag):
    """One block of colocated foreign-key writes, on **one core**.

    AH-T1 moved the forward check's descent out of the write scope: a
    statement resolves every parent it names once, at the dispatch fork, and
    the per-row check answers from what it holds. H-AH1 says that costs the
    colocated path nothing - and the colocated path is where the population
    lives, so a hoist that bought the crossing at its expense would be a bad
    trade made quietly.

    **One core, deliberately.** The pre-hoist arm refuses a peer write to an
    FK-linked relation outright (the funding gate work order AI later
    narrowed), so a peer-owned colocated pair is not a shape both arms admit.
    A single-core instance has no gate, no ring and no shipping in the way,
    and it exercises exactly the code the hoist moved.
    """
    workdir = os.path.join(args.workdir, f'hoist-{tag}')
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = write_conf(workdir, args.port, 1, args.durability)
    proc = start(conf, workdir, server)
    up, why = wait_up(args.port, proc, args.mount_timeout)
    if not up:
        stop(proc, args.port)
        return {'error': f'mount: {why}'}
    conns = None
    try:
        conns, _ = collect_connections(args.port, {0: 1}, args.max_connects)
        client = conns[0][0]
        for sql in ('CREATE TABLE hp (id int64, v int64) BTREE',
                    'CREATE TABLE hc (id int64, pid int64 REFERENCES hp)'):
            reply = retrying(client, sql)
            if reply.startswith('ERR'):
                return {'error': f'setup: {sql}: {reply[:160]}'}
        reply = retrying(client, 'INSERT INTO hp VALUES (5)')
        if reply.startswith('ERR'):
            return {'error': f'seed: {reply[:160]}'}
        parent_pk = field(reply, 'id')
        cells = []
        time_inserts(client, values_of('hc', parent_pk, 1), args.warm)
        for per in args.per_statement:
            samples, retries, err = time_inserts(
                client, values_of('hc', parent_pk, per), args.rows)
            if err:
                return {'error': f'per={per}: {err}'}
            cells.append({'per_statement': per, 'us': samples, 'retries': retries})
        return {'cells': cells}
    finally:
        if conns:
            for group in conns.values():
                for c in group:
                    c.close()
        stop(proc, args.port)


# ---- M2: the crossing, against the statement that does not cross ----------

def crossing_setup(client):
    """Four relations, so that one child sits with its parent and one does not.

    `rotate` at three cores alternates the two peers in create order, so the
    order of the creates *is* the placement: seq 1 and 3 land on one peer,
    seq 2 and 4 on the other. `spacer` exists only to push `cx` onto the
    other one.

    **Both children reference the same parent row**, which removes every
    variable but the one being measured: the two arms differ in where the
    parent lives relative to the child and in nothing else - not the parent
    relation, not the row, not the page.

    Placement is asserted, never assumed. A rotation rule that changed would
    turn this into two copies of the same shape, and a fast number that
    measured the wrong thing is worse than no number at all.
    """
    stmts = [
        'CREATE TABLE pa (id int64, v int64) BTREE',            # peer X
        'CREATE TABLE spacer (id int64, v int64)',              # peer Y
        'CREATE TABLE cc (id int64, pid int64 REFERENCES pa)',  # peer X - with pa
        'CREATE TABLE cx (id int64, pid int64 REFERENCES pa)',  # peer Y - across from pa
    ]
    for sql in stmts:
        reply = retrying(client, sql)
        if reply.startswith('ERR'):
            return None, f'setup: {sql}: {reply[:160]}'
    owners = {t: owner_of(client, t) for t in ('pa', 'spacer', 'cc', 'cx')}
    if owners['cc'] != owners['pa']:
        return None, f'the colocated pair is not colocated: {owners}'
    if owners['cx'] == owners['pa']:
        return None, f'the crossing pair does not cross: {owners}'
    if '0' in owners.values():
        return None, f'a relation landed on the system core: {owners}'
    # **The pk is issued, not named.** A caller-supplied primary key writes
    # the relation's catalog row, which is the system core's, so a peer
    # refuses one by name - and `pa` is peer-owned by construction here. So
    # the seed omits the key and the issued one is read back off the reply.
    reply = retrying(client, 'INSERT INTO pa VALUES (5)')
    if reply.startswith('ERR'):
        return None, f'seed pa: {reply[:160]}'
    parent_pk = field(reply, 'id')
    if parent_pk is None:
        return None, f'the seed reply named no id: {reply[:160]}'
    owners['parent_pk'] = parent_pk
    return owners, None


def crossing_run(args):
    workdir = os.path.join(args.workdir, 'crossing')
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = write_conf(workdir, args.port, args.cores, args.durability)
    proc = start(conf, workdir, args.server)
    up, why = wait_up(args.port, proc, args.mount_timeout)
    if not up:
        stop(proc, args.port)
        return {'error': f'mount: {why}'}
    out = {'blocks': []}
    conns = None
    try:
        conns, attempts = collect_connections(args.port, {0: 1, 1: 1, 2: 1}, args.max_connects)
        out['connect_attempts'] = attempts
        client = conns[0][0]
        owners, err = crossing_setup(client)
        if err:
            out['error'] = err
            return out
        out['owners'] = owners
        # The two cores the counters live on: the child's owner sends the
        # probe, the parent's grants the intent.
        parent_core = int(owners['pa'])
        crossing_child_core = int(owners['cx'])
        watcher = {core: conns[core][0] for core in (parent_core, crossing_child_core)}
        parent_pk = owners['parent_pk']
        tables = {'colocated': 'cc', 'crossing': 'cx'}
        for table in tables.values():
            time_inserts(client, values_of(table, parent_pk, 1), args.warm)
        for per in args.per_statement:
            for block in range(args.blocks):
                for name in ('colocated', 'crossing'):
                    sql = values_of(tables[name], parent_pk, per)
                    before = {c: fk_counters(w) for c, w in watcher.items()}
                    samples, retries, err = time_inserts(client, sql, args.rows)
                    after = {c: fk_counters(w) for c, w in watcher.items()}
                    if err:
                        out['error'] = f'{name} per={per}: {err}'
                        return out
                    out['blocks'].append({
                        'shape': name,
                        'per_statement': per,
                        'block': block,
                        'us': samples,
                        'retries': retries,
                        'probes_sent': (after[crossing_child_core]['fk_probes_sent'] -
                                        before[crossing_child_core]['fk_probes_sent']),
                        'intents_granted': (after[parent_core]['fk_intents_granted'] -
                                            before[parent_core]['fk_intents_granted']),
                        'intents_released': (after[parent_core]['fk_intents_released'] -
                                             before[parent_core]['fk_intents_released']),
                        'intents_live_after': after[parent_core]['fk_intents_live'],
                    })
    finally:
        if conns:
            for group in conns.values():
                for c in group:
                    c.close()
        stop(proc, args.port)
    return out


# ---- H-AH2's other half: cost counts owners --------------------------------

def owners_run(args):
    """One foreign owner against two, at four cores.

    AH-R2 says a statement's cross-owner cost is a function of **how many
    distinct owners its parents live on**, never of its row count. The sweep
    in `crossing` holds the owner count at one and varies the rows; this
    holds the rows at one and varies the owners, which is the other axis of
    the same claim.

    Four cores, because three cannot pose the question: the child must sit
    on a core that owns *neither* parent, so two foreign parents need three
    peers. `rotate` cycles the non-system cores in create order, so the
    order of the creates is again the placement, and it is asserted.

    **Both children take two values and differ only in how many of them are
    foreign keys.** `one`'s second column references nothing, so the two
    statements are the same width, the same row count and the same work
    everywhere except the number of owners that must be asked.
    """
    workdir = os.path.join(args.workdir, 'owners')
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    conf = write_conf(workdir, args.port, 4, args.durability)
    proc = start(conf, workdir, args.server)
    up, why = wait_up(args.port, proc, args.mount_timeout)
    if not up:
        stop(proc, args.port)
        return {'error': f'mount: {why}'}
    out = {'blocks': []}
    conns = None
    try:
        conns, attempts = collect_connections(args.port, {0: 1, 1: 1, 2: 1, 3: 1},
                                              args.max_connects)
        out['connect_attempts'] = attempts
        client = conns[0][0]
        stmts = [
            'CREATE TABLE op1 (id int64, v int64) BTREE',                    # core 1
            'CREATE TABLE op2 (id int64, v int64) BTREE',                    # core 2
            'CREATE TABLE otwo (id int64, a int64 REFERENCES op1, '
            'b int64 REFERENCES op2)',                                       # core 3
            'CREATE TABLE osp4 (id int64, v int64)',                         # core 1
            'CREATE TABLE osp5 (id int64, v int64)',                         # core 2
            'CREATE TABLE oone (id int64, a int64 REFERENCES op1, b int64)',  # core 3
        ]
        for sql in stmts:
            reply = retrying(client, sql)
            if reply.startswith('ERR'):
                out['error'] = f'setup: {sql}: {reply[:160]}'
                return out
        owners = {t: owner_of(client, t) for t in ('op1', 'op2', 'otwo', 'oone')}
        out['owners'] = owners
        if owners['otwo'] != owners['oone']:
            out['error'] = f'the two children are not on one core: {owners}'
            return out
        if owners['otwo'] in (owners['op1'], owners['op2']):
            out['error'] = f'a parent shares the child core: {owners}'
            return out
        if owners['op1'] == owners['op2']:
            out['error'] = f'the two parents share an owner: {owners}'
            return out
        pks = {}
        for parent in ('op1', 'op2'):
            reply = retrying(client, f'INSERT INTO {parent} VALUES (5)')
            if reply.startswith('ERR'):
                out['error'] = f'seed {parent}: {reply[:160]}'
                return out
            pks[parent] = field(reply, 'id')
        child_core = int(owners['otwo'])
        watcher = conns[child_core][0]
        shapes = {'one-owner': f"INSERT INTO oone VALUES ({pks['op1']}, 1)",
                  'two-owners': f"INSERT INTO otwo VALUES ({pks['op1']}, {pks['op2']})"}
        for sql in shapes.values():
            time_inserts(client, sql, args.warm)
        for block in range(args.blocks):
            for name in ('one-owner', 'two-owners'):
                before = fk_counters(watcher)
                samples, retries, err = time_inserts(client, shapes[name], args.rows)
                after = fk_counters(watcher)
                if err:
                    out['error'] = f'{name}: {err}'
                    return out
                out['blocks'].append({
                    'shape': name, 'block': block, 'us': samples, 'retries': retries,
                    'probes_sent': after['fk_probes_sent'] - before['fk_probes_sent'],
                })
    finally:
        if conns:
            for group in conns.values():
                for c in group:
                    c.close()
        stop(proc, args.port)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', choices=('gate-ab', 'crossing', 'hoist-ab', 'owners'),
                    required=True)
    ap.add_argument('--server', default=os.path.join(ROOT, 'build-release', 'kds_server'))
    ap.add_argument('--server-a')
    ap.add_argument('--server-b')
    ap.add_argument('--cores', type=int, default=3)
    ap.add_argument('--port', type=int, default=15496)
    ap.add_argument('--rows', type=int, default=400)
    ap.add_argument('--per-statement', default='1',
                    help='comma-separated child rows per INSERT, all naming one parent')
    ap.add_argument('--warm', type=int, default=40)
    ap.add_argument('--blocks', type=int, default=3)
    ap.add_argument('--max-connects', type=int, default=64)
    ap.add_argument('--workdir', default='/tmp/kds-ai-t3')
    ap.add_argument('--durability', default=None,
                    help='server durability class; omitted means the shipped default')
    ap.add_argument('--mount-timeout', type=float, default=30.0)
    ap.add_argument('--json', default=None)
    args = ap.parse_args()
    args.per_statement = [int(v) for v in str(args.per_statement).split(',') if v]

    result = {'mode': args.mode, 'rows': args.rows, 'blocks': args.blocks,
              'cores': args.cores, 'durability': args.durability or 'default',
              'per_statement': args.per_statement}
    failed = False

    if args.mode == 'gate-ab':
        if not args.server_a or not args.server_b:
            print('gate-ab needs --server-a and --server-b', file=sys.stderr)
            return 2
        arms = (('a-prelift', args.server_a), ('b-postlift', args.server_b))
        result['blocks'] = []
        for block in range(args.blocks):
            for name, server in arms:
                cell = gate_block(server, args, f'{name}-{block}')
                cell['arm'] = name
                cell['block'] = block
                if 'error' in cell:
                    failed = True
                    print(f'{name} block {block}: ERROR {cell["error"]}')
                else:
                    p = percentiles(cell['us'])
                    print(f'{name} block {block}: p50={p["p50"]:.1f}us '
                          f'p90={p["p90"]:.1f}us mean={p["mean"]:.1f}us '
                          f'retries={cell["retries"]}')
                result['blocks'].append(cell)
    elif args.mode == 'hoist-ab':
        if not args.server_a or not args.server_b:
            print('hoist-ab needs --server-a and --server-b', file=sys.stderr)
            return 2
        arms = (('a-prehoist', args.server_a), ('b-posthoist', args.server_b))
        result['blocks'] = []
        for block in range(args.blocks):
            for name, server in arms:
                cell = hoist_block(server, args, f'{name}-{block}')
                cell['arm'] = name
                cell['block'] = block
                if 'error' in cell:
                    failed = True
                    print(f'{name} block {block}: ERROR {cell["error"]}')
                else:
                    for inner in cell['cells']:
                        p = percentiles(inner['us'])
                        print(f'{name} per={inner["per_statement"]:<4} block {block}: '
                              f'p50={p["p50"]:.1f}us p90={p["p90"]:.1f}us '
                              f'mean={p["mean"]:.1f}us retries={inner["retries"]}')
                result['blocks'].append(cell)
    elif args.mode == 'owners':
        run = owners_run(args)
        result.update(run)
        if 'error' in run:
            failed = True
            print(f'ERROR {run["error"]}')
        else:
            print(f'owners: {run["owners"]}')
            for cell in run['blocks']:
                p = percentiles(cell['us'])
                print(f'{cell["shape"]:>11} block {cell["block"]}: p50={p["p50"]:.1f}us '
                      f'p90={p["p90"]:.1f}us mean={p["mean"]:.1f}us '
                      f'probes={cell["probes_sent"]} retries={cell["retries"]}')
                want = args.rows * (1 if cell['shape'] == 'one-owner' else 2)
                if cell['probes_sent'] != want:
                    failed = True
                    print(f'  FAIL: {cell["probes_sent"]} rounds where {want} were owed')
    else:
        run = crossing_run(args)
        result.update(run)
        if 'error' in run:
            failed = True
            print(f'ERROR {run["error"]}')
        else:
            print(f'owners: {run["owners"]}')
            for cell in run['blocks']:
                p = percentiles(cell['us'])
                print(f'{cell["shape"]:>9} per={cell["per_statement"]:<4} '
                      f'block {cell["block"]}: p50={p["p50"]:.1f}us '
                      f'p90={p["p90"]:.1f}us mean={p["mean"]:.1f}us '
                      f'probes={cell["probes_sent"]} granted={cell["intents_granted"]} '
                      f'live={cell["intents_live_after"]} retries={cell["retries"]}')
                if cell['shape'] == 'colocated' and cell['probes_sent'] != 0:
                    failed = True
                    print('  FAIL: the colocated arm probed')
                # **One round per statement, whatever its row count** - the
                # hypothesis, checked rather than reported.
                if cell['shape'] == 'crossing' and cell['probes_sent'] != args.rows:
                    failed = True
                    print(f'  FAIL: the crossing arm sent {cell["probes_sent"]} rounds '
                          f'for {args.rows} statements of {cell["per_statement"]} rows')
                if cell['shape'] == 'crossing' and cell['intents_live_after'] != 0:
                    failed = True
                    print('  FAIL: an intent outlived its autocommit statement')

    if args.json:
        with open(args.json, 'w') as f:
            json.dump(result, f, indent=2)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
