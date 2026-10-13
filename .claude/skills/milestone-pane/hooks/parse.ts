import type { Entry, Summary } from '../types'

const MAX = 180

export function clean(cell: string): string {
  const text = cell
    .replace(/<br\s*\/?>|<\/li>/gi, ' ')
    .replace(/<[^>]+>/g, '')
    .replace(/\*\*|~~/g, '')
    .replace(/\s+/g, ' ')
    .trim()
  return text.length > MAX ? `${text.slice(0, MAX - 1)}…` : text
}

function escape(s: string): string {
  return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

function cells(line: string): string[] {
  return line.split('|').slice(1, -1).map(c => c.trim())
}

function heading(rest: string): string {
  return clean(rest.replace(/^\s*[—-]\s*/, ''))
}

/** Reads one work order's markdown into the pane's summary. */
export function parseWorkOrder(markdown: string, file: string): Summary {
  const lines = markdown.split('\n')
  // The title is the first top-level heading, wherever it sits: a status row may precede it.
  const top = lines.find(l => /^#\s/.test(l)) ?? ''
  const letter = (/workorder-([a-z]+)/.exec(file)?.[1] ?? '?').toUpperCase()
  const L = escape(letter)
  const title = clean(top.replace(/^#\s+(Work order\s+)?/, '').replace(new RegExp(`^${L}\\s*[—-]\\s*`), ''))

  const S = `${L}-S\\d+[a-z]?`
  const stageRow = new RegExp(`^\\|\\s*(${S})\\s*\\|`)
  const itemRow = new RegExp(`^\\|\\s*(${L}-Q\\d+)\\s*\\|`)
  const ruling = new RegExp(`^###\\s+(${L}-R\\d+)\\b(.*)$`)
  // A heading may name several ids before its dash: "### BN-S2, BN-S6, BN-S7 — not built", "### AY-Q9 and AY-S3 — built".
  const status = new RegExp(`^###\\s+((?:${L}-[SQR]\\d+[a-z]?(?:\\s*,\\s*|\\s+and\\s+)?)+)\\s*[—-]\\s*(.*)$`)
  const leading = new RegExp(`^###\\s+(${S})(?![\\w(])(.*)$`)
  // A stage row whose text opens with one of these words is a status row (a later row, or the only one).
  const settled = /^(built|landed|done|filed|written|measured|run|closed|struck|folded|merged|retired|discarded)\b/i

  const stages = new Map<string, Entry>()
  const rulings: Entry[] = []
  const items: Entry[] = []

  for (const line of lines) {
    let m: RegExpExecArray | null
    if ((m = stageRow.exec(line))) {
      const c = cells(line)
      const id = m[1] ?? ''
      const text = clean(c[1] ?? '')
      const stage = stages.get(id) ?? { id, text, isStruck: (c[1] ?? '').includes('~~') }
      if (!stages.has(id)) stages.set(id, stage)
      if (!stage.note && settled.test(text)) stage.note = text
    } else if ((m = itemRow.exec(line))) {
      const c = cells(line)
      const mark = clean(c[4] ?? '')
      items.push({
        id: (m[1] ?? ''),
        text: clean(c[1] ?? ''),
        note: mark || (c[3] ? `proposal: ${clean(c[3])}` : undefined),
        isStruck: (c[1] ?? '').includes('~~') || /^Struck/i.test(mark),
        isOpen: mark === '',
        isQuietWrong: /quiet-wrong/.test(line),
      })
    } else if ((m = ruling.exec(line))) {
      const text = heading((m[2] ?? ''))
      rulings.push({ id: (m[1] ?? ''), text, isStruck: /^Struck/i.test(text), isQuietWrong: /quiet-wrong/.test((m[2] ?? '')) })
    } else if ((m = status.exec(line) ?? leading.exec(line))) {
      for (const id of (m[1] ?? '').split(/\s*,\s*|\s+and\s+/).map(x => x.trim())) {
        const stage = stages.get(id)
        if (stage && !stage.note) stage.note = heading(m[2] ?? '')
      }
    }
  }

  const stageList = [...stages.values()]
  const gates: string[] = []
  const next = stageList.find(s => !s.note && !s.isStruck)
  gates.push(next ? `Next stage: ${next.id} — ${next.text}` : 'Every stage has a row-status entry')
  const open = items.filter(i => i.isOpen && !i.isStruck)
  gates.push(`Operator items unmarked: ${open.length} of ${items.filter(i => !i.isStruck).length}${open.length ? ` (${open.map(i => i.id).join(', ')})` : ''}`)
  for (const i of open.filter(i => i.isQuietWrong)) gates.push(`[quiet-wrong] unmarked: ${i.id}`)
  for (const s of stageList) {
    const cond = /\b(only if|not built if|unless)\b[^.;]*/i.exec(s.text)
    if (cond && !s.note) gates.push(`${s.id} conditional: ${cond[0]}`)
  }
  gates.push('Per stage: critics-developer review, full suite; overhead A/B at the milestone close; push on the word')

  return { letter, title, file, stages: stageList, rulings, items, gates }
}

/** The command's report, as markdown: progress, then what is left. */
export function report(s: Summary): string {
  const live = s.stages.filter(x => !x.isStruck)
  const landed = live.filter(x => x.note)
  const left = live.filter(x => !x.note)
  const open = s.items.filter(i => i.isOpen && !i.isStruck)
  const last = landed[landed.length - 1]
  const conditional = s.gates.filter(g => / conditional: /.test(g))
  return [
    `## ${s.letter} — ${s.title}`,
    '',
    `\`${s.file}\``,
    '',
    `**Progress:** ${landed.length}/${live.length} stages landed · ${open.length} operator item(s) unmarked`,
    ...(last ? ['', `**Last landed:** **${last.id}** — *${last.note}*`] : []),
    '',
    '### Remaining stages',
    '',
    ...(left.length ? left.map(x => `- **${x.id}** — ${x.text}`) : ['*none*']),
    '',
    '### Unmarked operator items',
    '',
    ...(open.length ? open.map(i => `- **${i.id}**${i.isQuietWrong ? ' `quiet-wrong`' : ''} — ${i.text}`) : ['*none*']),
    ...(conditional.length ? ['', '### Conditional stages', '', ...conditional.map(g => `- ${g.replace(/^(\S+) conditional:/, '**$1** —')}`)] : []),
  ].join('\n')
}

/** The pane's body, as markdown the transcript's own renderer draws. */
export function toMarkdown(s: Summary): string {
  const strike = (x: Entry, text: string) => (x.isStruck ? `~~${text}~~` : text)
  const line = (x: Entry, tags: string[]) =>
    `- **${x.id}**${tags.map(t => ` \`${t}\``).join('')} — ${strike(x, x.text)}${x.note ? `\n  *${x.note}*` : ''}`
  const section = (name: string, list: Entry[], tags: (x: Entry) => string[]) =>
    [`## ${name} (${list.length})`, '', ...(list.length ? list.map(x => line(x, tags(x))) : ['*none*']), '']
  return [
    `# ${s.letter} — ${s.title}`,
    '',
    `\`${s.file}\``,
    '',
    '## Gates',
    '',
    ...s.gates.map(g => `- ${g.replace(/^([^:]+):/, '**$1:**')}`),
    '',
    '---',
    '',
    ...section('Stages', s.stages, x => (x.isStruck ? ['struck'] : [x.note ? 'landed' : 'open'])),
    ...section('Rulings', s.rulings, x => [...(x.isStruck ? ['struck'] : []), ...(x.isQuietWrong ? ['quiet-wrong'] : [])]),
    ...section('Operator items', s.items, x =>
      x.isStruck ? ['struck'] : [x.isOpen ? 'unmarked' : 'marked', ...(x.isQuietWrong ? ['quiet-wrong'] : [])]),
  ].join('\n')
}

/** The close the version's `index.md` records on a work order's row ("CLOSED 2026-09-08", "Closed 2026-10-10"), else undefined. */
export function closedMark(index: string, file: string): string | undefined {
  const name = file.replace(/^.*\//, '')
  const row = index.split('\n').find(l => l.startsWith(`| \`${name}\` |`))
  return row && /\bclosed\s+(\d{4}-\d{2}-\d{2})/i.exec(row)?.[1]
}

/** `/milestone list`: one row per open work order, in the order given - progress and the next stage. */
export function listing(open: Summary[]): string {
  const cell = (t: string) => t.replace(/\|/g, '\\|')
  return [
    `## Open milestones (${open.length})`,
    '',
    '| milestone | landed | next stage | file |',
    '|---|---|---|---|',
    ...open.map(s => {
      const live = s.stages.filter(x => !x.isStruck)
      const next = live.find(x => !x.note)
      return `| **${s.letter}** — ${cell(s.title)} | ${live.length - live.filter(x => !x.note).length}/${live.length} | ${next ? `**${next.id}** — ${cell(next.text)}` : ''} | \`${s.file.replace(/^.*\//, '')}\` |`
    }),
  ].join('\n')
}
