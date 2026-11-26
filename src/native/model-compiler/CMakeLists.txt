cmake_minimum_required(VERSION 3.20)
project(pipelang_native_models LANGUAGES CXX)

set(PIPELANG_JSON_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nlohmann/json.hpp")
file(SHA256 "${PIPELANG_JSON_HEADER}" PIPELANG_JSON_SHA256)
if(NOT PIPELANG_JSON_SHA256 STREQUAL "aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63")
    message(FATAL_ERROR "Pinned nlohmann/json.hpp checksum mismatch")
endif()

add_library(pipelang_model_compiler STATIC
    compiler.cpp composition.cpp CompositionGraph.cpp CompositionPlanner.cpp
    CompositionEmitter.cpp CompositionFactoryProfile.cpp ProjectModelFacets.cpp
    parser.cpp checker.cpp generator.cpp SemanticAnalysis.cpp SourceFormatting.cpp AsyncFinallyLowering.cpp NativeNumericExports.cpp constants.cpp
    ResourceBudget.cpp ScopedResourceBudget.cpp ../lexer/lexer.cpp)
target_compile_features(pipelang_model_compiler PUBLIC cxx_std_20)
target_include_directories(pipelang_model_compiler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/pipeview/model/native/include")
add_executable(pipelang-modelc main.cpp)
target_link_libraries(pipelang-modelc PRIVATE pipelang_model_compiler)
set_target_properties(pipelang-modelc pipelang_model_compiler PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pipelang-modelc PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(pipelang_model_compiler PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(PIPELANG_BUILD_CLASS_INHERITANCE_TESTS "Build the native class-inheritance compiler oracle" OFF)
if(PIPELANG_BUILD_CLASS_INHERITANCE_TESTS)
    add_executable(pipelang-class-inheritance-compiler-tests
        ../../../tests/native-models/class_inheritance_compiler.cpp)
    target_link_libraries(pipelang-class-inheritance-compiler-tests PRIVATE pipelang_model_compiler)
    if(DEFINED PIPELANG_MODEL_SOURCE_ROOT)
        target_compile_definitions(pipelang-class-inheritance-compiler-tests PRIVATE PIPELANG_CLASS_TRANSPORT=1)
        target_link_libraries(pipelang-class-inheritance-compiler-tests PRIVATE pipelang_tool_support)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(pipelang-class-inheritance-compiler-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

# Compiler and runtime contracts are exercised by the ordinary SDK test workflow.
if(PIPELANG_BUILD_SDK_TOOL_TESTS)
    add_executable(pipelang-loop-flow-compiler-tests
        ../../../tests/native-models/LoopFlowTests.cpp)
    target_link_libraries(pipelang-loop-flow-compiler-tests PRIVATE pipelang_model_compiler)
    target_compile_features(pipelang-loop-flow-compiler-tests PRIVATE cxx_std_20)
    set_target_properties(pipelang-loop-flow-compiler-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_executable(pipelang-generic-member-compiler-tests
        ../../../tests/native-models/GenericMemberCallsTests.cpp)
    target_link_libraries(pipelang-generic-member-compiler-tests PRIVATE pipelang_model_compiler)
    add_executable(pipelang-compiler-intelligence-tests
        ../../../tests/native-models/CompilerIntelligenceTests.cpp)
    target_link_libraries(pipelang-compiler-intelligence-tests PRIVATE pipelang_model_compiler)
    target_compile_features(pipelang-compiler-intelligence-tests PRIVATE cxx_std_20)
    set_target_properties(pipelang-compiler-intelligence-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_executable(pipelang-var-inference-compiler-tests
        ../../../tests/native-models/VarInferenceTests.cpp)
    target_link_libraries(pipelang-var-inference-compiler-tests PRIVATE pipelang_model_compiler)
    target_compile_features(pipelang-var-inference-compiler-tests PRIVATE cxx_std_20)
    set_target_properties(pipelang-var-inference-compiler-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_executable(pipelang-bounded-calls-compiler-tests
        ../../../tests/native-models/BoundedCallsTests.cpp)
    target_link_libraries(pipelang-bounded-calls-compiler-tests PRIVATE pipelang_model_compiler)
    target_compile_features(pipelang-bounded-calls-compiler-tests PRIVATE cxx_std_20)
    set_target_properties(pipelang-bounded-calls-compiler-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_executable(pipelang-execution-segments-compiler-tests
        ../../../tests/native-models/ExecutionSegmentsTests.cpp)
    target_link_libraries(pipelang-execution-segments-compiler-tests PRIVATE pipelang_model_compiler)
    target_compile_features(pipelang-execution-segments-compiler-tests PRIVATE cxx_std_20)
    set_target_properties(pipelang-execution-segments-compiler-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_executable(pipelang-independent-tokens-compiler-tests
        ../../../tests/native-models/IndependentTokensTests.cpp)
    target_link_libraries(pipelang-independent-tokens-compiler-tests PRIVATE pipelang_model_compiler)
    target_compile_features(pipelang-independent-tokens-compiler-tests PRIVATE cxx_std_20)
    set_target_properties(pipelang-independent-tokens-compiler-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_executable(pipelang-async-finally-compiler-tests
        ../../../tests/native-models/AsyncFinallyTests.cpp)
    target_link_libraries(pipelang-async-finally-compiler-tests PRIVATE pipelang_model_compiler)
    target_compile_features(pipelang-async-finally-compiler-tests PRIVATE cxx_std_20)
    set_target_properties(pipelang-async-finally-compiler-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_executable(pipelang-member-bindings-compiler-tests
        ../../../tests/native-models/MemberBindingsTests.cpp)
    target_link_libraries(pipelang-member-bindings-compiler-tests PRIVATE pipelang_model_compiler)
    target_compile_features(pipelang-member-bindings-compiler-tests PRIVATE cxx_std_20)
    set_target_properties(pipelang-member-bindings-compiler-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_executable(pipelang-empty-source-interface-compiler-tests
        ../../../tests/native-models/empty-source-interfaces/EmptySourceInterfaceTests.cpp)
    target_link_libraries(pipelang-empty-source-interface-compiler-tests PRIVATE pipelang_model_compiler)
    target_compile_features(pipelang-empty-source-interface-compiler-tests PRIVATE cxx_std_20)
    set_target_properties(pipelang-empty-source-interface-compiler-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_executable(pipelang-async-finally-scaling-compiler-tests
        ../../../tests/native-models/AsyncFinallyScalingTests.cpp)
    target_link_libraries(pipelang-async-finally-scaling-compiler-tests PRIVATE pipelang_model_compiler)
    target_compile_features(pipelang-async-finally-scaling-compiler-tests PRIVATE cxx_std_20)
    set_target_properties(pipelang-async-finally-scaling-compiler-tests PROPERTIES CXX_EXTENSIONS OFF)
    add_executable(pipelang-async-finally-storage-tests
        ../../../tests/native-models/AsyncFinallyStorageTests.cpp)
    target_include_directories(pipelang-async-finally-storage-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../sdk/pipeview/model/native/include")
    target_compile_features(pipelang-async-finally-storage-tests PRIVATE cxx_std_20)
    set_target_properties(pipelang-async-finally-storage-tests PROPERTIES CXX_EXTENSIONS OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(pipelang-loop-flow-compiler-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(pipelang-generic-member-compiler-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(pipelang-compiler-intelligence-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(pipelang-var-inference-compiler-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(pipelang-bounded-calls-compiler-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(pipelang-member-bindings-compiler-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(pipelang-async-finally-compiler-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(pipelang-execution-segments-compiler-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(pipelang-independent-tokens-compiler-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(pipelang-empty-source-interface-compiler-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(pipelang-async-finally-scaling-compiler-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(pipelang-async-finally-storage-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

if(PIPELANG_BUILD_SDK_TOOL_TESTS)
    add_executable(pipelang-source-formatting-tests ../../../tests/native-models/SourceFormattingTests.cpp)
    target_link_libraries(pipelang-source-formatting-tests PRIVATE pipelang_model_compiler)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(pipelang-source-formatting-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()
