cmake_minimum_required(VERSION 3.21.1)
project(PipeLangContainmentBootstrap LANGUAGES CXX)
add_library(pipelang_tool_support STATIC
    ../support/sha256.cpp ../support/files.cpp ../support/json.cpp
    ../support/process.cpp ../support/executable.cpp)
target_compile_features(pipelang_tool_support PUBLIC cxx_std_20)
set(PIPELANG_BUILD_SDK_TOOL_TESTS ON)
add_subdirectory(../containedexec containedexec)
