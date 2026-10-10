cmake_minimum_required(VERSION 3.20)

project(UHMAS_HL LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(UHMAS_BUILD_PROFILE "dev" CACHE STRING "Build profile: dev or release")
set_property(CACHE UHMAS_BUILD_PROFILE PROPERTY STRINGS dev release)
if(NOT UHMAS_BUILD_PROFILE MATCHES "^(dev|release)$")
    message(FATAL_ERROR "UHMAS_BUILD_PROFILE must be dev or release")
endif()

set(_uhmas_default_build_tests ON)
if(UHMAS_BUILD_PROFILE STREQUAL "release")
    set(_uhmas_default_build_tests OFF)
endif()

option(UHMAS_BUILD_TESTS "Build UHMAS-HL tests" ${_uhmas_default_build_tests})
option(UHMAS_BUILD_BENCHMARKS "Build UHMAS-HL benchmarks" OFF)

add_library(uhmas
    src/uhm.c
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(uhmas
        PRIVATE
            UHMAS_BUILD_SHARED
            UHMAS_BUILDING_LIBRARY
        INTERFACE
            UHMAS_BUILD_SHARED
    )
endif()

target_include_directories(uhmas
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

function(uhmas_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /experimental:c11atomics)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -fvisibility=hidden)
    endif()
endfunction()

uhmas_target_warnings(uhmas)

if(UHMAS_BUILD_TESTS)
    enable_testing()

    add_executable(uhmas_hl_tests
        tests/test_uhm_hl.c
    )
    target_link_libraries(uhmas_hl_tests PRIVATE uhmas)
    uhmas_target_warnings(uhmas_hl_tests)

    add_test(NAME uhmas_hl_tests COMMAND uhmas_hl_tests)
endif()

if(UHMAS_BUILD_BENCHMARKS)
    find_package(Threads REQUIRED)

    add_executable(uhmas_hl_benchmarks
        tests/bench_uhm_hl.c
    )
    target_link_libraries(uhmas_hl_benchmarks PRIVATE uhmas Threads::Threads)
    uhmas_target_warnings(uhmas_hl_benchmarks)
endif()
