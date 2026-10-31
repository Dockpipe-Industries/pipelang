cmake_minimum_required(VERSION 3.21.1)
project(PipeLangContainmentBootstrap LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../../sdk/pipeview/model/cmake/NativeCompilerProfile.cmake")
add_library(pipelang_tool_support STATIC
    ../support/sha256.cpp ../support/files.cpp ../support/json.cpp
    ../support/process.cpp ../support/executable.cpp)
target_compile_features(pipelang_tool_support PUBLIC cxx_std_20)
if(WIN32)
    target_sources(pipelang_tool_support PRIVATE ../support/process_windows.cpp ../support/files_windows.cpp)
    target_link_libraries(pipelang_tool_support PUBLIC advapi32 psapi)
    target_compile_definitions(pipelang_tool_support PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX)
endif()
set(PIPELANG_BUILD_SDK_TOOL_TESTS ON)
add_subdirectory(../containedexec containedexec)

pipelang_native_finalize("${CMAKE_CURRENT_SOURCE_DIR}")
