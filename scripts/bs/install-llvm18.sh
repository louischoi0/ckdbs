#!/bin/bash
# The clang BS builds with (BS-Q2 (a): sanitizers, libFuzzer and llvm-cov
# from one compiler), installed with no sudo: Ubuntu noble's clang 18
# packages fetched with `apt-get download` and unpacked with `dpkg -x` under
# $KDS_OPT (default /home/cdkbs/opt). Idempotent; ends by compiling a
# libFuzzer target and a coverage-instrumented binary.
set -euo pipefail
OPT=${KDS_OPT:-/home/cdkbs/opt}
DEBS=$OPT/llvm18-debs
ROOT=$OPT/llvm18
mkdir -p "$DEBS" "$ROOT"
PACKAGES="clang-18 libclang-cpp18 libclang-common-18-dev libclang-rt-18-dev llvm-18
          llvm-18-linker-tools libllvm18 lld-18 libclang1-18"
for p in $PACKAGES; do
    ls "$DEBS/${p}_"*.deb >/dev/null 2>&1 || (cd "$DEBS" && apt-get download "$p")
done
for d in "$DEBS"/*.deb; do dpkg -x "$d" "$ROOT"; done
B=$ROOT/usr/lib/llvm-18
"$B/bin/clang++" --version | head -1
S=$(mktemp -d)
trap 'rm -rf "$S"' EXIT
printf '#include <cstdint>\n#include <cstddef>\nextern "C" int LLVMFuzzerTestOneInput(const uint8_t*,size_t){return 0;}\n' > "$S/f.cpp"
"$B/bin/clang++" -fsanitize=fuzzer,address -o "$S/f" "$S/f.cpp"
"$S/f" -runs=10 2>&1 | tail -1
printf 'int main(){return 0;}\n' > "$S/m.cpp"
"$B/bin/clang++" -fprofile-instr-generate -fcoverage-mapping -o "$S/m" "$S/m.cpp"
LLVM_PROFILE_FILE="$S/m.profraw" "$S/m"
"$B/bin/llvm-profdata" merge -o "$S/m.profdata" "$S/m.profraw"
"$B/bin/llvm-cov" report "$S/m" -instr-profile="$S/m.profdata" | tail -1
echo "install-llvm18: ok, KDS_LLVM=$B"
