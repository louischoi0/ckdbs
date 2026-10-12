"""Full self/inclusive shares per dump (top 400), via tools/kds_sampler_report.py; analysis only."""
import re, subprocess, sys, json

REP = '/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue/tools/kds_sampler_report.py'
D = '/home/cdkbs/bench-runs/close/out/profile'


def parse(part):
    m = {}
    for line in part.strip().splitlines():
        mm = re.match(r'\s*([\d.]+)%\s+(\d+)\s+(.*)', line)
        if mm:
            m[mm.group(3)] = int(mm.group(2))
    return m


res = {}
for side in ('pre', 'post', 'fix', 'close'):
    res[side] = []
    for i in (0, 1):
        txt = subprocess.run(['python3', '-I', REP, f'{D}/{side}/dump.{i}', '--top', '400'],
                             capture_output=True, text=True).stdout
        n = int(re.search(r'samples (\d+)', txt).group(1))
        a = txt.split('-- self --')[1].split('-- inclusive --')
        res[side].append((n, parse(a[0]), parse(a[1])))
json.dump({s: [(n, sl, inc) for n, sl, inc in v] for s, v in res.items()}, open(sys.argv[1], 'w'))
names = ['kds::wal::WalManager::Append', 'kds::wal::WalStream::Append', 'kds::wal::WalManager::AppendRecord',
         'kds::txn::UndoLog::LogUndoWrite', 'kds::txn::TransactionManager::NoteOverwrite']
for nm in names:
    print(nm)
    for side, v in res.items():
        print('  ', side, 'self', [(sl.get(nm, 0), n) for n, sl, inc in v], 'incl', [(inc.get(nm, 0), n) for n, sl, inc in v])
