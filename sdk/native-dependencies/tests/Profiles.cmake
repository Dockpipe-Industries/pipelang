# SPDX-License-Identifier: Apache-2.0
# Caller owns workflow containment. This test creates only its fresh PROOF tree.
cmake_minimum_required(VERSION 3.21)

foreach(argument RESOLVER_PREFIX GO_TOOLCHAIN CMAKE_TOOLCHAIN_MANIFEST PROOF)
    if(NOT DEFINED ${argument} OR NOT IS_ABSOLUTE "${${argument}}")
        message(FATAL_ERROR "${argument} must be an explicit absolute path")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${RESOLVER_PREFIX}")
    message(FATAL_ERROR "RESOLVER_PREFIX must select an installed resolver")
endif()
foreach(toolchain GO_TOOLCHAIN CMAKE_TOOLCHAIN_MANIFEST)
    if(NOT EXISTS "${${toolchain}}" OR IS_DIRECTORY "${${toolchain}}")
        message(FATAL_ERROR "${toolchain} must select an existing toolchain manifest")
    endif()
endforeach()
if(EXISTS "${PROOF}")
    message(FATAL_ERROR "Select a fresh PROOF directory; existing evidence is preserved")
endif()
file(MAKE_DIRECTORY "${PROOF}/go" "${PROOF}/cmake" "${PROOF}/consumer"
    "${PROOF}/cache" "${PROOF}/logs")

file(WRITE "${PROOF}/go/go.mod" [=[module example.invalid/pipe-fixture

go 1.22
]=])
file(WRITE "${PROOF}/go/go.sum" "")
file(WRITE "${PROOF}/go/main.go" [=[package main

import "C"

//export PipeGoAdd
func PipeGoAdd(left C.int, right C.int) C.int {
    return left + right
}

func main() {}
]=])
file(WRITE "${PROOF}/cmake/CMakeLists.txt" [=[cmake_minimum_required(VERSION 3.21)
project(MixedNativeFixture LANGUAGES C CXX)
add_library(mixed_fixture STATIC cpart.c cpppart.cpp)
]=])
file(WRITE "${PROOF}/cmake/cpart.c" [=[int PipeCValue(void) {
    return 11;
}
]=])
file(WRITE "${PROOF}/cmake/cpppart.cpp" [=[extern "C" int PipeCppValue() {
    return 31;
}
]=])
file(WRITE "${PROOF}/cmake/native.lock" "profile-fixture-v1\nstdlib-only\n")
file(WRITE "${PROOF}/cmake/toolchain.cmake" [=[set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
]=])
file(WRITE "${PROOF}/consumer/go.cpp" [=[extern "C" int PipeGoAdd(int left, int right);
int main() {
    return PipeGoAdd(20, 22) == 42 ? 0 : 1;
}
]=])
file(WRITE "${PROOF}/consumer/cmake.cpp" [=[extern "C" int PipeCValue();
extern "C" int PipeCppValue();
int main() {
    return PipeCValue() + PipeCppValue() == 42 ? 0 : 1;
}
]=])
file(WRITE "${PROOF}/consumer/CMakeLists.txt" [=[cmake_minimum_required(VERSION 3.21)
project(NativeProfileCaller LANGUAGES C CXX)
find_package(PipeLangNativeDependencies 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_native_dependency(selected
    LANGUAGE "${PROFILE}"
    SOURCE_DIRECTORY "${DEPENDENCY_SOURCE}"
    MANIFEST "${DEPENDENCY_MANIFEST}" MANIFEST_SHA256 "${DEPENDENCY_MANIFEST_SHA256}"
    LOCK_FILE "${DEPENDENCY_LOCK}" LOCK_SHA256 "${DEPENDENCY_LOCK_SHA256}"
    TOOLCHAIN_FILE "${TOOLS}" TOOLCHAIN_SHA256 "${TOOLS_SHA256}"
    LIBRARY_NAME "${LIBRARY}"
    CACHE_DIRECTORY "${DEPENDENCY_CACHE}"
    CMAKE_TOOLCHAIN "${NATIVE_CMAKE_TOOLCHAIN}" CMAKE_TOOLCHAIN_SHA256 "${NATIVE_CMAKE_TOOLCHAIN_SHA256}"
    BUILD_TARGET mixed_fixture ARTIFACT libmixed_fixture.a
    SYSTEM_LIBRARIES pthread dl m)
add_executable(caller "${CALLER}")
target_link_libraries(caller PRIVATE selected)
]=])

# Preserve all stdout/stderr, including expected refusals, for caller review.
function(assert_process name expected_failure)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
        TIMEOUT 180)
    file(WRITE "${PROOF}/logs/${name}.log" "${output}${error}")
    if(expected_failure STREQUAL "")
        if(NOT "${status}" STREQUAL "0")
            message(FATAL_ERROR "${name} failed (${status}); see ${PROOF}/logs/${name}.log\n${output}${error}")
        endif()
    else()
        if("${status}" STREQUAL "0")
            message(FATAL_ERROR "${name} unexpectedly succeeded")
        endif()
        string(FIND "${output}${error}" "${expected_failure}" diagnostic)
        if(diagnostic LESS 0)
            message(FATAL_ERROR "${name} failed without expected diagnostic '${expected_failure}'; see ${PROOF}/logs/${name}.log")
        endif()
    endif()
    file(APPEND "${PROOF}/assertions.txt" "${name}: passed\n")
    message(STATUS "${name}: passed")
