#!/usr/bin/env python3
"""DI-S0 arm B2, over B1: the binding cut to the statement's first relation (and the
namespace word). A second relation is dropped, not an overflow.
  b2.py SRC"""
import sys

src = sys.argv[1]


def edit(rel, old, new):
    path = src + "/" + rel
    s = open(path).read()
    assert s.count(old) == 1, (rel, old, s.count(old))
    open(path, "w").write(s.replace(old, new))


edit("include/kds/stats/statement_binding.hpp",
     "    static constexpr std::size_t kMaxRelations = 16;",
     "    static constexpr std::size_t kMaxRelations = 1;")
edit("include/kds/stats/statement_binding.hpp",
     """        if (n == kMaxRelations) {
            *this = StatementBinding{};
            over = true;
            return;
        }""",
     "        if (n == kMaxRelations) return;")
edit("src/server/command_dispatcher.cpp",
     "    static_assert(stats::StatementBinding::kMaxRelations == exec::kMaxPlannedRelations);\n",
     "")
