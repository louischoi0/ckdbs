"""BQ-S2's mount probe v2 for H2's mount term (a size that adds more than
1 s to the mount). v1 crashed the server and timed the whole restart, so
recovery's redo and completion checkpoint swamped the tail scan, and its
5 s client timeout lost the 1 GiB arm to one stalled INSERT. v2: a fresh
volume, a bulk load of `--segments` segments with a 120 s client timeout
(the longest statement is recorded - a roll building a body inline stalls
it), a **clean** STOP through the debug text port, then a restart timed
from exec to the first accepted connection, with SHOW META's recovery
timings. What differs between sizes is then mostly `WalStream::ScanTail`,
which reads the last segment's body. Prints one JSON line.

usage: mount_probe2.py --bin B --workdir D --port P --segment-mib M [--segments 2.5]
"""
import argparse, json, os, shutil, socket, subprocess, sys, time

sys.path.insert(0, '/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue/tools')
from ckdbs_cli import ServerConnection  # noqa: E402


def wait_port(port, deadline):
    while time.time() < deadline:
        s = socket.socket()
        try:
            if s.connect_ex(('127.0.0.1', port)) == 0:
                return True
        finally:
            s.close()
        time.sleep(0.005)
    return False


def meta(port):
    c = ServerConnection('127.0.0.1', port, timeout=120.0)
    r = c.send_command('SHOW META')
    c.close()
    out = {}
    for tok in r.replace('\n', ' ').split():
        if '=' in tok:
            k, v = tok.split('=', 1)
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v
    return out


def text_stop(port):
    s = socket.create_connection(('127.0.0.1', port), timeout=120)
    s.sendall(b'STOP\n')
    try:
        s.recv(4096)
    except OSError:
        pass
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', required=True)
    ap.add_argument('--workdir', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--segment-mib', type=int, required=True)
    ap.add_argument('--segments', type=float, default=2.5)
    a = ap.parse_args()
    shutil.rmtree(a.workdir, ignore_errors=True)
    os.makedirs(a.workdir)
    conf = os.path.join(a.workdir, 's.conf')
    with open(conf, 'w') as f:
        f.write(f'data_file = {a.workdir}/d.db\nport = {a.port}\ndebug_text_port = {a.port + 1}\n'
                f'cores = 4\ndurability = relaxed\nbuffer_pool_frames = 131072\nlog_file = s.log\n'
                f'log_dir = {a.workdir}\nlog_level = info\nreactor_cpus = 0,2,4,6\n')
    cmd = ['taskset', '-c', '0,2,4,6', a.bin, '--config', conf]
    err = open(os.path.join(a.workdir, 's.stderr'), 'w')
    p = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
    if not wait_port(a.port, time.time() + 60):
        p.kill()
        raise SystemExit('first start did not listen')
    c = ServerConnection('127.0.0.1', a.port, timeout=120.0)
    c.send_command('CREATE TABLE mp (id int64, a int64, b int64, c int64)')
    m0 = meta(a.port)
    target = a.segments * a.segment_mib * 1024 * 1024
    n, longest = 0, 0.0
    while True:
        rows = ', '.join(f'({n + i + 1}, {i}, {i}, {i})' for i in range(1024))
        t0 = time.monotonic()
        r = c.send_command(f'INSERT INTO mp VALUES {rows}')
        longest = max(longest, time.monotonic() - t0)
        if r.startswith('ERR'):
            raise SystemExit(r)
        n += 1024
        if n % (1024 * 64) == 0 and meta(a.port).get('wal_append_lsn', 0) - m0.get('wal_append_lsn', 0) >= target:
            break
    m1 = meta(a.port)
    c.close()
    text_stop(a.port + 1)
    p.wait(timeout=600)
    t0 = time.monotonic()
    p = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
    up = wait_port(a.port, time.time() + 600)
    mount_s = time.monotonic() - t0
    m2 = meta(a.port) if up else {}
    text_stop(a.port + 1)
    try:
        p.wait(timeout=600)
    except subprocess.TimeoutExpired:
        p.kill()
    rec = {k: v for k, v in m2.items() if k.startswith('recovery_')}
    print(json.dumps({'segment_mib': a.segment_mib, 'rows': n,
                      'log_bytes': m1.get('wal_append_lsn', 0) - m0.get('wal_append_lsn', 0),
                      'rolls': m1.get('wal_rolls', 0), 'longest_insert_s': round(longest, 3),
                      'mount_s': round(mount_s, 3), 'listened': up, 'recovery': rec}))
    shutil.rmtree(a.workdir, ignore_errors=True)


PROCS = []
_popen = subprocess.Popen


def _tracked(*args, **kwargs):
    proc = _popen(*args, **kwargs)
    PROCS.append(proc)
    return proc


subprocess.Popen = _tracked
try:
    main()
finally:
    # Whatever ended the probe, no server outlives it on the port (v1 left
    # one listening after a client timeout, and the next probes talked to it).
    for proc in PROCS:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
