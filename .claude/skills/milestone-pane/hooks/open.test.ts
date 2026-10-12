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
