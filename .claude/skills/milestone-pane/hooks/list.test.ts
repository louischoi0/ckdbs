import { test, expect } from 'claude-code/testing'

const orders: Record<string, string> = {
  'instructions/v3.0.0/workorder-zz-open.md': [
    '# Work order ZZ — an open order',
    '| ZZ-S0 | **The order** | x | S |',
    '| ZZ-S1 | **The premise** | x | M |',
    '### ZZ-S0 — written 2026-10-12',
  ].join('\n'),
  'instructions/v3.0.0/workorder-xx-older.md': [
    '# Work order XX — an older open order',
    '| XX-S0 | **The order** | x | S |',
  ].join('\n'),
  'instructions/v3.0.0/workorder-yy-done.md': [
    '# Work order YY — a closed order',
    '| YY-S0 | **The order** | x | S |',
    '### YY-S0 — written 2026-10-11',
  ].join('\n'),
}
const files = `${Object.keys(orders).join('\n')}\n`
// git's last-touched order puts ZZ first; XX was added earlier.
const added: Record<string, string> = { 'instructions/v3.0.0/workorder-zz-open.md': '200', 'instructions/v3.0.0/workorder-xx-older.md': '100' }

test('/milestone list names every work order with a stage open, oldest registered first, and no closed one', async ($, on) => {
  const opened: string[] = []
  on('session.root', () => ({ value: '/repo' }))
  on('process.run', (_$, e) => ({ value: { stdout: e.argv.includes('--diff-filter=A') ? `${added[e.argv[e.argv.length - 1] ?? ''] ?? ''}\n` : files, stderr: '', exitCode: 0, isStdoutTruncated: false, isStderrTruncated: false } }))
  on('fs.read', (_$, e) => ({ value: orders[e.path.replace('/repo/', '')] ?? '' }))
  on('ui.open', (_$, e) => { opened.push(e.id); return { value: { isPlaced: true } } })

  const r = await $.command.run({ command: 'milestone', args: 'list', presentation: { isFullscreen: true, columns: 200 } } as Parameters<typeof $.command.run>[0])
  const text = r.text ?? ''
  expect(opened).toEqual([])
  expect(text).toContain('Open milestones (2)')
  expect(text.indexOf('**XX**')).toBeLessThan(text.indexOf('**ZZ**'))
  expect(text).toContain('**ZZ**')
  expect(text).toContain('1/2')
  expect(text).toContain('ZZ-S1')
  expect(text).not.toContain('YY')
})
