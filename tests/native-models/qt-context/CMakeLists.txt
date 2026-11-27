# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.22)
project(PipeLangQtContextRuntime LANGUAGES CXX)

foreach(required PIPELANG_QT_CONTEXT_SDK_PREFIX PIPELANG_QT_CONTEXT_GENERATED_DIRECTORY)
    if(NOT IS_ABSOLUTE "${${required}}" OR NOT IS_DIRECTORY "${${required}}")
        message(FATAL_ERROR "Qt context fixture requires absolute existing ${required}")
    endif()
endforeach()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Qml)
set(qt_headers "${PIPELANG_QT_CONTEXT_SDK_PREFIX}/include/pipelang/qt")
add_library(qt_context_runtime STATIC
    "${qt_headers}/InvocationOutcome.hpp"
    "${qt_headers}/ActionReport.hpp"
    "${qt_headers}/ContextualObject.hpp"
    "${qt_headers}/ContextObject.hpp")
target_include_directories(qt_context_runtime PUBLIC "${PIPELANG_QT_CONTEXT_SDK_PREFIX}/include")
target_link_libraries(qt_context_runtime PUBLIC Qt6::Core Qt6::Qml)
target_compile_features(qt_context_runtime PUBLIC cxx_std_20)
target_compile_definitions(qt_context_runtime PUBLIC QT_NO_KEYWORDS)
set_target_properties(qt_context_runtime PROPERTIES AUTOMOC ON LINKER_LANGUAGE CXX)

add_executable(qt-context-runtime
    Runtime.cpp
    "${PIPELANG_QT_CONTEXT_GENERATED_DIRECTORY}/ContextStateQt.hpp"
    "${PIPELANG_QT_CONTEXT_GENERATED_DIRECTORY}/Model.hpp")
target_include_directories(qt-context-runtime PRIVATE "${PIPELANG_QT_CONTEXT_GENERATED_DIRECTORY}")
target_link_libraries(qt-context-runtime PRIVATE qt_context_runtime)
set_target_properties(qt-context-runtime PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
if(EMSCRIPTEN)
    target_compile_options(qt_context_runtime PUBLIC -fexceptions)
    target_link_options(qt-context-runtime PRIVATE -fexceptions -sDISABLE_EXCEPTION_CATCHING=0 -sEXIT_RUNTIME=1)
endif()

enable_testing()
add_test(NAME QtContextRuntime COMMAND qt-context-runtime)
