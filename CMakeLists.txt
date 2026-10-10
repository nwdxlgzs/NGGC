cmake_minimum_required(VERSION 3.20)

project(NextGenerationGarbageCollector LANGUAGES C)

include(CheckIncludeFile)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES Profile)
    list(REMOVE_DUPLICATES CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING
        "Available build configurations" FORCE)
    set(CMAKE_C_FLAGS_PROFILE "${CMAKE_C_FLAGS_RELWITHDEBINFO}" CACHE STRING
        "C flags for Profile builds" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS_PROFILE
        "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}" CACHE STRING
        "Executable linker flags for Profile builds" FORCE)
    set(CMAKE_STATIC_LINKER_FLAGS_PROFILE
        "${CMAKE_STATIC_LINKER_FLAGS_RELWITHDEBINFO}" CACHE STRING
        "Static library linker flags for Profile builds" FORCE)
else()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel Profile)
    set(CMAKE_C_FLAGS_PROFILE "${CMAKE_C_FLAGS_RELWITHDEBINFO}" CACHE STRING
        "C flags for Profile builds" FORCE)
    set(CMAKE_EXE_LINKER_FLAGS_PROFILE
        "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}" CACHE STRING
        "Executable linker flags for Profile builds" FORCE)
    set(CMAKE_STATIC_LINKER_FLAGS_PROFILE
        "${CMAKE_STATIC_LINKER_FLAGS_RELWITHDEBINFO}" CACHE STRING
        "Static library linker flags for Profile builds" FORCE)
endif()

option(NGGC_BUILD_TESTS "Build NGGC tests" ON)
option(NGGC_ENABLE_VERIFY "Enable NGGC verifier code in all configs" OFF)
option(NGGC_ENABLE_ASAN_HOOKS "Enable NGGC ASAN hook code" OFF)
option(NGGC_ENABLE_TSAN_HOOKS "Enable NGGC TSAN hook code" OFF)
option(NGGC_ENABLE_FAULT_INJECTION "Enable NGGC fault injection code" OFF)
option(NGGC_ENABLE_PROFILING "Enable NGGC profiling code in all configs" OFF)
option(NGGC_ENABLE_TRACE_LOG "Enable NGGC trace logging code" OFF)

check_include_file(threads.h NGGC_HAVE_THREADS_H)
if(NGGC_HAVE_THREADS_H)
    find_package(Threads REQUIRED)
endif()

set(UHMAS_BUILD_TESTS OFF CACHE BOOL "Build UHMAS-HL tests" FORCE)
set(UHMAS_BUILD_BENCHMARKS OFF CACHE BOOL "Build UHMAS-HL benchmarks" FORCE)
add_subdirectory(mock-uhmas/UHMAS-HL)

add_library(nggc
    src/nggc_backend.c
    src/nggc.c
)

