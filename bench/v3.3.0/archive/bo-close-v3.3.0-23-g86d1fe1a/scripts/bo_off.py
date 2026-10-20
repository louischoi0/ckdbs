"""Measurement-only patch for BO's close (BO-Q14 (b)): arm A is the closing
commit with BO's statement-path recording compiled out. Run from the root of
an exported source tree: python3 bo_off.py

Removes: `RecordStatement`'s body (two monotonic clock reads, one wall read
and `StatementStats::Record`'s latched map update), and every
`RelationStats::Record` (one latched map update per write statement). Left
in: `Dispatch`'s and `DispatchAsync`'s one `NowNs()` at the head, whose value
then goes unread.
"""
import sys

p = 'src/server/command_dispatcher.cpp'
s = open(p).read()
head = '''void CommandDispatcher::RecordStatement(std::string_view line, const DispatchOutcome& out,
                                        sched::MonoTimeNs began_ns) const {
'''
assert s.count(head) == 1, 'RecordStatement not found'
s = s.replace(head, head + '''    if (out.fetch_id != ~std::uint64_t{0}) return;  // BO-Q14 (b): arm A
''')
open(p, 'w').write(s)

p = 'src/stats/relation_stats.cpp'
s = open(p).read()
old = '''                           std::uint64_t rows) noexcept {
    if (rows == 0) return;
'''
assert s.count(old) == 1, 'RelationStats::Record not found'
s = s.replace(old, '''                           std::uint64_t rows) noexcept {
    if (rows != ~std::uint64_t{0}) return;  // BO-Q14 (b): arm A
''')
open(p, 'w').write(s)
print('arm A patch applied')
