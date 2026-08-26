find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

add_executable(kds_tests
    alloc_counter.cpp
    status_test.cpp
    crc32c_test.cpp
    page_header_test.cpp
    anchor_page_test.cpp
    file_page_device_test.cpp
    memory_page_device_test.cpp
    keystone_test.cpp
    keystone_id_test.cpp
    keystone_budget_test.cpp
    heap_page_test.cpp
    wal_writer_test.cpp
    undo_page_test.cpp
    undo_log_test.cpp
    visibility_test.cpp
    trx_id_test.cpp
    step_vm_mvcc_test.cpp
    txn_manager_test.cpp
    txn_session_test.cpp
    heap_chain_test.cpp
    btree_test.cpp
    index_key_test.cpp
    index_tree_test.cpp
    index_ddl_test.cpp
    assertion_ddl_test.cpp
    assertion_catalog_test.cpp
    assertion_violation_test.cpp
    assertion_wal_test.cpp
    assertion_build_test.cpp
    assertion_enforce_test.cpp
    index_maintain_test.cpp
    index_compile_test.cpp
    index_contract_test.cpp
    superblock_test.cpp
    extent_lease_test.cpp
    extent_lease_service_test.cpp
    trx_id_lease_service_test.cpp
    statement_ship_service_test.cpp
    shipped_statement_executor_test.cpp
    core_runtime_test.cpp
    step_pipeline_test.cpp
    step_descriptor_test.cpp
    remote_step_service_test.cpp
    session_step_client_test.cpp
    core_affinity_test.cpp
    coro_test.cpp
    spsc_ring_test.cpp
    ring_transport_test.cpp
    sim_ring_transport_test.cpp
    send_retry_test.cpp
    row_codec_test.cpp
    types_predicate_test.cpp
    types_e2e_test.cpp
    types_contract_test.cpp
    row_layout_test.cpp
    tagged_cell_test.cpp
    analyze_test.cpp
    varheap_test.cpp
    fixed_length_tuple_test.cpp
    catalog_row_test.cpp
    waystone_page_test.cpp
    waystone_dir_test.cpp
    pattern_defs_test.cpp
    create_pattern_test.cpp
    range_scan_test.cpp
    access_stats_test.cpp
    decay_test.cpp
    relayout_planner_test.cpp
    show_relayout_test.cpp
    optimizer_signals_test.cpp
    cabin_optimizer_test.cpp
    cabin_optimizer_replay_test.cpp
    cabin_optimizer_exec_test.cpp
    trail_store_test.cpp
    trail_recorder_test.cpp
    waystone_replay_test.cpp
    waystone_contract_test.cpp
    cabin_contract_test.cpp
    foreign_key_test.cpp
    cabin_store_test.cpp
    scheduler_test.cpp
    log_test.cpp
    config_file_test.cpp
    superblock_checkpoint_anchor_test.cpp
    in_memory_page_store_test.cpp
    free_map_test.cpp
    device_page_store_test.cpp
    eviction_test.cpp
    bound_cabin_test.cpp
    catalog_cache_test.cpp
    catalog_test.cpp
    insert_wal_test.cpp
    bootstrap_test.cpp
    page_mgr_test.cpp
    command_dispatcher_test.cpp
    supplied_key_test.cpp
    tcp_server_test.cpp
    lexer_test.cpp
    fingerprint_test.cpp
    parser_test.cpp
    parser_golden_test.cpp
    parser_join_test.cpp
    parser_projection_test.cpp
    parser_aggregate_test.cpp
    parser_pagination_test.cpp
    pagination_exec_test.cpp
    order_by_exec_test.cpp
    parser_alter_test.cpp
    alter_table_test.cpp
    bulk_insert_test.cpp
    drop_table_test.cpp
    kwp_load_server_test.cpp
    aggregate_contract_test.cpp
    aggregate_test.cpp
    type_literals_test.cpp
    parser_subquery_test.cpp
    parser_fuzz_test.cpp
    step_compile_test.cpp
    inner_build_test.cpp
    inner_build_exec_test.cpp
    inner_build_contract_test.cpp
    step_correlation_test.cpp
    exec_chain_test.cpp
    exec_subquery_test.cpp
    exec_budget_test.cpp
    subsystem_logging_test.cpp
    kwp_frame_test.cpp
    wire_row_codec_test.cpp
    wal_record_test.cpp
    wal_payload_test.cpp
    wal_stream_test.cpp
    wal_log_scanner_test.cpp
    wal_analysis_test.cpp
    wal_redo_test.cpp
    wal_high_water_test.cpp
    wal_recovery_test.cpp
    mount_recovery_test.cpp
    stop_signal_test.cpp
    assertion_recover_test.cpp
    recovery_undo_test.cpp
    wal_manager_test.cpp
    page_mgr_wal_gate_test.cpp
    wal_checkpointer_test.cpp
    memory_log_device_test.cpp
    file_log_device_test.cpp
    sim_rng_test.cpp
    sim_loop_test.cpp
)
target_link_libraries(kds_tests PRIVATE kds kds_sim GTest::gtest GTest::gtest_main Threads::Threads)

# The TLS and SCRAM contract suites exist exactly when their code does
# (both ride KDS_WITH_TLS's OpenSSL; the TLS test also uses the OpenSSL
# API directly to generate its throwaway certificate).
if(KDS_WITH_TLS)
    target_sources(kds_tests PRIVATE tls_channel_test.cpp scram_test.cpp)
endif()

# The V01 golden corpus is data, not source: the test reads it at run time
# so a corpus edit does not force a rebuild. The path is baked in rather
# than resolved relative to the working directory, because ctest and a
# directly-invoked binary do not agree on what that is.
target_compile_definitions(kds_tests
    PRIVATE KDS_PARSER_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/testdata/parser_corpus.txt")

# PHY07's golden action traces, the same data-not-source arrangement for
# the same reasons - and stability of this file across runs and platforms
# is itself the acceptance (fixed-point determinism, PO10).
target_compile_definitions(kds_tests
    PRIVATE KDS_CABIN_TRACES="${CMAKE_CURRENT_SOURCE_DIR}/testdata/cabin_optimizer_traces.txt")

# V13's hygiene check greps the parser sources for productions that exist
# only to accept another dialect's spelling (spec I13). It reads them at
# run time, so it needs the repo root rather than the build tree.
target_compile_definitions(kds_tests PRIVATE KDS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
                                            KDS_BINARY_DIR="${CMAKE_BINARY_DIR}")

include(GoogleTest)
gtest_discover_tests(kds_tests)
