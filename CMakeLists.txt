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
    src/storage/crc32c.cpp
    src/storage/page_header.cpp
    src/storage/page_device.cpp
    src/storage/file_page_device.cpp
    src/storage/memory_page_device.cpp
    src/storage/keystone.cpp
    src/storage/heap/heap_page.cpp
    src/storage/free_map.cpp
    src/storage/in_memory_page_store.cpp
    src/storage/device_page_store.cpp
    src/storage/page_mgr/page_mgr.cpp
    src/server/superblock.cpp
    src/catalog/oid.cpp
    src/catalog/rows.cpp
    src/catalog/schema.cpp
    src/catalog/sys_object_registry.cpp
    src/catalog/catalog.cpp
    src/bootstrap/bootstrap.cpp
    src/server/command_dispatcher.cpp
    src/server/expeditor.cpp
    src/server/tcp_server.cpp
    src/parser/lexer.cpp
    src/parser/ast.cpp
    src/parser/parser.cpp
    src/wire/frame_codec.cpp
    src/wal/record.cpp
    src/wal/payload.cpp
    src/wal/stream.cpp
    src/wal/log_device.cpp
    src/wal/memory_log_device.cpp
    src/wal/file_log_device.cpp
    src/exec/row_codec.cpp
)
target_include_directories(kds PUBLIC include)

add_executable(kds_server src/server/main.cpp)
target_link_libraries(kds_server PRIVATE kds)

option(KDS_BUILD_TESTS "Build ckdbs unit tests" ON)
if(KDS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
