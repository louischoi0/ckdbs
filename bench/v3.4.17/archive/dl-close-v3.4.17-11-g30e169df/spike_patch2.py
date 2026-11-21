#!/usr/bin/env python3
"""DL-S0 spike, second switch: KDS_DL_NO_PLAN_COUNT skips CapturedPlans::CountServed (DL-Q4's price)."""
import sys

p = sys.argv[1] + "/src/exec/step_compiler.cpp"
s = open(p).read()
old = "        captured->CountServed();\n        const std::vector<std::size_t> perm(in_force.begin()"
assert s.count(old) == 1
s = s.replace(old,
              "        // DL-S0 spike (measurement only): DL-Q4's price of a word every core writes.\n"
              "        static const bool skip_count = std::getenv(\"KDS_DL_NO_PLAN_COUNT\") != nullptr;\n"
              "        if (!skip_count) captured->CountServed();\n"
              "        const std::vector<std::size_t> perm(in_force.begin()")
s = s.replace("#include <algorithm>\n", "#include <algorithm>\n#include <cstdlib>\n", 1)
open(p, "w").write(s)
print("patched")
