#!/usr/bin/env python3
"""DL-S0 spike, third switch: KDS_DL_NO_POOL_COUNT skips the pool's hit count (DL-Q4's price)."""
import sys

p = sys.argv[1] + "/src/storage/device_page_store.cpp"
s = open(p).read()
old = "        hits_.Add();\n        if (mark_dirty) it->second.MarkDirty(++dirty_gens_);"
assert s.count(old) == 1
s = s.replace(old,
              "        // DL-S0 spike (measurement only): DL-Q4's price of a word every core writes.\n"
              "        static const bool skip_hit = std::getenv(\"KDS_DL_NO_POOL_COUNT\") != nullptr;\n"
              "        if (!skip_hit) hits_.Add();\n"
              "        if (mark_dirty) it->second.MarkDirty(++dirty_gens_);")
s = s.replace("#include <algorithm>\n", "#include <algorithm>\n#include <cstdlib>\n", 1)
open(p, "w").write(s)
print("patched")
