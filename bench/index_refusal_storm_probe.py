#!/usr/bin/env python3
"""G2: does a REFUSED `CREATE INDEX` still consume pages?

The defect (`bench/v2.1.0/results-shipping-pretasks-v2.1.0-10-g82a2749.md`
§8d-1, `docs/inflight/in-progress/workplan-peer-writer.md` PW1c-9): `CREATE INDEX` builds the
whole index tree and seeds the relation's anchor slot **afterwards**, so a
refusal raised at the seed had already allocated an index tree -- and
nothing in this engine frees a page. A client that keeps retrying therefore
consumes the free map at the rate it can issue statements.

The exit criterion is `map_delta == 0` across a storm of refusals, which
is why this runs in three phases with a clean stop between each: only a
stopped instance has written its free map, and only a phase that builds
*nothing* can be asked whether refusing cost anything.

  phase 1  fill the relation's anchor entry table (679 entries; DROP INDEX
           does not free one, which is what makes the table fill at all)
  phase 2  the storm: every attempt now refuses, for `--seconds`
  phase 3  a control window of the same length doing `SELECT 1`, so the
           instance's own housekeeping is separated from the storm's cost

PASS is phase 2's map delta == 0 -- measured on the free-map bitmaps in the
data file itself, not inferred from its size, because a peer allocates out
of an extent core 0 reserved in advance and the file therefore lags.

    python3 bench/index_refusal_storm_probe.py [--cores N] [--seconds N]
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from multicore_benchmark import Conn, wait_for_port  # noqa: E402

PAGE = 8192
BODY = 32                 # kPageBodyOffset
BITS_PER_REGION = 65280   # kFreeMapBitsPerPage
FREE_MAP_TYPE = 7         # PageType::kFreeMap


def core_of(conn):
    for token in conn.cmd('SHOW META').split():
        if token.startswith('core='):
            return int(token.split('=')[1])
    return None


def session_on(port, core, spare):
    # Non-matching connections are kept open rather than closed: a closed
    # session's core is handed straight back to the next connection, so
    # dropping them would retry the same core forever.
    for _ in range(256):
        candidate = Conn(port)
        if core_of(candidate) == core:
            return candidate
        spare.append(candidate)
    raise RuntimeError(f'no session landed on core {core}')


def map_bits(path):
    """Allocated bits across every free-map region the file holds."""
    with open(path, 'rb') as f:
        data = f.read()
    pages = len(data) // PAGE
    total = 0
    region = 0
    while True:
        map_id = region * BITS_PER_REGION + 1
        if map_id >= pages:
            break
        page = data[map_id * PAGE:(map_id + 1) * PAGE]
        if page[0] != FREE_MAP_TYPE:
            break
        total += sum(bin(b).count('1') for b in page[BODY:])
        region += 1
    return total


class Server:
    """A server over one data file, started and stopped repeatedly."""

    def __init__(self, workdir, port, cores):
        self.workdir = workdir
        self.port = port
        self.cores = cores
        self.db = os.path.join(workdir, 's.db')
        self.conf = os.path.join(workdir, 's.conf')
        self.err = os.path.join(workdir, 's.stderr')
        with open(self.conf, 'w') as f:
            f.write(f"data_file = {self.db}\nport = {port}\ncores = {cores}\n"
                    f"placement = {'creating' if cores == 1 else 'rotate'}\n"
                    f"peer_listeners = {'off' if cores == 1 else 'on'}\n"
                    f"log_file = s.log\nlog_dir = {workdir}\nlog_level = error\n")
        self.proc = None

    def start(self):
        with open(self.err, 'a') as err:
            self.proc = subprocess.Popen(
                [os.path.join(ROOT, 'build-release/kds_server'), '--config', self.conf],
                stdout=err, stderr=subprocess.STDOUT)
        wait_for_port(self.port, self.err)

    def stop(self, conn):
        if conn is not None:
            conn.cmd('STOP')
        try:
            self.proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        self.proc = None

    def kill(self):
        if self.proc is not None:
            self.proc.terminate()
            self.proc.wait(timeout=10)
            self.proc = None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cores', type=int, default=2)
    ap.add_argument('--seconds', type=int, default=30)
    ap.add_argument('--rows', type=int, default=3000)
    ap.add_argument('--port', type=int, default=23100)
    ap.add_argument('--workdir', default='/tmp/kds-index-refusal-storm')
    args = ap.parse_args()

    shutil.rmtree(args.workdir, ignore_errors=True)
    os.makedirs(args.workdir, exist_ok=True)
    server = Server(args.workdir, args.port, args.cores)
    out = {'cores': args.cores, 'seconds': args.seconds}

    # ---- phase 1: fill the anchor entry table -------------------------
    server.start()
    spare = []
    try:
        ddl = session_on(args.port, 0, spare)
        created = ddl.cmd('CREATE TABLE t (id int64, owner varchar, balance int64) BTREE')
        if created.startswith('ERR'):
            raise RuntimeError(created)
        owner = int([x for x in ddl.cmd('DESCRIBE t').split()
                     if x.startswith('owner_core=')][0].split('=')[1])
        out['owner_core'] = owner
        writer = ddl if owner == 0 else session_on(args.port, owner, spare)
        for _ in range(300):
            if not writer.cmd("INSERT INTO t VALUES ('warm', 0)").startswith('ERR'):
                break
            time.sleep(0.01)
        writer.cmd('BEGIN')
        for i in range(args.rows):
            # Checked: a silently failing fill would leave the storm running
            # against an under-populated relation and price the wrong thing.
            reply = writer.cmd(f"INSERT INTO t VALUES ('x', {i})")
            if reply.startswith('ERR'):
                raise RuntimeError(f'fill INSERT {i} refused: {reply[:200]}')
        writer.cmd('COMMIT')

        built = 0
        fill_refusal = None
        while True:
            reply = ddl.cmd(f'CREATE INDEX fill{built} ON t (owner)')
            if reply.startswith('ERR'):
                if 'retryable=1' in reply:
                    time.sleep(0.05)
                    continue
                fill_refusal = reply[:200]
                break
            built += 1
            ddl.cmd(f'DROP INDEX fill{built - 1}')
        out['anchor_filled_after_builds'] = built
        out['fill_refusal'] = fill_refusal
        server.stop(ddl)
    except BaseException:
        server.kill()
        raise

    before = map_bits(server.db)
    out['map_bits_before_storm'] = before

    # ---- phase 2: the storm, where every attempt refuses --------------
    server.start()
    spare = []
    try:
        ddl = session_on(args.port, 0, spare)
        attempts = refused = built = retryable = 0
        spellings = {}
        deadline = time.time() + args.seconds
        while time.time() < deadline:
            reply = ddl.cmd(f'CREATE INDEX storm{attempts} ON t (owner)')
            attempts += 1
            if reply.startswith('ERR'):
                refused += 1
                if 'retryable=1' in reply:
                    retryable += 1
                # The index name is inside the message on the local arm
                # (`WithContext` prefixes), so an un-normalised key would
                # make every refusal its own histogram line.
                key = re.sub(r'storm\d+', 'storm<N>', reply)[:120]
                spellings[key] = spellings.get(key, 0) + 1
            else:
                # A build that succeeds allocates a tree and DROP INDEX
                # orphans it - the reclamation leak, which is gated and is
                # not what this probe measures. It must not happen here.
                built += 1
                ddl.cmd(f'DROP INDEX storm{attempts - 1}')
        out.update(storm_attempts=attempts, storm_refused=refused, storm_built=built,
                   storm_retryable=retryable, storm_spellings=spellings)
        server.stop(ddl)
    except BaseException:
        server.kill()
        raise

    after = map_bits(server.db)
    out['map_bits_after_storm'] = after
    out['map_delta'] = after - before

    # ---- phase 3: the control window ----------------------------------
    server.start()
    spare = []
    try:
        ddl = session_on(args.port, 0, spare)
        idle = 0
        deadline = time.time() + args.seconds
        while time.time() < deadline:
            ddl.cmd('SELECT 1')
            idle += 1
        out['control_statements'] = idle
        server.stop(ddl)
    except BaseException:
        server.kill()
        raise

    out['control_map_delta'] = map_bits(server.db) - after
    # Every mount reserves the peer an extent before it serves anything, so
    # both windows carry that fixed cost and neither is zero. The control
    # window is what prices it: what the storm cost is the *difference*,
    # and that is what must be 0. Reported as three numbers rather than
    # one, because a criterion that quietly subtracted would hide the day
    # the mount cost changes.
    out['storm_marginal_pages'] = out['map_delta'] - out['control_map_delta']

    for key, value in out.items():
        if key == 'storm_spellings':
            for spelling, n in sorted(value.items(), key=lambda kv: -kv[1]):
                print(f'  x{n}: {spelling}')
        else:
            print(f'{key} = {value}')

    # **Every one of these earns its place.** "No pages, no builds, some
    # refusals" is satisfied by any storm that refuses cheaply - including a
    # `TxnConflict` storm (a build window still open, a reply deadline), and
    # including a peer wedged by the `page id not found` defect this branch
    # also fixes, which under `placement = rotate` is where the relation
    # usually lives. So the criterion asserts *which* refusal it measured:
    # zero retryable, and the anchor's own message on every attempt and on
    # the one that ended phase 1.
    full = 'the table is full'
    passed = (out['storm_marginal_pages'] == 0
              and out['storm_built'] == 0
              and out['storm_refused'] > 0
              and out['storm_retryable'] == 0
              and full in (out['fill_refusal'] or '')
              and all(full in spelling for spelling in out['storm_spellings']))
    print('PASS' if passed else 'FAIL')
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
