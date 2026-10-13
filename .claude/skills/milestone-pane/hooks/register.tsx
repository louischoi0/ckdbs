import { atom, read, update } from 'claude-code'
import type { EngineInterface, Register } from 'claude-code'

import type { Summary } from '../types'
import { closedMark, listing, parseWorkOrder, report, toMarkdown } from './parse'

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

/** A work order read with the close its version's `index.md` records. */
async function load($: EngineInterface, root: string, file: string): Promise<Summary> {
  const s = parseWorkOrder(await $.fs.read(`${root}/${file}`), file)
  const index = await $.fs.read(`${root}/${file.replace(/[^/]*$/, 'index.md')}`).catch(() => '')
  const closed = closedMark(index, file)
  return closed ? { ...s, closed, gates: [`Closed ${closed} (index.md)`, ...s.gates] } : s
}

/** Every tracked work order not closed in `index.md` with a stage still open (neither landed nor struck), the one git touched last first. */
async function openOrders($: EngineInterface, root: string, firstOnly = false): Promise<Summary[]> {
  const tracked = new Set((await $.process.run(['git', 'ls-files', 'instructions/*/workorder-*.md'], { cwd: root })).stdout.split('\n').filter(Boolean))
  const log = await $.process.run(['git', 'log', '--format=', '--name-only', '--', 'instructions/*/workorder-*.md'], { cwd: root })
  const open: Summary[] = []
  for (const file of new Set(log.stdout.split('\n').filter(f => tracked.has(f)))) {
    const s = await load($, root, file)
    if (s.closed || !s.stages.some(x => !x.note && !x.isStruck)) continue
    open.push(s)
    if (firstOnly) break
  }
  return open
}

/** When a work order entered the tree: its first commit, followed across renames, in epoch seconds. */
async function addedAt($: EngineInterface, root: string, file: string): Promise<number> {
  const log = await $.process.run(['git', 'log', '--follow', '--diff-filter=A', '--format=%at', '--', file], { cwd: root })
  return Number(log.stdout.split('\n').filter(Boolean).pop() ?? Infinity)
}

/**
 * Reads the pinned order into the pane and returns it. A turn-end refresh writes only if nothing wrote the
 * pane since its dispatch began: `/milestone` runs mid-turn, and a dispatch's reads are one moment, so the
 * pin it read may already be stale.
 */
async function refresh($: EngineInterface, isTurnEnd = false): Promise<Summary> {
  const held = await $.state.get({ plugin: 'milestone-pane', key: 'summary' })
  const root = await $.session.root()
  const letter = await read($, pinned)
  let next: Summary
  try {
    const file = await locate($, root, letter)
    if (!file) throw new Error(letter ? `no work order for ${letter}` : 'no work order under instructions/')
    next = await load($, root, file)
  } catch (err) {
    next = { letter: letter ?? '?', title: '', file: '', stages: [], rulings: [], items: [], gates: [], error: String(err) }
  }
  if (isTurnEnd) await $.state.set({ plugin: 'milestone-pane', key: 'summary' }, next, { ifVersion: held.version })
  else await update($, summary, () => next)
  return next
}

async function isOpen($: EngineInterface): Promise<boolean> {
  return (await $.ui.panes()).some(p => p.id === PANE)
}

export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    await $.command.register({
      name: 'milestone',
      description: '/milestone <name>: progress and what is left, in the side pane; & the milestone in progress (an error if none); list every milestone with a stage open, oldest registered first; bare: toggles the pane (open closes it, closed opens the full summary); pane: the full summary there; off closes it; auto unpins',
      // Typed mid-turn it runs at once rather than waiting for the turn to end; the hook reads git and the pane, never the turn, and `refresh` keeps a turn's late refresh off what it writes.
      immediate: true,
    })
    return next(e)
  })

  on('command.run', { command: 'milestone' }, async ($, e) => {
    const arg = e.args.trim()
    // Bare, it toggles: an open pane closes, a closed one opens below.
    if (/^off$/i.test(arg) || (arg === '' && (await isOpen($)))) {
      await $.ui.close({ id: PANE })
      return { text: 'Milestone pane closed.' }
    }
    if (/^list$/i.test(arg)) {
      const root = await $.session.root()
      const open = await openOrders($, root)
      const added = new Map(await Promise.all(open.map(async s => [s.file, await addedAt($, root, s.file)] as const)))
      open.sort((a, b) => (added.get(a.file) ?? 0) - (added.get(b.file) ?? 0))
      if (!open.length) return { text: 'No milestone open: every tracked work order is closed in index.md or has its stages landed or struck.' }
      const text = listing(open)
      return { text, context: [`Open milestones listed by /milestone list:\n${text}`] }
    }
    if (arg === '&') {
      const [s] = await openOrders($, await $.session.root(), true)
      if (!s) return { text: 'Error: no milestone in progress (every tracked work order is closed in index.md or has its stages landed or struck).', exitCode: 1 }
      await update($, pinned, () => s.letter.toLowerCase())
    }
    const isName = arg !== '' && arg !== '&' && !/^(pane|auto|list)$/i.test(arg)
    if (/^auto$/i.test(arg)) await update($, pinned, () => null)
    else if (isName) await update($, pinned, () => arg)

    const s = await refresh($)
    if (s.error) {
      if (isName) await update($, pinned, () => null)
      return { text: s.error }
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
    if (e.agentId === undefined && (await isOpen($))) await refresh($, true)
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
