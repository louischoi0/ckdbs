"""BQ-S1's measurement matrix (workorder-bq-wal-segment-roll.md section 5).

Runs, interleaved and gated per cell:
  phase 'bl'  - BL's owed ruling-1 re-measurement (BQ-Q1 (a)): C1 mixed at
                three placements, 05cd011f against 3338cf0c, 3 runs.
  phase 'bq'  - C1 mixed and isolated at three placements, C2 and its
                isolated arm, C3; relaxed and group; N runs.

Every cell waits for a quiet host first: no compiler, cmake build, ctest or
test binary, no BP run, and a one-minute load under the bound. Each cell's
host stamp goes into <out>/hoststamps.log. Writes <out>/<phase>.done.

usage: python3 bq_s1_matrix.py <phase> <out_dir> <bq_bin> [runs]
"""
import json
import os
import subprocess
import sys
import time

W = '/home/cdkbs/ckdbs/.claude/worktrees/wal-segment-roll-order'
DRIVER = W + '/tools/bl_mixed.py'
BL_BIN = '/home/cdkbs/bench-runs/bl/bin/kds_server.'
LOAD_BOUND = 2.5
BUSY = ('cc1plus', 'cmake --build', 'ctest', 'kds_tests', 'gtest', 'bench-runs/bp-s1',
        'bi_checkpoint_stall', 'cell3')

COMMON = ['--cores', '4', '--seconds', '30', '--warmup', '2', '--runs', '1', '--max-wait', '0',
          '--server-cpus', '0,2,4,6', '--client-cpus', '1,3,5,7', '--frames', '131072',
          '--log-level', 'info', '--ctl-core', '0']


def busy_procs():
    out = subprocess.run(['pgrep', '-af', '|'.join(BUSY)], capture_output=True, text=True).stdout
    me = str(os.getpid())
    # A session's shell wrapper (`bash -c source ...shell-snapshots...`) only
    # names the work in its argv; the work itself is its own process, so the
    # wrapper never holds the gate (a waiter matching its own pattern is how
    # an earlier census chain deadlocked).
    return [l for l in out.splitlines() if l and not l.startswith(me + ' ') and 'pgrep' not in l
            and 'bq_s1_matrix' not in l and 'shell-snapshots' not in l]


def gate(stamps):
    waited = 0
    while True:
        load1 = float(open('/proc/loadavg').read().split()[0])
        procs = busy_procs()
        if not procs and load1 < LOAD_BOUND:
            break
        # The watchdog's heartbeat while the gate holds.
        with open(os.path.dirname(stamps.name) + '/gate.beat', 'w') as beat:
            beat.write(f'{time.time()} {load1} {len(procs)}\n')
        time.sleep(10)
        waited += 10
    stamps.write(json.dumps({'t': time.time(), 'loadavg': open('/proc/loadavg').read().strip(),
                             'gate_waited_s': waited}) + '\n')
    stamps.flush()


def cell(bin_path, workdir, out, port, extra, stamps):
    # A resumed chain (BQ_RESUME=1, the watchdog's relaunch) skips a cell
    # whose driver already wrote its result.
    if os.environ.get('BQ_RESUME') and any(f.endswith('.json') for f in
                                           (os.listdir(out) if os.path.isdir(out) else [])):
        return
    gate(stamps)
    cmd = ['python3', DRIVER, '--bin', bin_path, '--port', str(port), '--workdir', workdir,
           '--out', out] + COMMON + extra
    os.makedirs(out, exist_ok=True)
    with open(out + '/console.log', 'a') as f:
        f.write(' '.join(cmd) + '\n')
        f.flush()
        rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT)
    stamps.write(json.dumps({'t': time.time(), 'after': out, 'rc': rc,
                             'loadavg': open('/proc/loadavg').read().strip()}) + '\n')
    stamps.flush()


C1_PLACEMENTS = ('segregated-no0', 'batch0', 'spread')


def phase_bl(out_root, runs, stamps):
    for r in range(1, runs + 1):
        for pl in C1_PLACEMENTS:
            for b in ('3338cf0c', '05cd011f'):
                cell(BL_BIN + b, f'/home/cdkbs/bench-runs/bq/work-bl-{b}',
                     f'{out_root}/bl/{b}/{pl}/r{r}', 17531,
                     ['--arm', 'mixed', '--readers', '6', '--rate', '400', '--placement', pl,
                      '--durability', 'relaxed', '--seconds', '10', '--frames', '65536'],
                     stamps)  # BL-S1's own cell length and pool, so the pair compares


def phase_bq(out_root, bq_bin, runs, stamps):
    wd = '/home/cdkbs/bench-runs/bq/work-bq'
    for r in range(1, runs + 1):
        for dur in ('relaxed', 'group'):
            base = f'{out_root}/bq/{dur}'
            d = ['--durability', dur]
            cell(bq_bin, wd, f'{base}/c3/r{r}', 17541,
                 ['--arm', 'insert', '--readers', '0', '--writers', '0',
                  '--placement', 'segregated'] + d, stamps)
            cell(bq_bin, wd, f'{base}/c2/r{r}', 17541,
                 ['--arm', 'insert', '--readers', '0', '--writers', '8', '--writer-rate', '800',
                  '--placement', 'segregated'] + d, stamps)
            cell(bq_bin, wd, f'{base}/c2-iso/r{r}', 17541,
                 ['--arm', 'isolated', '--readers', '0', '--writers', '8', '--writer-rate', '800',
                  '--placement', 'segregated'] + d, stamps)
            for pl in C1_PLACEMENTS:
                cell(bq_bin, wd, f'{base}/c1-{pl}/r{r}', 17541,
                     ['--arm', 'mixed', '--readers', '6', '--rate', '400', '--placement', pl] + d,
                     stamps)
                cell(bq_bin, wd, f'{base}/c1-iso-{pl}/r{r}', 17541,
                     ['--arm', 'isolated', '--readers', '6', '--rate', '400', '--placement', pl] + d,
                     stamps)


def phase_smoke(out_root, bq_bin, stamps):
    # A short C2 and C3 at relaxed: the driver's new paths against a real
    # server before the long matrix spends its hour.
    wd = '/home/cdkbs/bench-runs/bq/work-smoke'
    cell(bq_bin, wd, f'{out_root}/smoke/c3', 17551,
         ['--arm', 'insert', '--readers', '0', '--writers', '0', '--placement', 'segregated',
          '--durability', 'relaxed', '--seconds', '6'], stamps)
    cell(bq_bin, wd, f'{out_root}/smoke/c2', 17551,
         ['--arm', 'insert', '--readers', '0', '--writers', '8', '--writer-rate', '800',
          '--placement', 'segregated', '--durability', 'relaxed', '--seconds', '6'], stamps)


def main():
    phase, out_root = sys.argv[1], sys.argv[2]
    bq_bin = sys.argv[3] if len(sys.argv) > 3 else ''
    runs = int(sys.argv[4]) if len(sys.argv) > 4 else 3
    os.makedirs(out_root, exist_ok=True)
    with open(out_root + '/hoststamps.log', 'a') as stamps:
        if phase == 'smoke':
            phase_smoke(out_root, bq_bin, stamps)
        elif phase == 'bl':
            phase_bl(out_root, runs, stamps)
        elif phase == 'bq':
            phase_bq(out_root, bq_bin, runs, stamps)
    open(f'{out_root}/{phase}.done', 'w').write('done\n')


main()
