import { test, expect } from 'claude-code/testing'
import type { TestBody } from 'claude-code/testing'

const file = 'instructions/v3.0.0/workorder-zz-fixture.md'
const doc = [
  '# Work order ZZ — a fixture order',
  '| stage | what | done when | size |',
  '|---|---|---|---|',
  '| ZZ-S0 | **The order** | x | S |',
  '| ZZ-S1 | **The premise, measured** | x | M |',
  '### ZZ-S0 — written 2026-10-12',
].join('\n')

type T = Parameters<TestBody>[0]
const run = ($: T, isFullscreen: boolean, columns: number) =>
  $.command.run({ command: 'milestone', args: 'ZZ', presentation: { isFullscreen, columns } } as Parameters<typeof $.command.run>[0])
const fixture = (on: Parameters<TestBody>[1], opened: string[]) => {
  on('session.root', () => ({ value: '/repo' }))
  on('process.run', () => ({ value: { stdout: `${file}\n`, stderr: '', exitCode: 0, isStdoutTruncated: false, isStderrTruncated: false } }))
  on('fs.read', () => ({ value: doc }))
  on('ui.open', (_$, e) => { opened.push(e.id); return { value: { isPlaced: true } } })
  on('ui.close', () => ({ value: undefined }))
}

const PANE = { title: 'Milestone ZZ', isFocused: false, bodyColumns: 80, placement: 'dock' as const, scroll: { offset: 0, bodyRows: 40 }, view: {} }

test('/milestone <name> draws in the pane and leaves one line in the transcript', async ($, on) => {
  const opened: string[] = []
  fixture(on, opened)

  const r = await run($, true, 120)
  const text = r.text ?? ''
  expect(opened).toEqual(['milestone'])
  expect(text.split('\n')).toHaveLength(1)
  expect(text).toContain('shown in the pane')
  expect(r.context?.[0]).toContain('ZZ-S1')

  for (const surface of ['terminal', 'desktop'] as const) {
    const ui = await $.ui.mount({ plugin: 'milestone-pane', surface, component: 'Pane', requestId: 'milestone', props: PANE })
    const md = await ui.find({ type: 'Markdown' })
    expect(md?.text).toContain('ZZ-S1')
    await ui.unmount()
  }
})

test('/milestone never opens inline: off the dock the summary goes to the transcript', async ($, on) => {
  const opened: string[] = []
  fixture(on, opened)
  for (const [isFullscreen, columns] of [[false, 200], [true, 100]] as const) {
    const r = await run($, isFullscreen, columns)
    expect(opened).toEqual([])
    expect(r.text).toContain('Not docked')
    expect(r.text).toContain('ZZ-S1')
  }
})

test('a pane seated inline draws one line, not the summary', async $ => {
  const ui = await $.ui.mount({ plugin: 'milestone-pane', surface: 'terminal', component: 'Pane', requestId: 'milestone', props: { ...PANE, placement: 'inline' as const } })
  const md = await ui.find({ type: 'Markdown' })
  expect(md?.text).toContain('hidden')
  await ui.unmount()
})

const done = 'instructions/v3.0.0/workorder-yy-done.md'
const doneDoc = ['# Work order YY — all landed', '| stage | what | done when | size |', '|---|---|---|---|', '| YY-S0 | **The order** | x | S |', '### YY-S0 — written 2026-10-01'].join('\n')

test('/milestone & shows the last-touched order with a stage open, skipping finished ones', async ($, on) => {
  const opened: string[] = []
  on('session.root', () => ({ value: '/repo' }))
  on('process.run', () => ({ value: { stdout: `${done}\n${file}\n`, stderr: '', exitCode: 0, isStdoutTruncated: false, isStderrTruncated: false } }))
  on('fs.read', (_$, e) => ({ value: e.path.endsWith(done) ? doneDoc : doc }))
  on('ui.open', (_$, e) => { opened.push(e.id); return { value: { isPlaced: true } } })
  const r = await $.command.run({ command: 'milestone', args: '&', presentation: { isFullscreen: true, columns: 120 } } as Parameters<typeof $.command.run>[0])
  expect(opened).toEqual(['milestone'])
  expect(r.text).toContain('Milestone **ZZ**')
  expect(r.context?.[0]).toContain('Remaining stages')
})

test('/milestone & with nothing in progress is an error', async ($, on) => {
  on('session.root', () => ({ value: '/repo' }))
  on('process.run', () => ({ value: { stdout: `${done}\n`, stderr: '', exitCode: 0, isStdoutTruncated: false, isStderrTruncated: false } }))
  on('fs.read', () => ({ value: doneDoc }))
  const r = await $.command.run({ command: 'milestone', args: '&', presentation: { isFullscreen: true, columns: 120 } } as Parameters<typeof $.command.run>[0])
  expect(r.text).toContain('Error: no milestone in progress')
  expect(r.exitCode).toBe(1)
})

