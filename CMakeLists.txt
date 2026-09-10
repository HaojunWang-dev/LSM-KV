cmake_minimum_required(VERSION 3.16)

project(lsm_kv LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lsm_kv
    src/arena.cc
    src/coding.cc
    src/memtable.cc
    src/status.cc
    src/write_batch.cc
)

target_include_directories(lsm_kv
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

include(CTest)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(lsm_kv_tests
        tests/unit/arena_test.cc
        tests/unit/coding_test.cc
        tests/unit/internal_key_test.cc
        tests/unit/lookup_key_test.cc
        tests/unit/slice_test.cc
        tests/unit/skiplist_test.cc
        tests/unit/memtable_test.cc
        tests/unit/status_test.cc
        tests/unit/write_batch_test.cc
    )

    target_link_libraries(lsm_kv_tests
        PRIVATE
            lsm_kv
            GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(lsm_kv_tests)
endif()
