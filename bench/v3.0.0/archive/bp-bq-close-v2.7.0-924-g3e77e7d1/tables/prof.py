import re, json
D = '/home/cdkbs/bench-runs/close/out/profile'


def load(f):
    return open(f).read().split('=== ')[1:]


def parse(part):
    m = {}
    for line in part.strip().splitlines():
        mm = re.match(r'\s*([\d.]+)%\s+(\d+)\s+(.*)', line)
        if mm:
            m[mm.group(3)] = int(mm.group(2))
    return m


secs = load(D + '/report.txt') + load(D + '/report2.txt')
res = {}
for side in ('pre', 'post', 'fix', 'close'):
    N = 0
    slf = {}
    inc = {}
    for s in secs:
        if s.startswith(side + ' '):
            N += int(re.search(r'samples (\d+)', s).group(1))
            a = s.split('-- self --')[1].split('-- inclusive --')
            for k, v in parse(a[0]).items():
                slf[k] = slf.get(k, 0) + v
            for k, v in parse(a[1]).items():
                inc[k] = inc.get(k, 0) + v
    res[side] = (N, slf, inc)
names = ['kds::wal::WalManager::Append', 'kds::wal::WalStream::Append',
         'kds::txn::UndoLog::LogUndoWrite', 'kds::txn::TransactionManager::NoteOverwrite',
         'kds::server::CommandDispatcher::UpdateInner::{lambda#1}::operator const',
         'std::_Hashtable::find [clone .isra.0]', '__libc_pwrite64']
print('side N ' + ' | '.join(n[-30:] for n in names))
for side, (N, slf, inc) in res.items():
    print(side, N, ' | '.join('self %.2f%% incl %.2f%% (%d/%d)' % (100 * slf.get(n, 0) / N, 100 * inc.get(n, 0) / N, slf.get(n, 0), inc.get(n, 0)) for n in names))
for side in ('pre', 'post', 'fix', 'close'):
    for l in open(f'{D}/{side}.log'):
        if l.startswith('{') and '"prof"' in l:
            d = json.loads(l)
            print(side, d['path'], 'ops', d['ops'], 'server_cpu_us %.1f' % d['server_cpu_us'], 'p50 %.1f' % d['p50_us'], 'p0 %.1f' % d['p0_us'])
