cmake_minimum_required(VERSION 3.21.1)
project(PipeLangJsonContracts LANGUAGES CXX)

find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
set(PIPELANG_JSON_LIBRARY_ROOT "${PipeLangModel_SOURCE_LIBRARY_DIRECTORY}/json"
    CACHE PATH "Immutable PipeLang.Json source library")
set(PIPELANG_TESTING_LIBRARY_ROOT "${PipeLangModel_SOURCE_LIBRARY_DIRECTORY}/testing"
    CACHE PATH "Immutable PipeLang.Testing source library")
set(references "")
foreach(library_root IN ITEMS "${PIPELANG_JSON_LIBRARY_ROOT}" "${PIPELANG_TESTING_LIBRARY_ROOT}")
    file(REAL_PATH "${library_root}" library_root)
    file(SHA256 "${library_root}/library.json" library_sha256)
    string(REPLACE "\\" "\\\\" library_json "${library_root}")
    string(REPLACE "\"" "\\\"" library_json "${library_json}")
    string(REPLACE "\n" "\\n" library_json "${library_json}")
    string(REPLACE "\r" "\\r" library_json "${library_json}")
    string(REPLACE "\t" "\\t" library_json "${library_json}")
    if(NOT references STREQUAL "")
        string(APPEND references ",")
    endif()
    string(APPEND references "{\"root\":\"${library_json}\",\"descriptor\":\"library.json\",\"sha256\":\"${library_sha256}\"}")
endforeach()
set(source_references "${CMAKE_CURRENT_BINARY_DIR}/json-libraries.json")
file(WRITE "${source_references}"
    "{\"protocol\":\"pipelang.source-library-references.v1\",\"libraries\":[${references}]}\n")

pipelang_add_model(json_model
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    ENTRY Entry.pipe
    NAMESPACE pipelang_json_tests
    SOURCE_LIBRARIES "${source_references}"
    COMPILATION COMPILED
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

option(PIPELANG_JSON_SANITIZERS "Instrument JSON reader and generated source contracts" OFF)
set(json_reader PipeLang::JsonReader)
if(PIPELANG_JSON_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "JSON ASan/UBSan requires a compatible GNU or Clang compiler")
    endif()
    if(NOT DEFINED PipeLangModel_JSON_READER_SOURCE_DIRECTORY)
        message(FATAL_ERROR "Selected SDK does not provide immutable native JSON reader sources")
    endif()

    set(json_sources "${PipeLangModel_JSON_READER_SOURCE_DIRECTORY}")
    file(SHA256 "${json_sources}/third_party/nlohmann/json.hpp" json_header_sha256)
    if(NOT json_header_sha256 STREQUAL "aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63")
        message(FATAL_ERROR "Installed pinned JSON header checksum mismatch")
    endif()

    add_library(json_instrumented_reader STATIC "${json_sources}/Reader.cpp"
        "${json_sources}/DocumentWriter.cpp")
    get_target_property(json_includes PipeLang::JsonReader INTERFACE_INCLUDE_DIRECTORIES)
    target_include_directories(json_instrumented_reader PUBLIC "${json_includes}"
        PRIVATE "${json_sources}/third_party")
    target_compile_features(json_instrumented_reader PUBLIC cxx_std_20)
    set_target_properties(json_instrumented_reader PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(json_instrumented_reader PRIVATE
        -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(json_instrumented_reader INTERFACE -fsanitize=address,undefined)
    _pipelang_model_resolve_cxx_resource_profile(resource_profile resource_response resource_digest resource_dependency)
    _pipelang_model_apply_cxx_resource_profile(json_instrumented_reader
        "${resource_profile}" "${resource_response}" "${resource_digest}")
    if(resource_response)
        target_compile_options(json_instrumented_reader PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:@${resource_response}>")
    endif()
    set(json_reader json_instrumented_reader)

    target_compile_options(json_model INTERFACE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(json_model INTERFACE -fsanitize=address,undefined)
endif()

add_executable(json_contracts Main.cpp)
add_executable(json_wire_source_contracts WireInputSourceContracts.cpp)
target_link_libraries(json_wire_source_contracts PRIVATE json_model "${json_reader}")
add_executable(json_document_writer_source_contracts DocumentWriterSourceContracts.cpp)
target_link_libraries(json_document_writer_source_contracts PRIVATE json_model "${json_reader}")
add_executable(json_value_render_source_contracts ValueRenderSourceContracts.cpp)
target_link_libraries(json_value_render_source_contracts PRIVATE json_model "${json_reader}")
target_link_libraries(json_contracts PRIVATE json_model "${json_reader}")
add_executable(json_string_index_source_contracts StringIndexSourceContracts.cpp)
target_link_libraries(json_string_index_source_contracts PRIVATE json_model "${json_reader}")
add_executable(json_string_index_contracts StringIndexContracts.cpp)
add_executable(json_native_contracts NativeContracts.cpp)
add_executable(json_wire_input_contracts WireInputContracts.cpp)
add_executable(json_document_writer_contracts DocumentWriterContracts.cpp)
add_executable(json_value_render_contracts ValueRenderContracts.cpp)
add_executable(json_maximum_input_contracts MaximalInputContracts.cpp)
foreach(target IN ITEMS json_native_contracts json_maximum_input_contracts json_string_index_contracts
        json_value_render_contracts json_document_writer_contracts json_wire_input_contracts)
    target_link_libraries(${target} PRIVATE "${json_reader}")
    if(PIPELANG_JSON_SANITIZERS)
        target_compile_options(${target} PRIVATE -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()
foreach(target IN ITEMS json_contracts json_native_contracts json_maximum_input_contracts
        json_string_index_contracts json_string_index_source_contracts json_value_render_contracts
        json_value_render_source_contracts json_document_writer_contracts json_document_writer_source_contracts
        json_wire_input_contracts json_wire_source_contracts)
    target_compile_features(${target} PRIVATE cxx_std_20)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
enable_testing()
add_test(NAME json_contracts COMMAND json_contracts)
add_test(NAME json_native_contracts COMMAND json_native_contracts)
add_test(NAME json_maximum_input_contracts COMMAND json_maximum_input_contracts)

add_test(NAME json_string_index_contracts COMMAND json_string_index_contracts)
add_test(NAME json_string_index_source_contracts COMMAND json_string_index_source_contracts)

add_test(NAME json_value_render_contracts COMMAND json_value_render_contracts)
add_test(NAME json_value_render_source_contracts COMMAND json_value_render_source_contracts)

add_test(NAME json_document_writer_contracts COMMAND json_document_writer_contracts)
add_test(NAME json_document_writer_source_contracts COMMAND json_document_writer_source_contracts
    "${CMAKE_CURRENT_BINARY_DIR}/json-document-writer-source-out.json")

add_test(NAME json_wire_input_contracts COMMAND json_wire_input_contracts)
add_test(NAME json_wire_source_contracts COMMAND json_wire_source_contracts)
