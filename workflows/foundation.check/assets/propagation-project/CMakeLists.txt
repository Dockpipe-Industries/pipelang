cmake_minimum_required(VERSION 3.22)
project(PipeLangResultPropagationProject LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
if(NOT PipeLangModel_MANIFEST_SHA256 STREQUAL FOUNDATION_SDK_SHA256)
    message(FATAL_ERROR "Result propagation project SDK manifest changed")
endif()
file(READ "${FOUNDATION_FIXTURE_SOURCE}" fixture)
string(REPLACE "public Class ResultPropagation {"
    "public Class ResultPropagation { public ResultPropagation() {}"
    fixture "${fixture}")
string(REPLACE "public Class AsyncPropagation {"
    "public Class AsyncPropagation { public AsyncPropagation() {}"
    fixture "${fixture}")
set(project_root "${CMAKE_CURRENT_BINARY_DIR}/project")
file(MAKE_DIRECTORY "${project_root}/src")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/ObjectTargets.pipe" object_targets)
string(REPLACE "public Class ResultPropagation {"
    "public Class ResultPropagation { ${object_targets}"
    fixture "${fixture}")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/HostPort.pipe" host_port)
file(WRITE "${project_root}/src/ResultPropagation.pipe" "module Propagation;\n${fixture}\n${host_port}")
file(WRITE "${project_root}/src/Startup.pipe"
    "module Composition;\nimport Propagation;\npublic composition Startup {\n    scoped input HostPort;\n    scoped ResultPropagation = ResultPropagation;\n    scoped AsyncPropagation = AsyncPropagation;\n    scoped PortClient = PortClient;\n    export ResultPropagation;\n    export AsyncPropagation;\n    export PortClient;\n}\n")
pipelang_add_model(result_propagation_model
    PROJECT_ROOT "${project_root}" SOURCE_ROOT src ENTRY Startup.pipe
    NAMESPACE propagation_project COMPOSITION_FACTORY
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/result_propagation")
add_executable(result-propagation-project main.cpp)
target_include_directories(result-propagation-project PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(result-propagation-project PRIVATE result_propagation_model)
target_compile_features(result-propagation-project PRIVATE cxx_std_20)
target_compile_options(result-propagation-project PRIVATE -Wall -Wextra -Werror)
if(FOUNDATION_SANITIZERS)
    target_compile_options(result-propagation-project PRIVATE -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(result-propagation-project PRIVATE -fsanitize=address,undefined)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(result-propagation-project PRIVATE -fno-inline
            --param=ggc-min-expand=10 --param=ggc-min-heapsize=4096)
    endif()
endif()
if(EMSCRIPTEN)
    add_executable(result-propagation-direct result_propagation.cpp)
    target_include_directories(result-propagation-direct PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(result-propagation-direct PRIVATE PipeLang::ModelRuntime)
    target_compile_features(result-propagation-direct PRIVATE cxx_std_20)
    target_compile_options(result-propagation-direct PRIVATE -O1 -Wall -Wextra -Werror -fexceptions)
    target_link_options(result-propagation-direct PRIVATE -O1 -fexceptions
        "-sDISABLE_EXCEPTION_CATCHING=0" "-sALLOW_MEMORY_GROWTH=1"
        "-sEXIT_RUNTIME=1" "-sENVIRONMENT=node")
    target_compile_options(result-propagation-project PRIVATE -O1 -fexceptions)
    target_link_options(result-propagation-project PRIVATE -O1 -fexceptions
        "-sDISABLE_EXCEPTION_CATCHING=0" "-sALLOW_MEMORY_GROWTH=1"
        "-sEXIT_RUNTIME=1" "-sENVIRONMENT=node")
endif()
