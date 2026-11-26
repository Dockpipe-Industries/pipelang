cmake_minimum_required(VERSION 3.21.1)
project(PipeLangCryptoSourceContracts LANGUAGES CXX)

find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
find_package(PipeLangCryptoStream 0.1.0 EXACT CONFIG REQUIRED)
foreach(required IN ITEMS PIPELANG_TESTING_LIBRARY_ROOT PIPELANG_TESTING_LIBRARY_SHA256)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Explicit ${required} required for the provisioned scoped-probe source library")
    endif()
endforeach()

if(NOT IS_ABSOLUTE "${PIPELANG_TESTING_LIBRARY_ROOT}")
    message(FATAL_ERROR "PIPELANG_TESTING_LIBRARY_ROOT must select an explicit absolute path")
endif()

file(SHA256 "${PIPELANG_TESTING_LIBRARY_ROOT}/library.json" testing_sha256)
if(NOT testing_sha256 STREQUAL PIPELANG_TESTING_LIBRARY_SHA256)
    message(FATAL_ERROR "Selected scoped-probe Testing descriptor differs from its explicit pin")
endif()

function(source_reference output root checksum)
    file(REAL_PATH "${root}" library_root)
    string(REPLACE "\\" "\\\\" encoded_root "${library_root}")
    string(REPLACE "\"" "\\\"" encoded_root "${encoded_root}")
    string(REPLACE "\n" "\\n" encoded_root "${encoded_root}")
    string(REPLACE "\r" "\\r" encoded_root "${encoded_root}")
    string(REPLACE "\t" "\\t" encoded_root "${encoded_root}")
    set(${output} "{\"root\":\"${encoded_root}\",\"descriptor\":\"library.json\",\"sha256\":\"${checksum}\"}" PARENT_SCOPE)
endfunction()

source_reference(crypto_reference "${PipeLangCryptoStream_SOURCE_LIBRARY_DIRECTORY}"
    "${PipeLangCryptoStream_SOURCE_LIBRARY_SHA256}")
source_reference(testing_reference "${PIPELANG_TESTING_LIBRARY_ROOT}" "${PIPELANG_TESTING_LIBRARY_SHA256}")
set(source_references "${CMAKE_CURRENT_BINARY_DIR}/crypto-libraries.json")
file(WRITE "${source_references}"
    "{\"protocol\":\"pipelang.source-library-references.v1\",\"libraries\":[${crypto_reference},${testing_reference}]}\n")

pipelang_add_model(crypto_model
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    ENTRY Entry.pipe
    NAMESPACE pipelang_crypto_tests
    SOURCE_LIBRARIES "${source_references}"
    COMPILATION COMPILED
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

option(PIPELANG_CRYPTO_SOURCE_SANITIZERS "Instrument adapter, native fixture and generated Source contracts" OFF)
if(PIPELANG_CRYPTO_SOURCE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "This source-contract sanitizer lane requires GNU or Clang")
    endif()

    target_compile_options(crypto_model INTERFACE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(crypto_model INTERFACE -fsanitize=address,undefined)
endif()

add_executable(crypto_source_contracts Main.cpp)
target_link_libraries(crypto_source_contracts PRIVATE crypto_model PipeLang::CryptoStream)
target_compile_features(crypto_source_contracts PRIVATE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(crypto_source_contracts PRIVATE -Wall -Wextra -Wpedantic -Werror)
else()
    message(FATAL_ERROR "The Linux deterministic cancellation fixture requires GNU-compatible linker wrapping")
endif()
target_link_options(crypto_source_contracts PRIVATE
    -Wl,--wrap=pipelang_crypto_stream_v2_update
    -Wl,--wrap=pipelang_crypto_stream_v2_finish)

enable_testing()
add_test(NAME crypto_source_contracts COMMAND crypto_source_contracts)
set_tests_properties(crypto_source_contracts PROPERTIES TIMEOUT 120)
