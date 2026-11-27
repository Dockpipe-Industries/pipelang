# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.21.1)
if(NOT REQUEST OR NOT ACTION MATCHES "^(build|test)$")
    message(FATAL_ERROR "Native dependency execution requires a request and action")
endif()
include("${REQUEST}")
include("${CMAKE_CURRENT_LIST_DIR}/VerifyInputs.cmake")
pipelang_verify_native_inputs()
file(REMOVE "${BUILD_DIRECTORY}/build-receipt.json")
file(MAKE_DIRECTORY "${BUILD_DIRECTORY}/home")
function(pipelang_execute)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env
        --unset=RUSTC_WRAPPER --unset=RUSTC_WORKSPACE_WRAPPER --unset=CARGO_ENCODED_RUSTFLAGS
        --unset=GOFLAGS --unset=GOWORK --unset=RUSTFLAGS
        "PATH=${tool_path}" ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIRECTORY}" RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "${LANGUAGE} native dependency ${ACTION} failed: ${status}")
    endif()
endfunction()
include("${CMAKE_CURRENT_LIST_DIR}/${LANGUAGE}.cmake")
# Build tools must not rewrite the admitted inputs, even after successful execution.
pipelang_verify_native_inputs()
if(ACTION STREQUAL "build")
    if(NOT EXISTS "${RESOLVED_ARTIFACT}" OR IS_DIRECTORY "${RESOLVED_ARTIFACT}")
        message(FATAL_ERROR "Native dependency did not produce its declared artifact")
    endif()
    file(SHA256 "${RESOLVED_ARTIFACT}" artifact_hash)
    file(WRITE "${BUILD_DIRECTORY}/build-receipt.json"
        "{\n  \"schema\": 1,\n  \"language\": \"${LANGUAGE}\",\n  \"target\": \"${target_triple}\",\n  \"toolchain_sha256\": \"${TOOLCHAIN_SHA256}\",\n  \"manifest_sha256\": \"${MANIFEST_SHA256}\",\n  \"lock_sha256\": \"${LOCK_SHA256}\",\n  \"artifact_sha256\": \"${artifact_hash}\"\n}\n")
endif()
