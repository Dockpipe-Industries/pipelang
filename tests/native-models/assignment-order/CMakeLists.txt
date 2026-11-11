cmake_minimum_required(VERSION 3.22)
project(PipeLangAssignmentOrder LANGUAGES CXX)

set(ASSIGNMENT_EXPECTED_SDK_MANIFEST_SHA256 "" CACHE STRING "Explicit accepted SDK manifest hash")
option(ASSIGNMENT_SANITIZERS "Instrument generated source and native assertions" OFF)
string(LENGTH "${ASSIGNMENT_EXPECTED_SDK_MANIFEST_SHA256}" digest_length)
if(NOT digest_length EQUAL 64 OR NOT ASSIGNMENT_EXPECTED_SDK_MANIFEST_SHA256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "An exact accepted SDK manifest SHA256 is required")
endif()
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
if(NOT PipeLangModel_MANIFEST_SHA256 STREQUAL ASSIGNMENT_EXPECTED_SDK_MANIFEST_SHA256)
    message(FATAL_ERROR "Installed SDK differs from the accepted manifest")
endif()
pipelang_add_model(assignment_model
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    SOURCE_ROOT src
    ENTRY Startup.pipe
    NAMESPACE assignment_fixture
    COMPOSITION_FACTORY
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_executable(pipelang-assignment-order AssignmentOrderTests.cpp)
target_link_libraries(pipelang-assignment-order PRIVATE assignment_model)
target_compile_features(pipelang-assignment-order PRIVATE cxx_std_20)
if(ASSIGNMENT_SANITIZERS)
    target_compile_options(pipelang-assignment-order PRIVATE -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pipelang-assignment-order PRIVATE -fsanitize=address,undefined)
else()
    target_compile_options(pipelang-assignment-order PRIVATE -O3)
endif()
enable_testing()
add_test(NAME pipelang-assignment-order COMMAND pipelang-assignment-order)
