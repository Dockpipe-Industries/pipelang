cmake_minimum_required(VERSION 3.20)
project(pipelang_cpp_emitter LANGUAGES CXX)

set(PIPELANG_JSON_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nlohmann/json.hpp")
if(NOT EXISTS "${PIPELANG_JSON_HEADER}")
    message(FATAL_ERROR "Missing pinned nlohmann/json.hpp")
endif()
file(SHA256 "${PIPELANG_JSON_HEADER}" PIPELANG_JSON_SHA256)
if(NOT PIPELANG_JSON_SHA256 STREQUAL "aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63")
    message(FATAL_ERROR "Pinned nlohmann/json.hpp checksum mismatch")
endif()

set(PIPELANG_RUNTIME_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/pipelang/cppbackend/runtime.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PIPELANG_RUNTIME_FILE}")
file(READ "${PIPELANG_RUNTIME_FILE}" PIPELANG_RUNTIME_SUPPORT)
string(FIND "${PIPELANG_RUNTIME_SUPPORT}" ")PL_RUNTIME\"" PIPELANG_RUNTIME_DELIMITER)
if(NOT PIPELANG_RUNTIME_DELIMITER EQUAL -1)
    message(FATAL_ERROR "Runtime support contains the embedding delimiter")
endif()
configure_file(runtime_support.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/runtime_support.hpp" @ONLY)

set(PIPELANG_FLOAT64_RUNTIME_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/pipelang/cppbackend/float64_runtime.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PIPELANG_FLOAT64_RUNTIME_FILE}")
file(READ "${PIPELANG_FLOAT64_RUNTIME_FILE}" PIPELANG_FLOAT64_SUPPORT)
string(FIND "${PIPELANG_FLOAT64_SUPPORT}" ")PL_FLOAT64\"" PIPELANG_FLOAT64_DELIMITER)
if(NOT PIPELANG_FLOAT64_DELIMITER EQUAL -1)
    message(FATAL_ERROR "Float64 runtime support contains the embedding delimiter")
endif()
configure_file(float64_support.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/float64_support.hpp" @ONLY)

add_executable(pipelang-cpp-emitter
    main.cpp
    emitter.cpp
    float64_profile.cpp
    core_json.cpp
    sha256.cpp
)
target_compile_features(pipelang-cpp-emitter PRIVATE cxx_std_17)
set_target_properties(pipelang-cpp-emitter PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(pipelang-cpp-emitter PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pipelang-cpp-emitter PRIVATE -Wall -Wextra -Wpedantic)
endif()
