cmake_minimum_required(VERSION 3.5)
project(Chat CXX)

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
    add_library(chat STATIC
        ${UTILS_SOURCES}
        chat.cpp
        chat_client.cpp
        chat_server.cpp
    )

    add_executable(test test.cpp)
    target_link_libraries(test chat pthread)

    add_executable(client chat_client_exe.cpp)
    target_link_libraries(client chat pthread)

    add_executable(server chat_server_exe.cpp)
    target_link_libraries(server chat pthread)
else()
    file(GLOB TEST_SOURCES *.cpp)
    list(APPEND TEST_SOURCES ${UTILS_SOURCES})
    add_executable(test ${TEST_SOURCES})
endif()
