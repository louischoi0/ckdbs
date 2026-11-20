#!/usr/bin/env python3
"""DL-S0's measurement-only spike: env switches over a clean copy of the tree.
KDS_DL_NO_COLLECTOR   - no dispatcher and no Cabin store is handed the signal collector (M2).
KDS_DL_NO_CABIN_COUNT - CabinStore::NoteHit/NoteMiss/NoteHint return at once (M4).
"""
import sys

root = sys.argv[1]


def patch(path, old, new, count=1):
    p = root + "/" + path
    s = open(p).read()
    assert s.count(old) == count, (path, old, s.count(old))
    s = s.replace(old, new)
    open(p, "w").write(s)


patch("include/kds/server/command_dispatcher.hpp",
      "        set_optimizer_signals(surface.signals);\n",
      "        // DL-S0 spike (measurement only): M2's unset arm.\n"
      "        set_optimizer_signals(std::getenv(\"KDS_DL_NO_COLLECTOR\") != nullptr ? nullptr\n"
      "                                                                              : surface.signals);\n")
p = root + "/include/kds/server/command_dispatcher.hpp"
s = open(p).read()
s = s.replace("#pragma once\n", "#pragma once\n\n#include <cstdlib>\n", 1)
open(p, "w").write(s)

patch("src/server/expeditor.cpp",
      "        expeditor->cabin_store_->set_signals(&*expeditor->optimizer_signals_);\n",
      "        // DL-S0 spike (measurement only): M2's unset arm.\n"
      "        if (std::getenv(\"KDS_DL_NO_COLLECTOR\") == nullptr) {\n"
      "            expeditor->cabin_store_->set_signals(&*expeditor->optimizer_signals_);\n"
      "        }\n")

skip = ("    // DL-S0 spike (measurement only): M4's compiled-out arm.\n"
        "    static const bool skip = std::getenv(\"KDS_DL_NO_CABIN_COUNT\") != nullptr;\n"
        "    if (skip) return;\n")
for fn in ("NoteHit(std::uint64_t cabin_id)", "NoteMiss(std::uint64_t cabin_id)",
           "NoteHint(std::uint64_t cabin_id, bool ok)"):
    head = "void CabinStore::%s {\n" % fn
    patch("src/stats/cabin_store.cpp", head, head + skip)
p = root + "/src/stats/cabin_store.cpp"
s = open(p).read()
s = s.replace("#include \"kds/stats/cabin_store.hpp\"\n",
              "#include \"kds/stats/cabin_store.hpp\"\n\n#include <cstdlib>\n", 1)
assert "#include <cstdlib>" in s
open(p, "w").write(s)
print("patched")
