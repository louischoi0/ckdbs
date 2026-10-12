import { atom, read, update } from 'claude-code'
import type { EngineInterface, Register } from 'claude-code'

import type { Summary } from '../types'
import { parseWorkOrder, report, toMarkdown } from './parse'

const PANE = 'milestone'
// The terminal docks a pane beside the transcript only in fullscreen from this width.
const DOCK_MIN_COLUMNS = 110
const DOCK_HINT = `the side dock needs the fullscreen layout (CLAUDE_CODE_NO_FLICKER=1) and at least ${DOCK_MIN_COLUMNS} columns`
const summary = atom({ plugin: 'milestone-pane', key: 'summary' } as const, null)
const pinned = atom({ plugin: 'milestone-pane', key: 'pinned' } as const, null)

/** The work orders a name picks: the letter exactly, else a substring of the file name. */
async function candidates($: EngineInterface, root: string, name: string): Promise<string[]> {
  const all = (await $.process.run(['git', 'ls-files', 'instructions/*/workorder-*.md'], { cwd: root })).stdout.split('\n').filter(Boolean)
  const n = name.toLowerCase()
  const exact = !/^[a-z]+$/.test(n) ? [] : all.filter(f => new RegExp(`/workorder-${n}(-|\\.md$)`).test(f))
  return exact.length ? exact : all.filter(f => f.toLowerCase().includes(n))
}

/** The pinned name's work order, else the one git touched last. */
async function locate($: EngineInterface, root: string, name: string | null): Promise<string | null> {
  if (name) {
    const found = await candidates($, root, name)
    if (found.length > 1) throw new Error(`"${name}" matches ${found.length} work orders: ${found.map(f => f.replace(/^.*\//, '')).join(', ')}`)
    return found[0] ?? null
  }
  const log = await $.process.run(['git', 'log', '-1', '--format=', '--name-only', '--', 'instructions/*/workorder-*.md'], { cwd: root })
  return log.stdout.split('\n').find(Boolean) ?? null
}

/** The in-progress work order: the one git touched last that still has a stage open, else null. */
async function inProgress($: EngineInterface, root: string): Promise<Summary | null> {
  const tracked = new Set((await $.process.run(['git', 'ls-files', 'instructions/*/workorder-*.md'], { cwd: root })).stdout.split('\n').filter(Boolean))
  const log = await $.process.run(['git', 'log', '--format=', '--name-only', '--', 'instructions/*/workorder-*.md'], { cwd: root })
  for (const file of new Set(log.stdout.split('\n').filter(f => tracked.has(f)))) {
    const s = parseWorkOrder(await $.fs.read(`${root}/${file}`), file)
    if (s.stages.some(x => !x.note && !x.isStruck)) return s
  }
  return null
}

async function refresh($: EngineInterface): Promise<void> {
  const root = await $.session.root()
  const letter = await read($, pinned)
  let next: Summary
  try {
    const file = await locate($, root, letter)
    if (!file) throw new Error(letter ? `no work order for ${letter}` : 'no work order under instructions/')
    next = parseWorkOrder(await $.fs.read(`${root}/${file}`), file)
  } catch (err) {
    next = { letter: letter ?? '?', title: '', file: '', stages: [], rulings: [], items: [], gates: [], error: String(err) }
  }
  await update($, summary, () => next)
}

async function isOpen($: EngineInterface): Promise<boolean> {
  return (await $.ui.panes()).some(p => p.id === PANE)
}

export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    await $.command.register({
      name: 'milestone',
      description: '/milestone <name>: progress and what is left, in the side pane; & the milestone in progress (an error if none); bare / pane: the full summary there; off closes it; auto unpins',
    })
    return next(e)
  })

  on('command.run', { command: 'milestone' }, async ($, e) => {
    const arg = e.args.trim()
    if (/^off$/i.test(arg)) {
      await $.ui.close({ id: PANE })
      return { text: 'Milestone pane closed.' }
    }
    if (arg === '&') {
      const s = await inProgress($, await $.session.root())
      if (!s) return { text: 'Error: no milestone in progress (every tracked work order has its stages landed or struck).', exitCode: 1 }
      await update($, pinned, () => s.letter.toLowerCase())
    }
    const isName = arg !== '' && arg !== '&' && !/^(pane|auto)$/i.test(arg)
    if (/^auto$/i.test(arg)) await update($, pinned, () => null)
    else if (isName) await update($, pinned, () => arg)

    await refresh($)
    const s = await read($, summary)
    if (!s || s.error) {
      if (isName) await update($, pinned, () => null)
      return { text: s?.error ?? 'No work order found.' }
    }
    // Every view is drawn in the pane; the transcript gets one line, the model the full text.
    const text = (await read($, pinned)) ? report(s) : toMarkdown(s)
    const context = [`Milestone progress read from ${s.file} by /milestone:\n${text}`]
    // Never inline above the prompt: where the surface cannot dock, the summary goes to the transcript.
    const { isFullscreen, columns } = e.presentation
    if (!isFullscreen || columns < DOCK_MIN_COLUMNS) {
      await $.ui.close({ id: PANE })
      return { text: `Not docked: ${DOCK_HINT} (now ${isFullscreen ? 'fullscreen' : 'main screen'}, ${columns} columns).\n\n${text}`, context }
    }
    const opened = await $.ui.open({ id: PANE, title: `Milestone ${s.letter}` })
    const where = opened.isPlaced ? '' : ` Pane waits undrawn: ${opened.reason}`
    return {
      text: `Milestone **${s.letter}** shown in the pane (\`${s.file}\`); \`/milestone off\` closes it.${where}`,
      context,
    }
  })

  on('turn.complete', async ($, e, next) => {
    if (e.agentId === undefined && (await isOpen($))) await refresh($)
    return next(e)
  })

  on('ui.render', { component: 'Pane', requestId: PANE }, async ($, e) => {
    const { Box, Markdown } = $.ui.resolve(e)
    // Seated inline (the terminal narrowed after the open): one line, not the summary.
    if (e.props.placement === 'inline') return <Markdown text={`*Milestone pane hidden: ${DOCK_HINT}. \`/milestone off\` closes it.*`} dimColor />
    const s = await read($, summary)
    const body = !s ? <Markdown text="*Loading…*" dimColor />
      : s.error ? <Markdown text={`**Error:** ${s.error}`} />
      : <Markdown text={(await read($, pinned)) ? report(s) : toMarkdown(s)} />
    // The fill covers the whole body box, so the pane reads black past the text too.
    return (
      <Box flexDirection="column" backgroundColor="#000000" width={e.props.bodyColumns} minHeight={e.props.scroll.bodyRows}>
        {body}
      </Box>
    )
  })

  on('ui.render', { component: 'CommandOutput', props: { command: 'milestone' } }, async ($, e, next) => {
    if (e.props.isErrored) return next(e)
    const { Markdown } = $.ui.resolve(e)
    return <Markdown text={e.props.text} />
  })
}
