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
    src/storage/free_map.cpp
    src/storage/extent_lease.cpp
    src/txn/undo_page.cpp
    src/txn/undo_log.cpp
    src/txn/visibility.cpp
    src/txn/trx_id.cpp
    src/txn/manager.cpp
    src/storage/in_memory_page_store.cpp
    src/storage/device_page_store.cpp
    src/storage/page_mgr/page_mgr.cpp
    src/server/superblock.cpp
    src/server/superblock_checkpoint_anchor.cpp
    src/server/remote_checkpoint_anchor.cpp
    src/server/core_runtime.cpp
    src/server/core_affinity.cpp
    src/server/extent_lease_service.cpp
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
    src/catalog/catalog_cache.cpp
    src/catalog/catalog.cpp
    src/bootstrap/bootstrap.cpp
    src/server/command_dispatcher.cpp
    src/server/expeditor.cpp
    src/server/config_file.cpp
    src/server/tcp_server.cpp
    src/parser/lexer.cpp
    src/parser/fingerprint.cpp
    src/parser/ast.cpp
    src/parser/parser.cpp
    src/wire/frame_codec.cpp
    src/wire/row_codec.cpp
    src/wal/record.cpp
    src/wal/payload.cpp
    src/wal/stream.cpp
    src/wal/manager.cpp
    src/wal/checkpointer.cpp
    src/wal/log_device.cpp
    src/wal/memory_log_device.cpp
    src/wal/file_log_device.cpp
    src/wal/writer.cpp
    src/exec/row_codec.cpp
    src/exec/fk_check.cpp
    src/exec/plan_printer.cpp
    src/exec/step_compiler.cpp
    src/exec/aggregate.cpp
    src/exec/chain_frame.cpp
    src/exec/step_vm.cpp
    src/exec/budget.cpp
    src/exec/catalog_view.cpp
    src/exec/trail_replay.cpp
    src/exec/tuple_verify.cpp
    src/exec/pattern_ddl.cpp
    src/exec/cabin_ddl.cpp
    src/sched/clock.cpp
    src/sched/scheduler.cpp
    src/sched/coro.cpp
    src/sched/spsc_ring.cpp
    src/sched/ring_transport.cpp
    src/sched/sim_ring_transport.cpp
    src/sched/epoll_io_backend.cpp
)
target_include_directories(kds PUBLIC include)

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
endif()

option(KDS_BUILD_TESTS "Build ckdbs unit tests" ON)
if(KDS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
