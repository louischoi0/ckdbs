find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

add_executable(kds_tests
    keystone_test.cpp
    heap_page_test.cpp
    superblock_test.cpp
    in_memory_page_store_test.cpp
    catalog_test.cpp
    bootstrap_test.cpp
    page_mgr_test.cpp
    command_dispatcher_test.cpp
    tcp_server_test.cpp
    lexer_test.cpp
    parser_test.cpp
)
target_link_libraries(kds_tests PRIVATE kds GTest::gtest GTest::gtest_main Threads::Threads)

include(GoogleTest)
gtest_discover_tests(kds_tests)
