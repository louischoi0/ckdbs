# Two cores growing one heap chain at once can overwrite a link and orphan a page

**Found** 2026-09-25 by the `critics-developer` pass on AT-S10b (`59ed9c0`
on `at-s10-ring-consumers`), as its C2, and confirmed by reading
`ChainInsert` (`src/storage/heap/heap_chain.cpp`) on the same branch at
`879b15b`. **Not fixed.** No cell reproduces it.

**Cost: a quiet wrong answer** - rows that were inserted and committed stop
being reachable by any walk.

## The wrong behaviour

`ChainInsert` finds the tail by walking (`ChainTail`) with no latch held,
then takes that page exclusive. When the tail is full it creates a page and
links it with `set_next_page_id(new_id)`, without asking whether the tail
already links somewhere. Two cores that both walked to tail T while it was
the last page:

1. Core A takes T, finds it full, creates page P1, fills it, links
   `T.next = P1`, releases T.
2. Core B, which reached T by the same walk and waited for its latch, takes
   T, finds it full, creates P2 and links `T.next = P2`.

P1 and every row placed in it are orphaned. The walk-then-latch gap is
AT-S5c's descent gap in the heap: the btree re-validates coverage after its
re-fetch, the chain does not re-validate the link.

## Reach

Heap relations only, and SUS-1 refuses a new one: this is every heap
relation created before 2026-09-05, written from two cores at once, which
AT-S5 made possible (a write runs where its session is). Before AT-S5 one
core wrote a relation and the reactor serialised the two growths.

## The fix, known and unscheduled

After taking the tail exclusive, re-read `next_page_id`: a tail that
already links somewhere is not the tail, and the insert walks on from the
link. Owner: `docs/spec/heap-and-tuple.md` §4.1a's chain growth.
