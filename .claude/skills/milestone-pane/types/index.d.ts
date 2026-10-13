export type Entry = { id: string; text: string; note?: string; isStruck?: boolean; isOpen?: boolean; isQuietWrong?: boolean }

export type Summary = {
  letter: string
  title: string
  file: string
  stages: Entry[]
  rulings: Entry[]
  items: Entry[]
  gates: string[]
  /** The close date `index.md` records, when it records one. */
  closed?: string
  error?: string
}

declare module 'claude-code' {
  interface PluginState {
    'milestone-pane': { summary: Summary | null; pinned: string | null }
  }
}
