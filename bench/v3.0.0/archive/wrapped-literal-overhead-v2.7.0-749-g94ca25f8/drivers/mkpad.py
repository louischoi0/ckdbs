#!/usr/bin/env python3
"""Layout control: base 6b0d8412 + two dead, never-called, non-inlined padding functions at the
points B's change adds code (before CoerceLiteralToColumn, before PkBound). No behaviour change."""
import re
R = "/tmp/claude-1000/-home-cdkbs-ckdbs/305b0005-d7d2-422c-b0ed-247e8d0b8e88/scratchpad/lw/base-ctl/src/exec/"


def pad(name, n):
    body = "\n".join(f"    g_{name}[{i % 64}] = {i * 2654435761 % 100000};" for i in range(n))
    return (f"volatile int g_{name}[64];\n"
            f"[[gnu::noinline, gnu::used]] void {name}() {{\n{body}\n}}\n\n")


def ins(path, anchor, text):
    s = open(R + path).read()
    i = s.index(anchor)
    open(R + path, "w").write(s[:i] + text + s[i:])


ins("row_codec.cpp", "Status CoerceLiteralToColumn(const catalog::SysColumnRow& col, parser::AstValue& val) {",
    pad("KdsLayoutPadRowCodec", 700))
ins("step_compiler.cpp", "std::optional<std::uint64_t> PkBound(const Operand& operand) {",
    pad("KdsLayoutPadStepCompiler", 300))
