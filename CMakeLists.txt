cmake_minimum_required(VERSION 3.20)
project(rowdb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
option(ROWDB_ASAN "Build with AddressSanitizer and UBSan" OFF)
if(ROWDB_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(rowdb_core src/disk.cpp src/wal.cpp src/buffer_pool.cpp src/engine.cpp)
target_include_directories(rowdb_core PUBLIC src)

add_executable(rowdb src/main.cpp)
target_link_libraries(rowdb rowdb_core)

add_executable(unit_test tests/unit_test.cpp)
target_link_libraries(unit_test rowdb_core)
target_compile_definitions(unit_test PRIVATE ROWDB_TEST_TMP="${CMAKE_BINARY_DIR}/test-tmp")
