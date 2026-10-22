cmake_minimum_required(VERSION 3.20)
project(ckdbs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

# BS-S1's sanitizer configurations (instructions/v3.0.0/workorder-bs-test-hardening.md):
# `address` is ASan + UBSan, `thread` is TSan, each in a build directory of
# its own and built with clang (BS-Q2 (a)) by scripts/bs/. Empty, the
# default, is the build the push hook runs, which never sets it (§6.1).
set(KDS_SANITIZE "" CACHE STRING "Sanitizers: address (ASan + UBSan), thread (TSan), or empty")
if(KDS_SANITIZE STREQUAL "address")
    # A UBSan report fails its cell rather than scrolling past it.
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(KDS_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
elseif(NOT KDS_SANITIZE STREQUAL "")
    message(FATAL_ERROR "KDS_SANITIZE='${KDS_SANITIZE}': expected address, thread or empty")
endif()

# BS-S2's coverage configuration: clang's source-based coverage, read by
# llvm-cov (BS-Q2 (a)); scripts/bs/coverage.sh builds and reports it.
option(KDS_COVERAGE "Instrument for llvm-cov source-based coverage (clang only)" OFF)
if(KDS_COVERAGE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate)
endif()

# BS-S5's fuzz targets (fuzz/): libFuzzer, clang only, the engine built with
# its coverage hooks so the fuzzer sees inside it; scripts/bs/fuzz.sh builds
# and runs them.
option(KDS_FUZZ "Build the libFuzzer targets under fuzz/ (clang only)" OFF)
if(KDS_FUZZ)
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()

add_library(kds
    src/base/log.cpp
    src/base/fail_stop.cpp
    src/base/crash_point.cpp
    src/storage/crc32c.cpp
    src/storage/page_header.cpp
    src/storage/anchor_page.cpp
    src/storage/page_device.cpp
    src/storage/page_write_fence.cpp
    src/storage/file_page_device.cpp
    src/storage/memory_page_device.cpp
    src/storage/keystone.cpp
    src/storage/tagged_cell.cpp
    src/storage/varheap.cpp
    src/storage/heap/heap_page.cpp
    src/storage/heap/heap_chain.cpp
    src/storage/btree/btree_page.cpp
    src/storage/btree/btree.cpp
    src/storage/index/index_page.cpp
    src/storage/index/index_tree.cpp
    src/storage/free_map.cpp
    src/txn/undo_page.cpp
    src/txn/varheap_release.cpp
    src/txn/undo_log.cpp
    src/txn/recovery_undo.cpp
    src/txn/visibility.cpp
    src/txn/trx_id.cpp
    src/txn/instance_visibility.cpp
    src/txn/lock_table.cpp
    src/txn/manager.cpp
    src/storage/in_memory_page_store.cpp
    src/storage/cabin_bound_page.cpp
    src/storage/data_syncer.cpp
    src/storage/device_page_store.cpp
    src/storage/page_mgr/page_mgr.cpp
    src/server/superblock.cpp
    src/server/superblock_checkpoint_anchor.cpp
    src/server/page_reclaim.cpp
    src/server/restore.cpp
    src/server/backup.cpp
    src/server/backup_manifest.cpp
    src/server/wal_archive.cpp
    src/server/core_runtime.cpp
    src/server/mount_recovery.cpp
    src/server/stop_signal.cpp
    src/exec/assertion_recover.cpp
    src/catalog/oid.cpp
    src/catalog/rows.cpp
    src/catalog/keystone_budget.cpp
    src/catalog/schema.cpp
    src/catalog/foreign_key.cpp
    src/catalog/row_layout.cpp
    src/catalog/sys_object_registry.cpp
    src/stats/waystone_page.cpp
    src/stats/waystone_dir.cpp
    src/stats/trail_store.cpp
    src/stats/plan_stats.cpp
    src/stats/checkpoint_log.cpp
    src/stats/lock_wait_log.cpp
    src/stats/relation_stats.cpp
    src/stats/statement_stats.cpp
    src/stats/trail_recorder.cpp
    src/stats/access_stats.cpp
    src/stats/cabin_store.cpp
    src/stats/optimizer_signals.cpp
    src/stats/cabin_optimizer.cpp
    src/catalog/catalog_cache.cpp
    src/catalog/catalog.cpp
    src/bootstrap/bootstrap.cpp
    src/server/command_dispatcher.cpp
    src/server/sys_views.cpp
    src/server/expeditor.cpp
    src/server/config_file.cpp
    src/server/tcp_server.cpp
    src/server/connection_handoff.cpp
    src/server/load_placer.cpp
    src/server/priority.cpp
    src/server/kwp_load_server.cpp
    src/server/kwp_session.cpp
    src/parser/lexer.cpp
    src/parser/fingerprint.cpp
    src/parser/ast.cpp
    src/parser/parser.cpp
    src/wire/frame_codec.cpp
    src/wire/kwp_types.cpp
    src/wire/handshake.cpp
    src/wire/error_registry.cpp
    src/wire/row_codec.cpp
    src/wal/record.cpp
    src/wal/payload.cpp
    src/wal/stream.cpp
    src/wal/log_scanner.cpp
    src/wal/analysis.cpp
    src/wal/redo.cpp
    src/wal/high_water.cpp
    src/wal/recovery.cpp
    src/wal/manager.cpp
    src/wal/checkpointer.cpp
    src/wal/log_device.cpp
    src/wal/memory_log_device.cpp
    src/wal/file_log_device.cpp
    src/wal/writer.cpp
    src/exec/row_codec.cpp
    src/exec/catalog_spills.cpp
    src/exec/purge_key.cpp
    src/exec/clean_dropped.cpp
    src/exec/varheap_sweep.cpp
    src/exec/index_key.cpp
    src/exec/fk_check.cpp
    src/exec/plan_printer.cpp
    src/exec/functions.cpp
    src/exec/step_compiler.cpp
    src/exec/join_order.cpp
    src/exec/plan_memo.cpp
    src/exec/set_expr.cpp
    src/exec/query_layer.cpp
    src/exec/aggregate.cpp
    src/exec/sort.cpp
    src/exec/type_literals.cpp
    src/exec/chain_frame.cpp
    src/exec/step_vm.cpp
    src/exec/budget.cpp
    src/exec/trail_replay.cpp
    src/exec/tuple_verify.cpp
    src/exec/wal_row_log.cpp
    src/exec/cabin_ddl.cpp
    src/exec/cabin_expr.cpp
    src/exec/index_ddl.cpp
    src/exec/assertion_build.cpp
    src/exec/assertion_catalog.cpp
    src/exec/declaration_relation.cpp
    src/exec/transition_catalog.cpp
    src/exec/transition.cpp
    src/exec/assertion_check.cpp
    src/exec/assertion_replay.cpp
    src/exec/assertion_violation.cpp
    src/exec/bound_cabin.cpp
    src/exec/cabin_optimizer_exec.cpp
    src/exec/index_maintain.cpp
    src/sched/clock.cpp
    src/sched/scheduler.cpp
    src/sched/coro.cpp
    src/sched/waker.cpp
    src/sched/sim_waker_table.cpp
    src/sched/epoll_io_backend.cpp
)
target_include_directories(kds PUBLIC include)

# `kds/base/version.hpp`: the build's `git describe --tags`, which a backup
# manifest records (BR-R7). Regenerated at every build, rewritten only when it
# changes.
set(KDS_VERSION_HEADER ${CMAKE_BINARY_DIR}/generated/kds/base/version.hpp)
add_custom_target(kds_version ALL
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${KDS_VERSION_HEADER}
            -P ${CMAKE_SOURCE_DIR}/cmake/version.cmake
    BYPRODUCTS ${KDS_VERSION_HEADER})
add_dependencies(kds kds_version)
target_include_directories(kds PUBLIC ${CMAKE_BINARY_DIR}/generated)

# The TLS wire channel (docs/spec/protocol.md §1, direct TLS decided
# 2026-08-13). OpenSSL is the engine's first external dependency, so it
# stays behind an option: OFF builds every subsystem except
# src/server/tls_channel.cpp, and a config with `tls = on` is then
# refused at startup naming this flag. ON (the default) requires the
# OpenSSL headers and fails the *configure* loudly rather than the first
# TLS connection quietly.
option(KDS_WITH_TLS "Build the TLS wire channel (requires OpenSSL >= 3)" ON)
if(KDS_WITH_TLS)
    find_package(OpenSSL 3.0 REQUIRED)
    # scram.cpp and auth.cpp ride the same dependency: SCRAM's PBKDF2 and
    # HMAC come from the OpenSSL this option already requires.
    target_sources(kds PRIVATE src/server/tls_channel.cpp src/server/scram.cpp
                               src/server/auth.cpp)
    target_link_libraries(kds PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(kds PUBLIC KDS_WITH_TLS=1)
else()
    message(STATUS
        "KDS_WITH_TLS=OFF: the TLS channel and its tests are excluded; `tls = on` will be refused at startup")
endif()

add_executable(kds_server src/server/main.cpp)
target_link_libraries(kds_server PRIVATE kds)

# The v2 microbenchmarks (kds_bench, kds_keystone_alloc_bench, kds_txn_bench,
# kds_crosscore_bench, kds_session_step_state_bench) left the tree with
# bench/ on 2026-09-02, ahead of the big-bang change; `git show
# 1769487:bench/<file>` retrieves a source and bench/README.md says why.
# KDS_BUILD_BENCH returned with the first v3 microbenchmark (AN-S5): off by
# default, so the test builds pay nothing for it.
option(KDS_BUILD_BENCH "Build the ckdbs microbenchmarks" OFF)
if(KDS_BUILD_BENCH)
    add_executable(kds_read_view_bench bench/read_view_bench.cpp)
    target_link_libraries(kds_read_view_bench PRIVATE kds)
    add_executable(kds_pool_sweep_bench bench/pool_sweep_bench.cpp)
    target_link_libraries(kds_pool_sweep_bench PRIVATE kds)
endif()

# The simulation harness (bench/workplan-teststrategy SIM01-SIM14): the
# engine driven in-process through CommandDispatcher over crashable
# in-memory devices, every random choice derived from one --seed. A
# library so the unit tests can drive the same loop the standalone binary
# runs; the binary itself must not link gtest (the workplan's
# [OPEN: harness home] note - a framework's fixture lifecycle fights
# crash-restart iteration).
option(KDS_BUILD_SIM "Build the ckdbs simulation harness" ON)
if(KDS_BUILD_SIM)
    add_library(kds_sim
        sim/instance.cpp
        sim/integrity.cpp
        sim/faults.cpp
        sim/minimize.cpp
        sim/workload.cpp
        sim/oracle.cpp
        sim/loop.cpp
    )
    target_include_directories(kds_sim PUBLIC ${CMAKE_SOURCE_DIR})
    target_link_libraries(kds_sim PUBLIC kds)

    add_executable(ckdbs-sim sim/sim_main.cpp)
    target_link_libraries(ckdbs-sim PRIVATE kds_sim)
endif()

option(KDS_BUILD_TESTS "Build ckdbs unit tests" ON)
if(KDS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(KDS_FUZZ)
    add_subdirectory(fuzz)
endif()