endfunction()

foreach(profile Go CMake)
    if(profile STREQUAL "Go")
        set(source "${PROOF}/go")
        set(manifest "${source}/go.mod")
        set(lock "${source}/go.sum")
        set(tools "${GO_TOOLCHAIN}")
        set(library gofixture)
        set(caller "${PROOF}/consumer/go.cpp")
    else()
        set(source "${PROOF}/cmake")
        set(manifest "${source}/CMakeLists.txt")
        set(lock "${source}/native.lock")
        set(tools "${CMAKE_TOOLCHAIN_MANIFEST}")
        set(library mixed_fixture)
        set(caller "${PROOF}/consumer/cmake.cpp")
    endif()
    set(build "${PROOF}/build-${profile}")
    set(native_toolchain "${PROOF}/cmake/toolchain.cmake")
    file(SHA256 "${manifest}" manifest_hash)
    file(SHA256 "${lock}" lock_hash)
    file(SHA256 "${tools}" tools_hash)
    file(SHA256 "${native_toolchain}" native_toolchain_hash)
    set(configuration
        "-DCMAKE_PREFIX_PATH=${RESOLVER_PREFIX}"
        "-DPROFILE=${profile}"
        "-DDEPENDENCY_SOURCE=${source}"
        "-DDEPENDENCY_MANIFEST=${manifest}"
        "-DDEPENDENCY_MANIFEST_SHA256=${manifest_hash}"
        "-DDEPENDENCY_LOCK=${lock}"
        "-DDEPENDENCY_LOCK_SHA256=${lock_hash}"
        "-DTOOLS=${tools}"
        "-DTOOLS_SHA256=${tools_hash}"
        "-DLIBRARY=${library}"
        "-DDEPENDENCY_CACHE=${PROOF}/cache"
        "-DNATIVE_CMAKE_TOOLCHAIN=${native_toolchain}"
        "-DNATIVE_CMAKE_TOOLCHAIN_SHA256=${native_toolchain_hash}"
        "-DCALLER=${caller}")
    assert_process("${profile}-configure" "" "${CMAKE_COMMAND}"
        -S "${PROOF}/consumer" -B "${build}" ${configuration})
    assert_process("${profile}-build" "" "${CMAKE_COMMAND}"
        --build "${build}" --parallel 1)
    assert_process("${profile}-call" "" "${build}/caller")

    # Exercise revalidation even after a successful incremental build exists.
    file(READ "${lock}" original_lock)
    file(APPEND "${lock}" "changed-lock\n")
    assert_process("${profile}-lock-refusal" "Dependency lock digest mismatch"
        "${CMAKE_COMMAND}" --build "${build}" --parallel 1)
    file(WRITE "${lock}" "${original_lock}")

    # Pin the altered manifest itself correctly; refuse the incorrect tool digest.
    file(READ "${tools}" bad_tools)
    string(REPEAT "0" 64 incorrect_digest)
    string(JSON bad_tools SET "${bad_tools}" tools cc sha256 "\"${incorrect_digest}\"")
    set(bad_tools_file "${PROOF}/${profile}-bad-tools.json")
    file(WRITE "${bad_tools_file}" "${bad_tools}")
    file(SHA256 "${bad_tools_file}" bad_tools_hash)
    set(bad_configuration ${configuration})
    list(REMOVE_ITEM bad_configuration "-DTOOLS=${tools}" "-DTOOLS_SHA256=${tools_hash}")
    list(APPEND bad_configuration "-DTOOLS=${bad_tools_file}" "-DTOOLS_SHA256=${bad_tools_hash}")
    assert_process("${profile}-tool-refusal" "Tool cc digest mismatch"
        "${CMAKE_COMMAND}" -S "${PROOF}/consumer" -B "${build}-bad-tool" ${bad_configuration})

    if(profile STREQUAL "CMake")
        set(unsupported_configuration ${configuration})
        list(REMOVE_ITEM unsupported_configuration "-DPROFILE=${profile}")
        list(APPEND unsupported_configuration "-DPROFILE=Python")
        assert_process(unsupported-language-refusal "Unsupported native dependency language"
            "${CMAKE_COMMAND}" -S "${PROOF}/consumer" -B "${build}-unsupported" ${unsupported_configuration})
    endif()
endforeach()
file(STRINGS "${PROOF}/assertions.txt" assertions)
list(LENGTH assertions assertion_count)
if(NOT assertion_count EQUAL 11)
    message(FATAL_ERROR "Expected eleven profile assertions, found ${assertion_count}")
endif()
message(STATUS "All eleven Go/C/C++ native ABI and refusal assertions passed")
