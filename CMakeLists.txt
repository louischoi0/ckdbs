cmake_minimum_required(VERSION 3.20)
project(ckdbs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

add_library(kds
    src/base/log.cpp
    src/storage/crc32c.cpp
    src/storage/page_header.cpp
    src/storage/anchor_page.cpp
    src/storage/page_device.cpp
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
    src/storage/extent_lease.cpp
    src/txn/undo_page.cpp
    src/txn/undo_log.cpp
    src/txn/recovery_undo.cpp
    src/txn/visibility.cpp
    src/txn/trx_id.cpp
    src/txn/manager.cpp
    src/storage/in_memory_page_store.cpp
    src/storage/cabin_bound_page.cpp
    src/storage/device_page_store.cpp
    src/storage/page_mgr/page_mgr.cpp
    src/server/superblock.cpp
    src/server/superblock_checkpoint_anchor.cpp
    src/server/remote_checkpoint_anchor.cpp
    src/server/core_runtime.cpp
    src/server/mount_recovery.cpp
    src/server/stop_signal.cpp
    src/exec/assertion_recover.cpp
    src/server/core_affinity.cpp
    src/server/extent_lease_service.cpp
    src/server/row_id_lease_service.cpp
    src/server/relation_grant_service.cpp
    src/server/index_build_service.cpp
    src/server/assertion_build_service.cpp
    src/server/statement_ship_service.cpp
    src/server/shipped_statement_executor.cpp
    src/server/trx_id_lease_service.cpp
    src/server/step_descriptor.cpp
    src/server/remote_step_service.cpp
    src/server/session_step_client.cpp
    src/catalog/oid.cpp
    src/catalog/rows.cpp
    src/catalog/keystone_budget.cpp
    src/catalog/schema.cpp
    src/catalog/foreign_key.cpp
    src/catalog/row_layout.cpp
    src/catalog/sys_object_registry.cpp
    src/stats/waystone_page.cpp
    src/stats/waystone_dir.cpp
    src/stats/pattern_defs.cpp
    src/stats/trail_store.cpp
    src/stats/trail_recorder.cpp
    src/stats/access_stats.cpp
    src/stats/cabin_store.cpp
    src/stats/relayout_planner.cpp
    src/stats/optimizer_signals.cpp
    src/stats/cabin_optimizer.cpp
    src/catalog/catalog_cache.cpp
    src/catalog/catalog.cpp
    src/bootstrap/bootstrap.cpp
    src/server/command_dispatcher.cpp
    src/server/expeditor.cpp
    src/server/config_file.cpp
    src/server/tcp_server.cpp
    src/server/kwp_load_server.cpp
    src/parser/lexer.cpp
    src/parser/fingerprint.cpp
    src/parser/ast.cpp
    src/parser/parser.cpp
    src/wire/frame_codec.cpp
    src/wire/kwp_types.cpp
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
    src/exec/index_key.cpp
    src/exec/fk_check.cpp
    src/exec/plan_printer.cpp
    src/exec/step_compiler.cpp
    src/exec/aggregate.cpp
    src/exec/sort.cpp
    src/exec/type_literals.cpp
    src/exec/chain_frame.cpp
    src/exec/step_vm.cpp
    src/exec/budget.cpp
    src/exec/catalog_view.cpp
    src/exec/trail_replay.cpp
    src/exec/tuple_verify.cpp
    src/exec/pattern_ddl.cpp
    src/exec/wal_row_log.cpp
    src/exec/cabin_ddl.cpp
    src/exec/index_ddl.cpp
    src/exec/assertion_build.cpp
    src/exec/assertion_catalog.cpp
    src/exec/assertion_check.cpp
    src/exec/assertion_replay.cpp
    src/exec/assertion_violation.cpp
    src/exec/bound_cabin.cpp
    src/exec/cabin_optimizer_exec.cpp
    src/exec/index_maintain.cpp
    src/sched/clock.cpp
    src/sched/scheduler.cpp
    src/sched/coro.cpp
    src/sched/spsc_ring.cpp
    src/sched/ring_transport.cpp
    src/sched/waker.cpp
    src/sched/sim_ring_transport.cpp
    src/sched/epoll_io_backend.cpp
)
target_include_directories(kds PUBLIC include)

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

# The benchmark measures libkds, not its own translation unit, so
# optimizing only this target would buy nothing - the numbers are only
# meaningful when the whole tree is configured Release:
#
#     cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
#     cmake --build build-release -j && ./build-release/kds_bench
#
# The binary says so at startup when it detects otherwise, rather than
# printing Debug numbers someone might quote.
option(KDS_BUILD_BENCH "Build ckdbs microbenchmarks" ON)
if(KDS_BUILD_BENCH)
    add_executable(kds_bench bench/bench_main.cpp)
    target_link_libraries(kds_bench PRIVATE kds)

    # Separate binary rather than a section of kds_bench: this one asks a
    # question about one subsystem (keystoneid-invariant.md K0) and carries
    # a prototype allocator that is not a candidate for merging. Keeping it
    # apart is what keeps that distinction visible.
    add_executable(kds_keystone_alloc_bench bench/keystone_alloc_bench.cpp)
    target_link_libraries(kds_keystone_alloc_bench PRIVATE kds)

    # A third binary, asking where a *transaction's* time goes rather than
    # what one component costs: it drives the four statements
    # tools/stress_business.py measures TPS for, and prices each layer.
    add_executable(kds_txn_bench bench/txn_layers_bench.cpp)
    target_link_libraries(kds_txn_bench PRIVATE kds)

    # A fourth, and the only benchmark in this tree that cannot be a server
    # driver: the cross-core pipeline runs only against a peer-owned
    # relation, and a peer-owned relation cannot be populated over the wire
    # (cross-core writes are refused, DML shipping is unbuilt, core 0 alone
    # listens). It builds its rows in-process and runs one dataset through
    # two dispatchers differing only in core_id - see the file header.
    add_executable(kds_crosscore_bench bench/crosscore_pipeline_bench.cpp)
    target_link_libraries(kds_crosscore_bench PRIVATE kds)
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
