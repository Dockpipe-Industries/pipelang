# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.21.1)
foreach(argument PROOF RESOLVER_PREFIX RUST_TOOLCHAIN CARGO_CACHE)
    if(NOT IS_ABSOLUTE "${${argument}}")
        message(FATAL_ERROR "${argument} must be an absolute path")
    endif()
endforeach()
if(EXISTS "${PROOF}")
    message(FATAL_ERROR "Select a fresh proof directory")
endif()
file(MAKE_DIRECTORY "${PROOF}/source/src" "${PROOF}/consumer")
file(WRITE "${PROOF}/source/Cargo.toml" [=[
[package]
name = "resolver-fixture"
version = "0.1.0"
edition = "2021"
[lib]
name = "resolver_fixture"
crate-type = ["staticlib"]
]=])
file(WRITE "${PROOF}/source/Cargo.lock" [=[
version = 4
[[package]]
name = "resolver-fixture"
version = "0.1.0"
]=])
file(WRITE "${PROOF}/source/src/lib.rs" [=[
#[no_mangle]
pub extern "C" fn pipe_rust_answer() -> i32 {
    42
}
]=])
file(WRITE "${PROOF}/consumer/main.cpp" [=[
extern "C" int pipe_rust_answer();
int main() {
    return pipe_rust_answer() == 42 ? 0 : 1;
}
]=])
file(WRITE "${PROOF}/consumer/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.21.1)
project(RustResolverCaller LANGUAGES CXX)
find_package(PipeLangNativeDependencies 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_native_dependency(rust_fixture LANGUAGE Rust
    SOURCE_DIRECTORY "${FIXTURE_SOURCE}"
    MANIFEST "${FIXTURE_SOURCE}/Cargo.toml" MANIFEST_SHA256 "${MANIFEST_HASH}"
    LOCK_FILE "${FIXTURE_SOURCE}/Cargo.lock" LOCK_SHA256 "${LOCK_HASH}"
    TOOLCHAIN_FILE "${TOOLS}" TOOLCHAIN_SHA256 "${TOOLS_HASH}"
    CACHE_DIRECTORY "${CACHE}" LIBRARY_NAME resolver_fixture PACKAGE resolver-fixture
    SYSTEM_LIBRARIES dl pthread m)
add_executable(caller main.cpp)
target_link_libraries(caller PRIVATE rust_fixture)
]=])
function(assert_process name expected_error)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 180)
    file(WRITE "${PROOF}/${name}.output" "${output}\n${error}")
    if(expected_error STREQUAL "")
        if(NOT status EQUAL 0)
            message(FATAL_ERROR "${name} failed: ${status}; see its output")
        endif()
    elseif(status EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected_error}")
        message(FATAL_ERROR "${name} did not produce the required refusal")
    endif()
    file(APPEND "${PROOF}/assertions.txt" "${name}: passed\n")
endfunction()
file(SHA256 "${PROOF}/source/Cargo.toml" manifest_hash)
file(SHA256 "${PROOF}/source/Cargo.lock" lock_hash)
file(SHA256 "${RUST_TOOLCHAIN}" tools_hash)
set(configuration
    "-DCMAKE_PREFIX_PATH=${RESOLVER_PREFIX}" "-DFIXTURE_SOURCE=${PROOF}/source"
    "-DMANIFEST_HASH=${manifest_hash}" "-DLOCK_HASH=${lock_hash}"
    "-DTOOLS=${RUST_TOOLCHAIN}" "-DTOOLS_HASH=${tools_hash}" "-DCACHE=${CARGO_CACHE}")
assert_process(configure "" "${CMAKE_COMMAND}" -S "${PROOF}/consumer" -B "${PROOF}/build" ${configuration})
assert_process(build "" "${CMAKE_COMMAND}" --build "${PROOF}/build" --parallel 1)
assert_process(call "" "${PROOF}/build/caller")
file(READ "${PROOF}/source/Cargo.lock" original_lock)
file(APPEND "${PROOF}/source/Cargo.lock" "# changed after configure\n")
assert_process(lock-refusal "Dependency lock digest mismatch" "${CMAKE_COMMAND}" --build "${PROOF}/build" --parallel 1)
file(WRITE "${PROOF}/source/Cargo.lock" "${original_lock}")
file(READ "${RUST_TOOLCHAIN}" bad_tools)
string(REPEAT "0" 64 incorrect_digest)
string(JSON bad_tools SET "${bad_tools}" tools rustc sha256 "\"${incorrect_digest}\"")
file(WRITE "${PROOF}/bad-tools.json" "${bad_tools}")
file(SHA256 "${PROOF}/bad-tools.json" bad_tools_hash)
list(REMOVE_ITEM configuration "-DTOOLS=${RUST_TOOLCHAIN}" "-DTOOLS_HASH=${tools_hash}")
list(APPEND configuration "-DTOOLS=${PROOF}/bad-tools.json" "-DTOOLS_HASH=${bad_tools_hash}")
assert_process(tool-refusal "Tool rustc digest mismatch" "${CMAKE_COMMAND}" -S "${PROOF}/consumer" -B "${PROOF}/bad-build" ${configuration})
message(STATUS "Rust C ABI and incremental admission refusal assertions passed")
