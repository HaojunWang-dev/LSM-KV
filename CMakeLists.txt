cmake_minimum_required(VERSION 3.16)

project(lsm_kv LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(lsm_kv
    src/arena.cc
    src/coding.cc
    src/comparator.cc
    src/format.cc
    src/iterator.cc
    src/crc32c.cc
    src/memtable.cc
    src/status.cc
    src/write_batch.cc
    src/env.cc
    src/env_posix.cc
    src/log_writer.cc
    src/log_reader.cc
    src/VersionEdit.cc
    src/VersionSet.cc
    src/filename.cc
    src/logging.cc
    src/table_format.cc
    src/table_block.cc
    src/table.cc
    src/two_level_iterator.cc
)

target_include_directories(lsm_kv
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# -DBUILD_TESTING=OFF builds only lsm_kv, without requiring GoogleTest.
option(BUILD_TESTING "Build LSM-KV unit tests (requires GoogleTest)" ON)
include(CTest)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(lsm_kv_tests
        tests/unit/arena_test.cc
        tests/unit/coding_test.cc
        tests/unit/comparator_test.cc
        tests/unit/crc32c_test.cc
        tests/unit/env_test.cc
        tests/unit/env_posix_test.cc
        tests/unit/random_access_file_test.cc
        tests/unit/filename_test.cc
        tests/unit/internal_key_test.cc
        tests/unit/iterator_test.cc
        tests/unit/lookup_key_test.cc
        tests/unit/log_reader_test.cc
        tests/unit/log_writer_test.cc
        tests/unit/logging_test.cc
        tests/unit/slice_test.cc
        tests/unit/skiplist_test.cc
        tests/unit/memtable_test.cc
        tests/unit/status_test.cc
        tests/unit/table_format_test.cc
        tests/unit/table_block_test.cc
        tests/unit/table_test.cc
        tests/unit/write_batch_test.cc
        tests/unit/version_edit_test.cc
        tests/unit/version_set_test.cc
    )

    target_link_libraries(lsm_kv_tests
        PRIVATE
            lsm_kv
            GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(lsm_kv_tests TEST_LIST lsm_kv_discovered_tests)
    # Only bound the new table tests; malformed input or a broken Seek must not
    # leave CTest waiting forever. This file runs after test discovery.
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/table_test_properties.cmake")
endif()
