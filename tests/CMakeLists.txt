find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

add_executable(kds_tests
    status_test.cpp
    crc32c_test.cpp
    page_header_test.cpp
    file_page_device_test.cpp
    memory_page_device_test.cpp
    keystone_test.cpp
    heap_page_test.cpp
    heap_chain_test.cpp
    btree_test.cpp
    superblock_test.cpp
    row_codec_test.cpp
    scheduler_test.cpp
    log_test.cpp
    config_file_test.cpp
    superblock_checkpoint_anchor_test.cpp
    in_memory_page_store_test.cpp
    free_map_test.cpp
    device_page_store_test.cpp
    catalog_cache_test.cpp
    catalog_test.cpp
    insert_wal_test.cpp
    bootstrap_test.cpp
    page_mgr_test.cpp
    command_dispatcher_test.cpp
    tcp_server_test.cpp
    lexer_test.cpp
    parser_test.cpp
    subsystem_logging_test.cpp
    kwp_frame_test.cpp
    wal_record_test.cpp
    wal_payload_test.cpp
    wal_stream_test.cpp
    wal_manager_test.cpp
    page_mgr_wal_gate_test.cpp
    wal_checkpointer_test.cpp
    memory_log_device_test.cpp
    file_log_device_test.cpp
)
target_link_libraries(kds_tests PRIVATE kds GTest::gtest GTest::gtest_main Threads::Threads)

include(GoogleTest)
gtest_discover_tests(kds_tests)
