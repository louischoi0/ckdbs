"""BQ-S2's mount probe for H2 (workorder-bq-wal-segment-roll.md, H2's reject
criterion: a size that adds more than 1 s to the mount). One run: a fresh
volume, a bulk load long enough to fill `--segments` segments, SIGKILL, then
a restart timed from exec to the first accepted connection, with SHOW
META's recovery timings. Prints one JSON line.

usage: mount_probe.py --bin B --workdir D --port P --segment-mib M
                      [--segments 2.5] [--rate-mbps 75]
"""
import argparse, json, os, shutil, signal, socket, subprocess, sys, time

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
    c = ServerConnection('127.0.0.1', port)
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', required=True)
    ap.add_argument('--workdir', required=True)
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--segment-mib', type=int, required=True)
    ap.add_argument('--segments', type=float, default=2.5)
    ap.add_argument('--rate-mbps', type=float, default=75.0)
    a = ap.parse_args()
    shutil.rmtree(a.workdir, ignore_errors=True)
    os.makedirs(a.workdir)
    conf = os.path.join(a.workdir, 's.conf')
    with open(conf, 'w') as f:
        f.write(f'data_file = {a.workdir}/d.db\nport = {a.port}\ncores = 4\ndurability = relaxed\n'
                f'buffer_pool_frames = 131072\nlog_file = s.log\nlog_dir = {a.workdir}\n'
                f'log_level = info\nreactor_cpus = 0,2,4,6\n')
    cmd = ['taskset', '-c', '0,2,4,6', a.bin, '--config', conf]
    err = open(os.path.join(a.workdir, 's.stderr'), 'w')
    p = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
    if not wait_port(a.port, time.time() + 60):
        p.kill()
        raise SystemExit('first start did not listen')
    c = ServerConnection('127.0.0.1', a.port)
    c.send_command('CREATE TABLE mp (id int64, a int64, b int64, c int64)')
    load_s = a.segments * a.segment_mib / a.rate_mbps
    m0 = meta(a.port)
    t_end = time.time() + load_s
    n = 0
    while time.time() < t_end:
        rows = ', '.join(f'({n + i + 1}, {i}, {i}, {i})' for i in range(1024))
        r = c.send_command(f'INSERT INTO mp VALUES {rows}')
        if r.startswith('ERR'):
            raise SystemExit(r)
        n += 1024
    m1 = meta(a.port)
    c.close()
    p.send_signal(signal.SIGKILL)
    p.wait()
    t0 = time.monotonic()
    p = subprocess.Popen(cmd, stdout=err, stderr=subprocess.STDOUT)
    up = wait_port(a.port, time.time() + 600)
    mount_s = time.monotonic() - t0
    m2 = meta(a.port) if up else {}
    p.terminate()
    try:
        p.wait(timeout=60)
    except subprocess.TimeoutExpired:
        p.kill()
    rec = {k: v for k, v in m2.items() if k.startswith('recovery_')}
    print(json.dumps({'segment_mib': a.segment_mib, 'rows': n, 'load_s': load_s,
                      'log_bytes': m1.get('wal_append_lsn', 0) - m0.get('wal_append_lsn', 0),
                      'rolls': m1.get('wal_rolls', 0), 'mount_s': round(mount_s, 3),
                      'listened': up, 'recovery': rec}))
    shutil.rmtree(a.workdir, ignore_errors=True)


main()
