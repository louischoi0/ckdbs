#!/usr/bin/env python3
"""Layout control C: base 6b0d8412 + one dead, never-called, non-inlined padding function placed
before EncodeOneValue (the function B's change grows). No behaviour change."""
R = "/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/ew/base-ctl/src/exec/row_codec.cpp"


def pad(name, n):
    body = "\n".join(f"    g_{name}[{i % 64}] = {i * 2654435761 % 100000};" for i in range(n))
    return (f"volatile int g_{name}[64];\n[[gnu::noinline, gnu::used]] void {name}() {{\n{body}\n}}\n\n")


anchor = "Status EncodeOneValue(const catalog::SysColumnRow& col, const parser::AstValue& val,"
s = open(R).read()
assert s.count(anchor) == 1
i = s.index(anchor)
open(R, "w").write(s[:i] + pad("KdsLayoutPadEncodeOneValue", 700) + s[i:])