test('a bare /milestone toggles the pane: closed it opens, open it closes', async ($, on) => {
  const opened: string[] = []
  const closed: string[] = []
  let isPaneOpen = false
  on('session.root', () => ({ value: '/repo' }))
  on('process.run', () => ({ value: { stdout: `${file}\n`, stderr: '', exitCode: 0, isStdoutTruncated: false, isStderrTruncated: false } }))
  on('fs.read', () => ({ value: doc }))
  on('ui.open', (_$, e) => { opened.push(e.id); return { value: { isPlaced: true } } })
  on('ui.close', (_$, e) => { closed.push(e.id); isPaneOpen = false; return { value: undefined } })
  on('ui.panes', () => ({ value: isPaneOpen ? [{ id: 'milestone' }] : [] } as never))
  const bare = () => $.command.run({ command: 'milestone', args: '', presentation: { isFullscreen: true, columns: 120 } } as Parameters<typeof $.command.run>[0])

  const first = await bare()
  expect(opened).toEqual(['milestone'])
  expect(first.text).toContain('shown in the pane')
  isPaneOpen = true

  const second = await bare()
  expect(closed).toEqual(['milestone'])
  expect(opened).toEqual(['milestone'])
  expect(second.text).toBe('Milestone pane closed.')
})

test('an unpinned /milestone finds the last order a non-merge commit touched', async ($, on) => {
  on('session.root', () => ({ value: '/repo' }))
  // A merge commit lists no files: answer empty unless the query skips merges.
  on('process.run', (_$, e) => ({ value: { stdout: e.argv.includes('--no-merges') ? `${file}\n` : '', stderr: '', exitCode: 0, isStdoutTruncated: false, isStderrTruncated: false } }))
  on('fs.read', () => ({ value: doc }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.panes', () => ({ value: [] } as never))
  const r = await $.command.run({ command: 'milestone', args: '', presentation: { isFullscreen: true, columns: 120 } } as Parameters<typeof $.command.run>[0])
  expect(r.text).toContain('Milestone **ZZ**')
})

test('/milestone is registered to run at once when typed mid-turn', async ($, on) => {
  const registered: { name: string, immediate?: true }[] = []
  on('command.register', (_$, e) => { registered.push(e); return { value: { command: e.name } } })
  on('session.start', (_$, e) => ({ cwd: e.cwd }))
  await $.session.start({ cwd: '/repo', surface: 'terminal', isInteractive: true })
  expect(registered.find(c => c.name === 'milestone')?.immediate).toBe(true)
})

test('a turn-end refresh outrun by a mid-turn re-pin leaves the newer order in the pane', async ($, on) => {
  const other = 'instructions/v3.0.0/workorder-xx-other.md'
  const otherDoc = ['# Work order XX — another order', '| stage | what | done when | size |', '|---|---|---|---|', '| XX-S0 | **The other** | x | S |'].join('\n')
  let hold: (() => void) | null = null
  let reached: () => void = () => {}
  const isReached = new Promise<void>(r => { reached = r })
  on('session.root', () => ({ value: '/repo' }))
  on('process.run', async () => {
    if (hold === null && armed) { const gate = new Promise<void>(r => { hold = r }); reached(); await gate }
    return { value: { stdout: `${file}\n${other}\n`, stderr: '', exitCode: 0, isStdoutTruncated: false, isStderrTruncated: false } }
  })
  on('fs.read', (_$, e) => ({ value: e.path.endsWith(other) ? otherDoc : doc }))
  on('ui.open', () => ({ value: { isPlaced: true } }))
  on('ui.panes', () => ({ value: [{ id: 'milestone' }] } as never))
  on('turn.complete', () => ({ text: '' }))
  let armed = false
  const run = (args: string) => $.command.run({ command: 'milestone', args, presentation: { isFullscreen: true, columns: 120 } } as Parameters<typeof $.command.run>[0])

  await run('ZZ')
  armed = true
  const turn = $.turn.complete({ answer: '', durationMs: 0, isAborted: false, turnId: 't', reason: 'answer' })
  await isReached
  armed = false
  const r = await run('XX')
  expect(r.text).toContain('Milestone **XX**')
  ;(hold as unknown as () => void)()
  await turn

  const ui = await $.ui.mount({ plugin: 'milestone-pane', surface: 'terminal', component: 'Pane', requestId: 'milestone', props: PANE })
  const md = await ui.find({ type: 'Markdown' })
  expect(md?.text).toContain('XX-S0')
  await ui.unmount()
})
