cmake_minimum_required(VERSION 3.5)
project(ThreadPool CXX)

set(CMAKE_CXX_STANDARD 17)

set(COMMON_FLAGS
    -Wextra
    -Werror
    -Wall
    -Wno-gnu-folding-constant
    -g
)
add_compile_options(${COMMON_FLAGS})

option(ENABLE_LEAK_CHECKS
    "Enable memory leak checks with heap_help"
    OFF)

option(ENABLE_GLOB_SEARCH
    "Enable compilation of all the files, not just the preselected ones"
    OFF)

set(UTILS_DIR ${CMAKE_SOURCE_DIR}/../utils)
set(UTILS_SOURCES ${UTILS_DIR}/unit.cpp)

include_directories(${UTILS_DIR})

if(ENABLE_LEAK_CHECKS)
    list(APPEND UTILS_SOURCES ${UTILS_DIR}/heap_help/heap_help.cpp)
    include_directories(${UTILS_DIR}/heap_help)
endif()

if(NOT ENABLE_GLOB_SEARCH)
    set(TEST_SOURCES
        thread_pool.cpp
        test.cpp
        ${UTILS_SOURCES}
    )
    add_executable(test ${TEST_SOURCES})
else()
    file(GLOB TEST_SOURCES *.cpp)
    list(APPEND TEST_SOURCES ${UTILS_SOURCES})
    add_executable(test ${TEST_SOURCES})
endif()

target_link_libraries(test pthread)
