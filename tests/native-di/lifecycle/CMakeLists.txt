cmake_minimum_required(VERSION 3.22)
project(PipeLangOwnedLifecycleIntegration LANGUAGES CXX)

set(DI_EXPECTED_SDK_MANIFEST_SHA256 "" CACHE STRING "Explicit accepted installed SDK manifest SHA256")
set(DI_MODEL_COMPILATION "HEADER_ONLY" CACHE STRING "Installed model compilation mode")
set_property(CACHE DI_MODEL_COMPILATION PROPERTY STRINGS HEADER_ONLY COMPILED)
option(DI_ENABLE_SANITIZERS "Instrument generated source implementations and native consumers" OFF)
option(DI_BUILD_LIFECYCLE_BENCHMARK "Build the equivalent manual lifecycle benchmark" OFF)

string(LENGTH "${DI_EXPECTED_SDK_MANIFEST_SHA256}" expected_digest_length)
if(NOT expected_digest_length EQUAL 64 OR NOT DI_EXPECTED_SDK_MANIFEST_SHA256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "An exact accepted SDK manifest SHA256 is required")
endif()
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
if(NOT PipeLangModel_MANIFEST_SHA256 STREQUAL DI_EXPECTED_SDK_MANIFEST_SHA256)
    message(FATAL_ERROR "Installed SDK differs from the explicitly accepted manifest")
endif()

pipelang_add_model(lifecycle_model
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../lifecycle-fixture"
    ENTRY Startup.pipe
    NAMESPACE lifecycle_fixture
    COMPILATION "${DI_MODEL_COMPILATION}"
    COMPOSITION_FACTORY
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(DI_ENABLE_SANITIZERS)
    target_compile_options(lifecycle_model INTERFACE -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(lifecycle_model INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
else()
    target_compile_options(lifecycle_model INTERFACE -O3)
endif()

foreach(consumer IN ITEMS Integration Async Hostile)
    string(TOLOWER "${consumer}" consumer_name)
    add_executable("pipelang-lifecycle-${consumer_name}" "Lifecycle${consumer}Tests.cpp")
    target_link_libraries("pipelang-lifecycle-${consumer_name}" PRIVATE lifecycle_model)
    target_compile_features("pipelang-lifecycle-${consumer_name}" PRIVATE cxx_std_20)
endforeach()

if(DI_BUILD_LIFECYCLE_BENCHMARK)
    if(DI_ENABLE_SANITIZERS)
        message(FATAL_ERROR "Normal performance and sanitizer correctness are separate profiles")
    endif()
    add_executable(pipelang-lifecycle-benchmark LifecycleBenchmark.cpp ../AllocationProbe.cpp)
    target_link_libraries(pipelang-lifecycle-benchmark PRIVATE lifecycle_model)
    target_compile_features(pipelang-lifecycle-benchmark PRIVATE cxx_std_20)
endif()

enable_testing()
add_test(NAME pipelang-lifecycle-integration COMMAND pipelang-lifecycle-integration)
add_test(NAME pipelang-lifecycle-async COMMAND pipelang-lifecycle-async)
add_test(NAME pipelang-lifecycle-hostile COMMAND pipelang-lifecycle-hostile)
