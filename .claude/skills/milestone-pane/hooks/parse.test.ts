import { expect, test } from 'claude-code/testing'

import { parseWorkOrder, report, toMarkdown } from './parse'

const doc = [
  '# Work order ZZ — a fixture order',
  '| stage | what | done when | size |',
  '|---|---|---|---|',
  '| ZZ-S0 | **The order** | <ul><li>This file</li></ul> | S |',
  '| ZZ-S1 | **The premise, measured** | x | M |',
  '| ZZ-S2 | **The cache**, only if ZZ-Q2 opens it | x | M |',
  '### ZZ-R1 — Never wrong `[quiet-wrong]`',
  '### ZZ-R2 — Struck (W4): gone',
  '| item | question | kind | CLA\'s proposal | mark |',
  '|---|---|---|---|---|',
  '| ZZ-Q0 | **Open ZZ** | process | Yes | Marked (W1) |',
  '| ZZ-Q1 | ~~Old~~ | - | - | **Struck by W4** |',
  '| ZZ-Q2 | **The memo**, [quiet-wrong] under (b) | design | (a) | |',
  '### ZZ-S0 — written 2026-10-12',
].join('\n')

test('reads stages, rulings, items and gates', () => {
  const s = parseWorkOrder(doc, 'instructions/v3.0.0/workorder-zz-fixture.md')
  expect(s.letter).toBe('ZZ')
  expect(s.title).toBe('a fixture order')
  expect(s.stages.map(x => [x.id, x.note ?? null])).toEqual([['ZZ-S0', 'written 2026-10-12'], ['ZZ-S1', null], ['ZZ-S2', null]])
  expect(s.rulings.map(r => [r.id, !!r.isStruck, !!r.isQuietWrong])).toEqual([['ZZ-R1', false, true], ['ZZ-R2', true, false]])
  expect(s.items.map(i => [i.id, !!i.isOpen, !!i.isStruck])).toEqual([['ZZ-Q0', false, false], ['ZZ-Q1', false, true], ['ZZ-Q2', true, false]])
  expect(s.gates[0]).toBe('Next stage: ZZ-S1 — The premise, measured')
  expect(s.gates[1]).toBe('Operator items unmarked: 1 of 2 (ZZ-Q2)')
  expect(s.gates).toContain('[quiet-wrong] unmarked: ZZ-Q2')
  expect(s.gates).toContain('ZZ-S2 conditional: only if ZZ-Q2 opens it')
})

test('reports progress and what is left', () => {
  const text = report(parseWorkOrder(doc, 'instructions/v3.0.0/workorder-zz-fixture.md'))
  expect(text).toContain('**Progress:** 1/3 stages landed · 1 operator item(s) unmarked')
  expect(text).toContain('- **ZZ-S1** — The premise, measured')
  expect(text).toContain('- **ZZ-Q2** `quiet-wrong` — The memo')
  expect(text).toContain('- **ZZ-S2** — only if ZZ-Q2 opens it')
  expect(text).not.toContain('- **ZZ-S0**')
})

test('draws the summary as markdown', () => {
  const md = toMarkdown(parseWorkOrder(doc, 'instructions/v3.0.0/workorder-zz-fixture.md'))
  expect(md).toContain('# ZZ — a fixture order')
  expect(md).toContain('- **Next stage:** ZZ-S1 — The premise, measured')
  expect(md).toContain('- **ZZ-S0** `landed` — The order\n  *written 2026-10-12*')
  expect(md).toContain('- **ZZ-R2** `struck` — ~~Struck (W4): gone~~')
  expect(md).toContain('- **ZZ-Q2** `unmarked` `quiet-wrong` — The memo')
})
