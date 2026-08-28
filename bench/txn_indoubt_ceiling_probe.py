#!/usr/bin/env python3
"""R6-R cells R3-R4: `in_doubt_ceiling_ms` swept against both axes CR3
names - a writer's stall (R3) and how much WAL a prepared transaction's
floor holds back from truncation (R4).

`instructions/v2.5.0/cross-owner-protocol-closing.md` §7. D5's ratified
bound: a writer that conflicts with a row a *prepared-but-undecided*
cross-owner transaction holds blocks until the transaction is decided or
`in_doubt_ceiling_ms` elapses, whichever comes first
(`command_dispatcher.cpp:249-296`, `DispatchAndStage`'s `in_doubt_block`
loop). RP2's review added a second consumer of the same fact: the
checkpoint floors its redo start at the oldest still-prepared transaction's
LSN (`Checkpointer::Start`, `checkpointer.cpp:135-149`,
`ActiveTransactions::OldestPreparedLsn`), so whatever holds a transaction
prepared also holds that many bytes of WAL un-truncatable.

**Why this cannot be a kill -9 probe.** Both sides of the wire are threads
of one process (`bench/docs/README.md`'s note on
`txn_2pc_kill_matrix_probe.py`), and a participant's own recovery reads its
coordinator's *already-durable* stream at mount, before the instance serves
a connection (`prepared_resolver.hpp`) - so a transaction killed prepared
resolves during the restart that would otherwise let this driver observe
it, and never sits live and in-doubt after one. The only live window this
engine ever produces is the ordinary one: the gap between a participant's
own prepare-durability and its coordinator's decide, which exists on every
cross-owner commit and is normally a couple of milliseconds wide (RP8's B1,
`bench/v2.5.0/results-r6b-cross-owner-cost-*.md` §5b: xowner-2 p50 ~2.6 ms).
This driver races a writer against exactly that natural window, at swept
ceiling values that bracket it, rather than manufacturing a longer one -
manufacturing one would need an engine-side delay hook this repo does not
have and this agent may not add.

Two modes:

  **live**     A `holder` connection loops a genuine `--participants`-owner
               cross-owner transaction (`BEGIN; UPDATE t0 ...; UPDATE t1
               ...; ...; COMMIT`) against one fixed row on each of `t0..
               t(N-1)`, one row per peer core. Concurrently, a `racer`
               connection **seated on t0's owner core** (so its write is
               local, never shipped) repeats a plain autocommit
               `UPDATE t0 ... WHERE id = <pk>` against `t0`'s row, timed
               per attempt, for the same wall-clock window. Every racer
               attempt is classified: succeeded, refused by D5's ceiling
               (the exact message `command_dispatcher.cpp:283-289`
               writes), or refused for any other reason (almost always an
               *ordinary* write-write conflict - `t0`'s row is uncommitted,
               visibly written, but not yet *prepared*, which is a wider
               window than "prepared and undecided" and is not what D5's
               ceiling governs).

               **Why more than 2 participants.** `t0` is the first table
               the holder writes, so it is usually the first to reach
               `TXN_PREPARE`; with 2 participants the coordinator then has
               only one more prepare reply to wait for before it can
               decide, so `t0`'s own "prepared, undecided" window is short
               (RP8's B1: xowner-2 p50 ~2.6 ms end to end, and prepare is
               only part of that). Raising `--participants` makes the
               coordinator wait on more peers' prepare replies before it
               can decide anything, which widens exactly the window this
               cell exists to race against, without any engine-side delay
               hook - it is the same natural mechanism RP8's B2 measured
               (width cost rising past N=4 on this host).
  **control**  The same duration, the same row, but the holder writes it
               with a plain **local** autocommit `UPDATE` on core 1 -
               never `BEGIN`, never a second owner, so no participant is
               ever prepared and `OldestPreparedLsn()` stays 0 throughout.
               This is R4's "what would redo_start be with nothing
               prepared" baseline; it takes no `--ceiling` or
               `--participants` sweep because neither knob has anything to
               act on.

**R4's measurement.** Every server runs `log_level = debug` and a short
`checkpoint_interval_ms`, so every checkpoint publishes an anchor naming
its core (`INFO [checkpoint] anchor published: core=<N> checkpoint_lsn=...
redo_start=... durable_lsn=... segment=...`,
`remote_checkpoint_anchor.cpp`/`superblock_checkpoint_anchor.cpp`). An LSN
is a stream-local **byte offset** (`wal/record.hpp:29`) and `durable_lsn`
is the log's current durable position in that same unit, so
`retained_bytes = durable_lsn - redo_start` from one anchor line needs no
conversion and no second measurement. (A first cut of this driver summed
WAL segment file sizes instead; segments are pre-allocated to
`segment_size` and `stat().st_size` reports that constant regardless of
how much is actually written, which overstated retained bytes by roughly
64 MiB per segment - the anchor line is what replaced it.)

    python3 bench/txn_indoubt_ceiling_probe.py --mode live --ceiling-ms 50 \
        --duration 3 --server build-release/kds_server --json out.json
    python3 bench/txn_indoubt_ceiling_probe.py --mode control --duration 3 \
        --server build-release/kds_server --json control.json
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from multicore_benchmark import Conn, field, wait_for_port  # noqa: E402

DOUBT_REFUSAL = 'was refused rather than waiting longer'


def percentiles(values):
    if not values:
        return {}
    s = sorted(values)

    def at(p):
        return s[min(len(s) - 1, int(len(s) * p))]

    return {'p0': round(s[0], 1), 'p25': round(at(0.25), 1),
            'p50': round(at(0.50), 1), 'p95': round(at(0.95), 1),
            'p99': round(at(0.99), 1), 'n': len(s),
            'mean': round(sum(s) / len(s), 1)}


def write_conf(workdir, port, ceiling_ms, checkpoint_ms, cores):
    conf = os.path.join(workdir, 's.conf')
    with open(conf, 'w') as f:
        f.write(f"data_file = {os.path.join(workdir, 's.db')}\n"
                f"port = {port}\ncores = {cores}\n"
                f"placement = rotate\n"
                f"peer_listeners = on\n"
                f"durability = group\n"
                f"in_doubt_ceiling_ms = {ceiling_ms}\n"
                f"checkpoint_interval_ms = {checkpoint_ms}\n"
                f"log_file = s.log\nlog_dir = {workdir}\nlog_level = debug\n")
    return conf


def session_on_core(port, core, spare, tries=256):
    for _ in range(tries):
        c = Conn(port)
        if field(c.cmd('SHOW META'), 'core') == core:
            return c
        spare.append(c)
    raise RuntimeError(f'no session landed on core {core}')


def build_relations(c0, n):
    """`n` relations, one owner core each (`rotate` never repeats a core
    until every peer has one, same as `txn_2pc_cost_probe.py`'s
    `build_relations`)."""
    owners = {}
    for i in range(n):
        name = f't{i}'
        created = c0.cmd(f'CREATE TABLE {name} (id int64, tag varchar, n int64) BTREE')
        if created.startswith('ERR'):
            raise RuntimeError(f'CREATE {name}: {created[:200]}')
        owners[name] = field(c0.cmd(f'DESCRIBE {name}'), 'owner_core')
    if len(set(owners.values())) != n:
        raise RuntimeError(f'{n} tables did not land on {n} distinct cores: {owners}')
    return owners


def seed_row(c0, table, retries=200):
    r = None
    for _ in range(retries):
        r = c0.cmd(f"INSERT INTO {table} VALUES ('seed', 0)")
        if not (r.startswith('ERR') and 'retryable=1' in r):
            break
        time.sleep(0.01)
    if r.startswith('ERR'):
        raise RuntimeError(f'seed {table}: {r[:200]}')
    return field(r, 'id')


def holder_loop_live(conn, tables, pks, stop_at, counters):
    """Repeated N-owner cross-owner transactions on the SAME N rows,
    retried as a unit on a retryable refusal (the racer contending for
    `tables[0]`'s row is exactly what can produce one)."""
    while time.perf_counter() < stop_at:
        r = conn.cmd('BEGIN')
        if r.startswith('ERR'):
            continue
        failed = False
        for t, pk in zip(tables, pks):
            r = conn.cmd(f"UPDATE {t} SET n = 1 WHERE id = {pk}")
            if r.startswith('ERR'):
                failed = True
                break
        if failed:
            conn.cmd('ROLLBACK')
            counters['holder_conflicts'] += 1
            continue
        r = conn.cmd('COMMIT')
        if r.startswith('ERR'):
            counters['holder_conflicts'] += 1
            continue
        counters['holder_committed'] += 1


def holder_loop_control(conn, pk0, stop_at, counters):
    """The R4 baseline: the same row, written locally, never a participant."""
    while time.perf_counter() < stop_at:
        r = conn.cmd(f"UPDATE t0 SET n = 1 WHERE id = {pk0}")
        if r.startswith('ERR'):
            counters['holder_conflicts'] += 1
        else:
            counters['holder_committed'] += 1


def racer_loop(conn, pk0, stop_at, results):
    while time.perf_counter() < stop_at:
        start = time.perf_counter()
        r = conn.cmd(f"UPDATE t0 SET n = 0 WHERE id = {pk0}")
        us = (time.perf_counter() - start) * 1e6
        if not r.startswith('ERR'):
            results.append(('ok', us, None))
        elif DOUBT_REFUSAL in r:
            results.append(('doubt', us, r[:200]))
        else:
            results.append(('other', us, r[:200]))


def read_log(workdir):
    path = os.path.join(workdir, 's.log')
    try:
        with open(path, errors='replace') as f:
            return f.read()
    except OSError:
        return ''


def anchor_series(log_text, core):
    """Every `anchor published: core=<N> checkpoint_lsn=... redo_start=...
    durable_lsn=... segment=...` line for `core`
    (`remote_checkpoint_anchor.cpp`/`superblock_checkpoint_anchor.cpp`'s
    published form). Explicitly core-tagged, unlike the `Checkpointer`'s
    own "started:"/"checkpoint complete:" debug lines - which is why this
    is what the driver reads rather than those.

    `durable_lsn` is "current position" and `redo_start` is the floor;
    both are the same byte-offset unit an LSN is (`wal/record.hpp:29`), so
    `durable_lsn - redo_start` is bytes retained with no conversion,
    **and it is the durable log position, not a segment file's allocated
    size** - segment files are pre-allocated to `segment_size` and
    `stat().st_size` is that constant regardless of how much of the
    segment is actually written, which this driver's first cut got wrong
    (67,108,864 = 64 MiB on a run that had written roughly 1 MiB).

    **The peak, not the last tick, is what R4 asks about.** A run's *final*
    anchor is read after the holder and racer have both stopped, by which
    point every transaction that was ever prepared has long since decided -
    so the last tick alone undercounts systematically (a first cut of this
    driver reported 80 bytes retained on every cell, live or control,
    because it read only the last line). The **maximum** gap over every
    tick during the run is what a checkpoint actually had to carry at its
    worst moment, which is the quantity `docs/spec/wal.md` §11-3's floor is
    a bound on."""
    pat = re.compile(
        rf'anchor published: core={core} checkpoint_lsn=(\d+) redo_start=(\d+) '
        rf'durable_lsn=(\d+) segment=(\d+)')
    matches = pat.findall(log_text)
    if not matches:
        return None
    ticks = [{'checkpoint_lsn': int(cl), 'redo_start': int(rs), 'durable_lsn': int(dl),
              'segment': int(seg), 'retained_bytes': int(dl) - int(rs)}
             for cl, rs, dl, seg in matches]
    peak = max(ticks, key=lambda t: t['retained_bytes'])
    return {'ticks_seen': len(ticks), 'last': ticks[-1], 'peak': peak,
            'peak_retained_bytes': peak['retained_bytes'],
            'last_retained_bytes': ticks[-1]['retained_bytes']}


def run_cell(args):
    n = args.participants if args.mode == 'live' else 2
    label = f'{args.mode}-{args.ceiling_ms}-p{n}' if args.mode == 'live' else 'control'
    workdir = os.path.join(args.workdir, label)
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)

    cores = n + 1
    conf = write_conf(workdir, args.port, args.ceiling_ms, args.checkpoint_ms, cores)
    out = {'mode': args.mode, 'ceiling_ms': args.ceiling_ms, 'duration_s': args.duration,
           'participants': n, 'cores': cores}

    err = open(os.path.join(workdir, 's.stderr'), 'a')
    proc = subprocess.Popen([args.server, '--config', conf], stdout=err,
                            stderr=subprocess.STDOUT)
    spare = []
    try:
        wait_for_port(args.port, os.path.join(workdir, 's.stderr'))
        c0 = session_on_core(args.port, 0, spare)
        owners = build_relations(c0, n)
        out['owners'] = owners
        tables = [f't{i}' for i in range(n)]
        owner_core0 = owners['t0']
        pks = [seed_row(c0, t) for t in tables]
        out['pks'] = dict(zip(tables, pks))
        pk0 = pks[0]

        holder = c0 if args.mode == 'live' else session_on_core(args.port, owner_core0, spare)
        racer = session_on_core(args.port, owner_core0, spare)

        counters = {'holder_committed': 0, 'holder_conflicts': 0}
        results = []
        stop_at = time.perf_counter() + args.duration
        if args.mode == 'live':
            ht = threading.Thread(target=holder_loop_live, args=(holder, tables, pks, stop_at, counters))
        else:
            ht = threading.Thread(target=holder_loop_control, args=(holder, pk0, stop_at, counters))
        rt = threading.Thread(target=racer_loop, args=(racer, pk0, stop_at, results))
        ht.start()
        rt.start()
        ht.join(timeout=args.duration + 15)
        rt.join(timeout=args.duration + 15)

        out['holder_committed'] = counters['holder_committed']
        out['holder_conflicts'] = counters['holder_conflicts']
        out['racer_attempts'] = len(results)
        by_kind = {'ok': [], 'doubt': [], 'other': []}
        for kind, us, _ in results:
            by_kind[kind].append(us)
        out['racer_by_kind_n'] = {k: len(v) for k, v in by_kind.items()}
        out['racer_all_us'] = percentiles([us for _, us, _ in results])
        out['racer_ok_us'] = percentiles(by_kind['ok'])
        out['racer_doubt_us'] = percentiles(by_kind['doubt'])
        sample_other = next((r for k, _, r in results if k == 'other'), None)
        if sample_other:
            out['sample_other_refusal'] = sample_other

        # Let the checkpoint cadence catch up before the last sample: the
        # config's own interval, plus one grace period.
        time.sleep(args.checkpoint_ms / 1000.0 * 2 + 0.2)

        log_text = read_log(workdir)
        anchor0 = anchor_series(log_text, 0)
        anchor1 = anchor_series(log_text, owner_core0)
        out['core0_anchor'] = anchor0
        out['core0_peak_retained_bytes'] = anchor0['peak_retained_bytes'] if anchor0 else None
        out['core0_last_retained_bytes'] = anchor0['last_retained_bytes'] if anchor0 else None
        out['owner_core'] = owner_core0
        out['owner_anchor'] = anchor1
        out['owner_peak_retained_bytes'] = anchor1['peak_retained_bytes'] if anchor1 else None
        out['owner_last_retained_bytes'] = anchor1['last_retained_bytes'] if anchor1 else None

        c0.cmd('STOP')
        try:
            proc.wait(timeout=60)
        except subprocess.TimeoutExpired:
            out['stop_timed_out'] = True
    finally:
        for c in spare:
            c.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=10)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', choices=('live', 'control'), required=True)
    ap.add_argument('--ceiling-ms', type=int, default=200)
    ap.add_argument('--participants', type=int, default=4)
    ap.add_argument('--checkpoint-ms', type=int, default=300)
    ap.add_argument('--duration', type=float, default=3.0)
    ap.add_argument('--port', type=int, default=22980)
    ap.add_argument('--server', default=os.path.join(ROOT, 'build-release/kds_server'))
    ap.add_argument('--workdir', default=os.path.expanduser('~/kds-rr-indoubt'))
    ap.add_argument('--json', default=None)
    args = ap.parse_args()

    out = run_cell(args)
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(out, f, indent=2)
    print(json.dumps(out, indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())
