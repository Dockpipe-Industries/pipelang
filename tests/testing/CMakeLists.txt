cmake_minimum_required(VERSION 3.21.1)
project(PipeLangTestingContracts LANGUAGES CXX)

find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
set(PIPELANG_TESTING_LIBRARY_ROOT "${PipeLangModel_SOURCE_LIBRARY_DIRECTORY}/testing"
    CACHE PATH "Immutable installed PipeLang.Testing source library")
file(REAL_PATH "${PIPELANG_TESTING_LIBRARY_ROOT}" testing_library_root)
file(SHA256 "${testing_library_root}/library.json" testing_library_sha256)
# Encode the selected absolute path without changing its identity.
string(REPLACE "\\" "\\\\" testing_library_json "${testing_library_root}")
string(REPLACE "\"" "\\\"" testing_library_json "${testing_library_json}")
string(REPLACE "\n" "\\n" testing_library_json "${testing_library_json}")
string(REPLACE "\r" "\\r" testing_library_json "${testing_library_json}")
string(REPLACE "\t" "\\t" testing_library_json "${testing_library_json}")
set(testing_references "${CMAKE_CURRENT_BINARY_DIR}/testing-libraries.json")
file(WRITE "${testing_references}"
    "{\"protocol\":\"pipelang.source-library-references.v1\",\"libraries\":[{\"root\":\"${testing_library_json}\",\"descriptor\":\"library.json\",\"sha256\":\"${testing_library_sha256}\"}]}\n")

pipelang_add_model(testing_model
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    ENTRY Entry.pipe
    NAMESPACE pipelang_testing
    SOURCE_LIBRARIES "${testing_references}"
    COMPILATION COMPILED
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

option(PIPELANG_TESTING_SANITIZERS "Instrument the source library and native result checker" OFF)
if(PIPELANG_TESTING_SANITIZERS)
    target_compile_options(testing_model INTERFACE
        -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(testing_model INTERFACE -fsanitize=address,undefined)
endif()

add_executable(testing_contracts Main.cpp)
target_link_libraries(testing_contracts PRIVATE testing_model)
target_compile_features(testing_contracts PRIVATE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(testing_contracts PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
enable_testing()
add_test(NAME testing_contracts COMMAND testing_contracts)

# Additive scoped-probe contracts use the same generated Source/library owner.
add_executable(testing_probe_contracts ProbeMain.cpp)
target_link_libraries(testing_probe_contracts PRIVATE testing_model)
target_compile_features(testing_probe_contracts PRIVATE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(testing_probe_contracts PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME testing_probe_contracts COMMAND testing_probe_contracts)
