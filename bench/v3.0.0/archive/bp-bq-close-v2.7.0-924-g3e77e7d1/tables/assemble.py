import os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
for g in ('gen1.py', 'gen2.py', 'gen3.py', 'gen4.py'):
    subprocess.run(['python3', '-I', os.path.join(HERE, g)], capture_output=True, check=True)
txt = open(HERE + '/template.md').read() + open(HERE + '/template2.md').read()
now = subprocess.run(['date', '-u', '+%H:%M'], capture_output=True, text=True).stdout.strip()
txt = txt.replace('20:3x-21:xx UTC', f'20:24-{now} UTC')
missing = []


def sub(m):
    name = m.group(1)
    p = f'{HERE}/frag_{name}.md'
    if not os.path.exists(p):
        missing.append(name)
        return m.group(0)
    return open(p).read().rstrip('\n')


txt = re.sub(r'\{\{(\w+)\}\}', sub, txt)
out = '/home/cdkbs/ckdbs/.claude/worktrees/bp-per-core-residue/bench/v3.0.0/results-bp-bq-close-v2.7.0-924-g3e77e7d1.md'
open(out, 'w').write(txt)
print('written', out, len(txt.splitlines()), 'lines; missing', missing)
