"""Resolve {{run:script args}} and {{sect:script args|heading}} placeholders in a template."""
import re, subprocess, sys

S = __import__('os').path.dirname(__import__('os').path.abspath(__file__)) + '/'
cache = {}


def run(spec):
    if spec not in cache:
        parts = spec.split()
        out = subprocess.run(['nice', 'python3', '-I', S + parts[0]] + parts[1:], capture_output=True, text=True)
        if out.returncode != 0:
            raise SystemExit(f'{spec}: {out.stderr}')
        cache[spec] = out.stdout
    return cache[spec]


def sect(spec, heading):
    text = run(spec)
    cur, buf, secs = None, [], {}
    for line in text.splitlines():
        if line.startswith('#'):
            if cur is not None:
                secs[cur] = '\n'.join(buf).strip()
            cur, buf = line.lstrip('# ').strip(), []
        else:
            buf.append(line)
    if cur is not None:
        secs[cur] = '\n'.join(buf).strip()
    if heading not in secs:
        raise SystemExit(f'{spec}: no section {heading!r}; have {list(secs)}')
    return secs[heading]


tpl = open(sys.argv[1]).read()


def sub(m):
    kind, body = m.group(1), m.group(2)
    if kind == 'run':
        return run(body).strip()
    spec, heading = body.split('|', 1)
    return sect(spec, heading)


out = re.sub(r'\{\{(run|sect):([^}]*)\}\}', sub, tpl)
open(sys.argv[2], 'w').write(out)
print('wrote', sys.argv[2], len(out.splitlines()), 'lines')
