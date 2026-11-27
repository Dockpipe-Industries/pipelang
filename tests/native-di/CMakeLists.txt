cmake_minimum_required(VERSION 3.22)
project(PipeLangNativeDiIntegration LANGUAGES CXX)

set(DI_EXPECTED_SDK_MANIFEST_SHA256 "" CACHE STRING "Exact accepted SDK manifest digest")
set(DI_MODEL_COMPILATION "HEADER_ONLY" CACHE STRING "Installed model compilation profile")
set_property(CACHE DI_MODEL_COMPILATION PROPERTY STRINGS HEADER_ONLY COMPILED)
option(DI_ENABLE_SANITIZERS "Instrument generated implementations and native consumers" OFF)
option(DI_BUILD_BENCHMARK "Build the equivalent-manual allocation and timing benchmark" OFF)

if(NOT DI_EXPECTED_SDK_MANIFEST_SHA256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "DI_EXPECTED_SDK_MANIFEST_SHA256 must identify the accepted installed SDK")
endif()
string(LENGTH "${DI_EXPECTED_SDK_MANIFEST_SHA256}" expected_digest_length)
if(NOT expected_digest_length EQUAL 64)
    message(FATAL_ERROR "DI_EXPECTED_SDK_MANIFEST_SHA256 must contain 64 hexadecimal characters")
endif()

find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
if(NOT PipeLangModel_MANIFEST_SHA256 STREQUAL DI_EXPECTED_SDK_MANIFEST_SHA256)
    message(FATAL_ERROR "Selected SDK does not match the explicitly accepted manifest")
endif()

pipelang_add_model(di_fixture
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/composition-fixture"
    ENTRY Startup.pipe
    NAMESPACE di_fixture
    COMPILATION "${DI_MODEL_COMPILATION}"
    COMPOSITION_FACTORY
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/di_fixture")

pipelang_add_model(di_shadow
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/shadow-fixture"
    ENTRY Startup.pipe
    NAMESPACE di_shadow
    COMPILATION "${DI_MODEL_COMPILATION}"
    COMPOSITION_FACTORY
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/di_shadow")

if(DI_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "The selected sanitizer profile is not qualified for this compiler")
    endif()
    target_compile_options(di_fixture INTERFACE -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(di_fixture INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_options(di_shadow INTERFACE -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(di_shadow INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
else()
    target_compile_options(di_fixture INTERFACE -O3)
    target_compile_options(di_shadow INTERFACE -O3)
endif()

add_executable(pipelang-di-integration CompositionIntegrationTests.cpp)
target_link_libraries(pipelang-di-integration PRIVATE di_fixture)
target_compile_features(pipelang-di-integration PRIVATE cxx_std_20)

add_executable(pipelang-di-concurrency CompositionConcurrencyTests.cpp)
target_link_libraries(pipelang-di-concurrency PRIVATE di_fixture)
target_compile_features(pipelang-di-concurrency PRIVATE cxx_std_20)

get_target_property(shadow_factory_header di_shadow PIPELANG_COMPOSITION_FACTORY)
add_executable(pipelang-di-shadow CompositionShadowTests.cpp)
target_link_libraries(pipelang-di-shadow PRIVATE di_shadow)
target_compile_definitions(pipelang-di-shadow PRIVATE DI_SHADOW_FACTORY_HEADER="${shadow_factory_header}")
target_compile_features(pipelang-di-shadow PRIVATE cxx_std_20)

if(DI_BUILD_BENCHMARK)
    if(DI_ENABLE_SANITIZERS)
        message(FATAL_ERROR "Performance measurements use the normal matched profile; sanitizer runs are correctness only")
    endif()
    add_executable(pipelang-di-benchmark CompositionBenchmark.cpp AllocationProbe.cpp)
    target_link_libraries(pipelang-di-benchmark PRIVATE di_fixture)
    target_compile_features(pipelang-di-benchmark PRIVATE cxx_std_20)
endif()

enable_testing()
add_test(NAME pipelang-di-integration COMMAND pipelang-di-integration)
add_test(NAME pipelang-di-concurrency COMMAND pipelang-di-concurrency)
add_test(NAME pipelang-di-shadow COMMAND pipelang-di-shadow)
