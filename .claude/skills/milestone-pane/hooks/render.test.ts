import { test, expect } from 'claude-code/testing'

test('the command row draws as markdown, not dim', async $ => {
  for (const surface of ['terminal', 'desktop'] as const) {
    const ui = await $.ui.mount({ plugin: 'milestone-pane', surface, component: 'CommandOutput',
      props: { command: 'milestone', args: 'BL', text: '## BL\n\n- **BL-S1** — x', isErrored: false } })
    const md = await ui.find({ type: 'Markdown' })
    expect(md).toBeDefined()
    await ui.unmount()
  }
})