target_include_directories(nggc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(nggc
    PUBLIC
        NGGC_ENABLE_VERIFY=$<IF:$<OR:$<CONFIG:Debug>,$<BOOL:${NGGC_ENABLE_VERIFY}>>,1,0>
        NGGC_ENABLE_ASAN_HOOKS=$<IF:$<BOOL:${NGGC_ENABLE_ASAN_HOOKS}>,1,0>
        NGGC_ENABLE_TSAN_HOOKS=$<IF:$<BOOL:${NGGC_ENABLE_TSAN_HOOKS}>,1,0>
        NGGC_ENABLE_FAULT_INJECTION=$<IF:$<BOOL:${NGGC_ENABLE_FAULT_INJECTION}>,1,0>
        NGGC_ENABLE_PROFILING=$<IF:$<OR:$<CONFIG:Profile>,$<BOOL:${NGGC_ENABLE_PROFILING}>>,1,0>
        NGGC_ENABLE_TRACE_LOG=$<IF:$<BOOL:${NGGC_ENABLE_TRACE_LOG}>,1,0>
        NGGC_HAVE_C11_THREADS=$<IF:$<BOOL:${NGGC_HAVE_THREADS_H}>,1,0>
)

target_link_libraries(nggc
    PUBLIC
        uhmas
)

if(NGGC_HAVE_THREADS_H)
    target_link_libraries(nggc PUBLIC Threads::Threads)
endif()

function(nggc_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /experimental:c11atomics)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

nggc_target_warnings(nggc)

if(NGGC_BUILD_TESTS)
    enable_testing()

    add_executable(nggc_basic_tests
        tests/test_nggc_basic.c
    )
    target_link_libraries(nggc_basic_tests PRIVATE nggc)
    nggc_target_warnings(nggc_basic_tests)

    add_test(NAME nggc_basic_tests COMMAND nggc_basic_tests)

    add_executable(nggc_pin_tests
        tests/test_nggc_pin.c
    )
    target_link_libraries(nggc_pin_tests PRIVATE nggc)
    nggc_target_warnings(nggc_pin_tests)

    add_test(NAME nggc_pin_tests COMMAND nggc_pin_tests)

    add_executable(nggc_mutator_assist_tests
        tests/test_nggc_mutator_assist.c
    )
    target_link_libraries(nggc_mutator_assist_tests PRIVATE nggc)
    nggc_target_warnings(nggc_mutator_assist_tests)

    add_test(NAME nggc_mutator_assist_tests
             COMMAND nggc_mutator_assist_tests)

    add_executable(nggc_trigger_tests
        tests/test_nggc_trigger.c
    )
    target_link_libraries(nggc_trigger_tests PRIVATE nggc)
    nggc_target_warnings(nggc_trigger_tests)

    add_test(NAME nggc_trigger_tests COMMAND nggc_trigger_tests)

    add_executable(nggc_region_tests
        tests/test_nggc_regions.c
    )
    target_link_libraries(nggc_region_tests PRIVATE nggc)
    nggc_target_warnings(nggc_region_tests)

    add_test(NAME nggc_region_tests COMMAND nggc_region_tests)

    add_executable(nggc_young_tests
        tests/test_nggc_young.c
    )
    target_link_libraries(nggc_young_tests PRIVATE nggc)
    nggc_target_warnings(nggc_young_tests)

    add_test(NAME nggc_young_tests COMMAND nggc_young_tests)

    add_executable(nggc_backend_failure_tests
        tests/test_nggc_backend_failures.c
    )
    target_link_libraries(nggc_backend_failure_tests PRIVATE nggc)
    nggc_target_warnings(nggc_backend_failure_tests)

    add_test(NAME nggc_backend_failure_tests
             COMMAND nggc_backend_failure_tests)

    add_executable(nggc_internal_tests
        tests/test_nggc_internal.c
    )
    target_include_directories(nggc_internal_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(nggc_internal_tests PRIVATE nggc)
    nggc_target_warnings(nggc_internal_tests)

    add_test(NAME nggc_internal_tests COMMAND nggc_internal_tests)

    add_executable(nggc_scanner_replay_tests
        tests/test_nggc_scanner_replay.c
    )
    target_link_libraries(nggc_scanner_replay_tests PRIVATE nggc)
    nggc_target_warnings(nggc_scanner_replay_tests)

    add_test(NAME nggc_scanner_replay_tests
             COMMAND nggc_scanner_replay_tests)

    add_executable(nggc_scanner_ex_tests
        tests/test_nggc_scanner_ex.c
    )
    target_link_libraries(nggc_scanner_ex_tests PRIVATE nggc)
    nggc_target_warnings(nggc_scanner_ex_tests)

    add_test(NAME nggc_scanner_ex_tests
             COMMAND nggc_scanner_ex_tests)

    add_executable(nggc_scanner_guard_tests
        src/nggc_backend.c
        src/nggc.c
        tests/test_nggc_scanner_guard.c
    )
    target_include_directories(nggc_scanner_guard_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(nggc_scanner_guard_tests
        PRIVATE NGGC_ENABLE_VERIFY=1 NGGC_ENABLE_TEST_HOOKS=1
    )
    target_link_libraries(nggc_scanner_guard_tests PRIVATE uhmas)
    if(NGGC_HAVE_THREADS_H)
        target_compile_definitions(nggc_scanner_guard_tests
            PRIVATE NGGC_HAVE_C11_THREADS=1
        )
        target_link_libraries(nggc_scanner_guard_tests PRIVATE Threads::Threads)
    endif()
    nggc_target_warnings(nggc_scanner_guard_tests)
    add_test(NAME nggc_scanner_guard_tests COMMAND nggc_scanner_guard_tests)
    set_tests_properties(nggc_scanner_guard_tests PROPERTIES TIMEOUT 30)

    add_executable(nggc_error_result_tests
        src/nggc_backend.c
        src/nggc.c
        tests/test_nggc_error_results.c
    )
    target_include_directories(nggc_error_result_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(nggc_error_result_tests PRIVATE
        NGGC_ENABLE_VERIFY=1 NGGC_ENABLE_TEST_HOOKS=1
    )
    target_link_libraries(nggc_error_result_tests PRIVATE uhmas)
    if(NGGC_HAVE_THREADS_H)
        target_compile_definitions(nggc_error_result_tests PRIVATE
            NGGC_HAVE_C11_THREADS=1
        )
        target_link_libraries(nggc_error_result_tests PRIVATE Threads::Threads)
    endif()
    nggc_target_warnings(nggc_error_result_tests)
    add_test(NAME nggc_error_result_tests COMMAND nggc_error_result_tests)
    set_tests_properties(nggc_error_result_tests PROPERTIES TIMEOUT 30)

    add_executable(nggc_registry_verifier_tests
        src/nggc_backend.c
        src/nggc.c
        tests/test_nggc_registry_verifier.c
    )
    target_include_directories(nggc_registry_verifier_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(nggc_registry_verifier_tests
        PRIVATE
            NGGC_ENABLE_VERIFY=1
            NGGC_ENABLE_TEST_HOOKS=1
    )
    target_link_libraries(nggc_registry_verifier_tests PRIVATE uhmas)
    nggc_target_warnings(nggc_registry_verifier_tests)

    add_test(NAME nggc_registry_verifier_tests
             COMMAND nggc_registry_verifier_tests)

    add_executable(nggc_reference_verifier_tests
        src/nggc_backend.c
        src/nggc.c
        tests/test_nggc_reference_verifier.c
    )
    target_include_directories(nggc_reference_verifier_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(nggc_reference_verifier_tests
        PRIVATE
            NGGC_ENABLE_VERIFY=1
            NGGC_ENABLE_TEST_HOOKS=1
    )
    target_link_libraries(nggc_reference_verifier_tests PRIVATE uhmas)
    nggc_target_warnings(nggc_reference_verifier_tests)

    add_test(NAME nggc_reference_verifier_tests
             COMMAND nggc_reference_verifier_tests)

    add_executable(nggc_discovered_tests
        src/nggc_backend.c
        src/nggc.c
        tests/test_nggc_discovered.c
    )
    target_include_directories(nggc_discovered_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(nggc_discovered_tests
        PRIVATE
            NGGC_ENABLE_VERIFY=1
            NGGC_ENABLE_TEST_HOOKS=1
    )
    target_link_libraries(nggc_discovered_tests PRIVATE uhmas)
    if(NGGC_HAVE_THREADS_H)
        target_compile_definitions(nggc_discovered_tests
            PRIVATE
                NGGC_HAVE_C11_THREADS=1
        )
        target_link_libraries(nggc_discovered_tests PRIVATE Threads::Threads)
    endif()
    nggc_target_warnings(nggc_discovered_tests)

    add_test(NAME nggc_discovered_tests COMMAND nggc_discovered_tests)

    add_executable(nggc_reference_packet_tests
        src/nggc_backend.c
        src/nggc.c
        tests/test_nggc_reference_packets.c
    )
    target_include_directories(nggc_reference_packet_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(nggc_reference_packet_tests
        PRIVATE
            NGGC_ENABLE_VERIFY=1
            NGGC_ENABLE_TEST_HOOKS=1
    )
    target_link_libraries(nggc_reference_packet_tests PRIVATE uhmas)
    nggc_target_warnings(nggc_reference_packet_tests)

    add_test(NAME nggc_reference_packet_tests
             COMMAND nggc_reference_packet_tests)

    add_executable(nggc_phase_verifier_tests
        src/nggc_backend.c
        src/nggc.c
        tests/test_nggc_phase_verifier.c
    )
    target_include_directories(nggc_phase_verifier_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(nggc_phase_verifier_tests
        PRIVATE
            NGGC_ENABLE_VERIFY=1
            NGGC_ENABLE_TEST_HOOKS=1
    )
    target_link_libraries(nggc_phase_verifier_tests PRIVATE uhmas)
    if(NGGC_HAVE_THREADS_H)
        target_compile_definitions(nggc_phase_verifier_tests
            PRIVATE
                NGGC_HAVE_C11_THREADS=1
        )
        target_link_libraries(nggc_phase_verifier_tests PRIVATE Threads::Threads)
    endif()
    nggc_target_warnings(nggc_phase_verifier_tests)

    add_test(NAME nggc_phase_verifier_tests
             COMMAND nggc_phase_verifier_tests)

    add_executable(nggc_remset_verifier_tests
        src/nggc_backend.c
        src/nggc.c
        tests/test_nggc_remset_verifier.c
    )
    target_include_directories(nggc_remset_verifier_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(nggc_remset_verifier_tests
        PRIVATE
            NGGC_ENABLE_VERIFY=1
            NGGC_ENABLE_TEST_HOOKS=1
    )
    target_link_libraries(nggc_remset_verifier_tests PRIVATE uhmas)
    if(NGGC_HAVE_THREADS_H)
        target_compile_definitions(nggc_remset_verifier_tests
            PRIVATE
                NGGC_HAVE_C11_THREADS=1
        )
        target_link_libraries(nggc_remset_verifier_tests
            PRIVATE Threads::Threads)
    endif()
    nggc_target_warnings(nggc_remset_verifier_tests)

    add_test(NAME nggc_remset_verifier_tests
             COMMAND nggc_remset_verifier_tests)

    add_executable(nggc_fault_injection_tests
        src/nggc_backend.c
        src/nggc.c
        tests/test_nggc_fault_injection.c
    )
    target_include_directories(nggc_fault_injection_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(nggc_fault_injection_tests
        PRIVATE
            NGGC_ENABLE_FAULT_INJECTION=1
    )
    target_link_libraries(nggc_fault_injection_tests PRIVATE uhmas)
    nggc_target_warnings(nggc_fault_injection_tests)

    add_test(NAME nggc_fault_injection_tests
             COMMAND nggc_fault_injection_tests)

    if(NGGC_HAVE_THREADS_H)
        add_executable(nggc_concurrent_copy_tests
            src/nggc_backend.c
            src/nggc.c
            tests/test_nggc_concurrent_copy.c
        )
        target_include_directories(nggc_concurrent_copy_tests
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_compile_definitions(nggc_concurrent_copy_tests
            PRIVATE
                NGGC_ENABLE_VERIFY=1
                NGGC_ENABLE_TEST_HOOKS=1
                NGGC_HAVE_C11_THREADS=1
        )
        target_link_libraries(nggc_concurrent_copy_tests PRIVATE uhmas Threads::Threads)
        nggc_target_warnings(nggc_concurrent_copy_tests)
        add_test(NAME nggc_concurrent_copy_tests COMMAND nggc_concurrent_copy_tests)
        set_tests_properties(nggc_concurrent_copy_tests PROPERTIES TIMEOUT 30)

        add_executable(nggc_gc_pool_tests
            src/nggc_backend.c
            src/nggc.c
            tests/test_nggc_gc_pool.c
        )
        target_include_directories(nggc_gc_pool_tests
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_compile_definitions(nggc_gc_pool_tests
            PRIVATE
                NGGC_ENABLE_VERIFY=1
                NGGC_ENABLE_TEST_HOOKS=1
                NGGC_HAVE_C11_THREADS=1
        )
        target_link_libraries(nggc_gc_pool_tests PRIVATE uhmas Threads::Threads)
        nggc_target_warnings(nggc_gc_pool_tests)

        add_test(NAME nggc_gc_pool_tests COMMAND nggc_gc_pool_tests)
        set_tests_properties(nggc_gc_pool_tests PROPERTIES TIMEOUT 30)

        add_executable(nggc_reaper_tests
            tests/test_nggc_reaper.c
        )
        target_link_libraries(nggc_reaper_tests PRIVATE nggc Threads::Threads)
        nggc_target_warnings(nggc_reaper_tests)

        add_test(NAME nggc_reaper_tests COMMAND nggc_reaper_tests)

        add_executable(nggc_thread_tests
            tests/test_nggc_threads.c
        )
        target_link_libraries(nggc_thread_tests PRIVATE nggc Threads::Threads)
        nggc_target_warnings(nggc_thread_tests)

        add_test(NAME nggc_thread_tests COMMAND nggc_thread_tests)
    endif()
endif()
