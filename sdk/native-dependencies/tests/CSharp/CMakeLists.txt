cmake_minimum_required(VERSION 3.21.1)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/PinnedInputs.cmake")
    message(FATAL_ERROR "Run Prepare.cmake with a successful actual locked restore first")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/PinnedInputs.cmake")
file(READ "${FIXTURE_TOOLCHAIN}" tools)
string(JSON fixture_cxx GET "${tools}" tools cxx path)
string(JSON expected_cxx_hash GET "${tools}" tools cxx sha256)
file(SHA256 "${fixture_cxx}" actual_cxx_hash)
if(NOT actual_cxx_hash STREQUAL expected_cxx_hash)
    message(FATAL_ERROR "Native ABI caller compiler checksum mismatch")
endif()
set(CMAKE_CXX_COMPILER "${fixture_cxx}" CACHE FILEPATH "Pinned fixture compiler")
project(PipeLangCSharpNativeAbiFixture LANGUAGES CXX)
find_package(PipeLangNativeDependencies 0.1.0 EXACT CONFIG REQUIRED)

pipelang_add_native_dependency(csharp_fixture
    LANGUAGE CSharp
    SOURCE_DIRECTORY "${FIXTURE_SOURCE}"
    MANIFEST "${FIXTURE_SOURCE}/ResolverNativeFixture.csproj"
    MANIFEST_SHA256 "${FIXTURE_MANIFEST_SHA256}"
    LOCK_FILE "${FIXTURE_SOURCE}/packages.lock.json"
    LOCK_SHA256 "${FIXTURE_LOCK_SHA256}"
    TOOLCHAIN_FILE "${FIXTURE_TOOLCHAIN}"
    TOOLCHAIN_SHA256 "${FIXTURE_TOOLCHAIN_SHA256}"
    RESTORE_ASSETS "${FIXTURE_SOURCE}/obj/project.assets.json"
    RESTORE_ASSETS_SHA256 "${FIXTURE_ASSETS_SHA256}"
    CACHE_DIRECTORY "${FIXTURE_CACHE}"
    LIBRARY_NAME ResolverNativeFixture)

add_executable(native_csharp_caller caller.cpp)
target_compile_features(native_csharp_caller PRIVATE cxx_std_20)
target_link_libraries(native_csharp_caller PRIVATE csharp_fixture)
enable_testing()
add_test(NAME CSharpExportAbi COMMAND native_csharp_caller)
set_tests_properties(CSharpExportAbi PROPERTIES TIMEOUT 10)
