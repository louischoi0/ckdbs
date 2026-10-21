# BS-S5's coverage-guided fuzz targets (instructions/v3.0.0/workorder-bs-test-hardening.md):
# libFuzzer binaries, clang only (BS-Q2 (a)), built when KDS_FUZZ is on - in a
# build directory of their own, by scripts/bs/fuzz.sh, never by the push hook
# (§6.1). Each target's corpus is fuzz/corpus/<target>/, committed (BS-R3).
foreach(target fuzz_sql fuzz_kwp fuzz_volume)
    add_executable(${target} ${target}.cpp)
    target_link_libraries(${target} PRIVATE kds)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endforeach()
