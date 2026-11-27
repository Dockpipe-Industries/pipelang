# SPDX-License-Identifier: Apache-2.0
if(NOT ACTION STREQUAL "build" OR NOT BUILD_TARGET MATCHES "^[A-Za-z][A-Za-z0-9_-]*$")
    message(FATAL_ERROR "CMake requires an explicit library build target")
endif()
pipelang_execute("${tool_cmake}" -S "${SOURCE_DIRECTORY}" -B "${BUILD_DIRECTORY}/cmake" -G "Unix Makefiles"
    "-DCMAKE_MAKE_PROGRAM=${tool_make}" "-DCMAKE_C_COMPILER=${tool_cc}"
    "-DCMAKE_CXX_COMPILER=${tool_cxx}" "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN}"
    -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_FULLY_DISCONNECTED=ON)
pipelang_execute("${tool_cmake}" --build "${BUILD_DIRECTORY}/cmake" --target "${BUILD_TARGET}" --parallel 1)
