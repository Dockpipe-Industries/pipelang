cmake_minimum_required(VERSION 3.22)
project(PipeLangResultInterfaceProject LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
if(NOT PipeLangModel_MANIFEST_SHA256 STREQUAL FOUNDATION_SDK_SHA256)
    message(FATAL_ERROR "Result interface project SDK manifest changed")
endif()
file(READ "${FOUNDATION_FIXTURE_SOURCE}" fixture)
string(REPLACE "public Class ResultInterfaceModel {"
    "public Class ResultInterfaceModel { public ResultInterfaceModel() {}"
    fixture "${fixture}")
set(project_root "${CMAKE_CURRENT_BINARY_DIR}/project")
file(MAKE_DIRECTORY "${project_root}/src")
file(WRITE "${project_root}/src/ResultInterface.pipe" "module Results;\n${fixture}")
file(WRITE "${project_root}/src/Startup.pipe"
    "module Composition;\nimport Results;\npublic composition Startup {\n    scoped ResultInterfaceModel = ResultInterfaceModel;\n    export ResultInterfaceModel;\n}\n")
pipelang_add_model(result_interface_model
    PROJECT_ROOT "${project_root}" SOURCE_ROOT src ENTRY Startup.pipe
    NAMESPACE interface_project COMPOSITION_FACTORY
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/result_interface")
add_executable(result-interface-project main.cpp)
target_include_directories(result-interface-project PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(result-interface-project PRIVATE result_interface_model)
target_compile_features(result-interface-project PRIVATE cxx_std_20)
target_compile_options(result-interface-project PRIVATE -Wall -Wextra -Werror)
if(FOUNDATION_SANITIZERS)
    target_compile_options(result-interface-project PRIVATE -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(result-interface-project PRIVATE -fsanitize=address,undefined)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(result-interface-project PRIVATE -fno-inline
            --param=ggc-min-expand=10 --param=ggc-min-heapsize=4096)
    endif()
endif()
if(EMSCRIPTEN)
    add_executable(result-interface-direct result_interface.cpp)
    target_include_directories(result-interface-direct PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(result-interface-direct PRIVATE PipeLang::ModelRuntime)
    target_compile_features(result-interface-direct PRIVATE cxx_std_20)
    target_compile_options(result-interface-direct PRIVATE -O1 -Wall -Wextra -Werror -fexceptions)
    target_link_options(result-interface-direct PRIVATE -O1 -fexceptions
        "-sDISABLE_EXCEPTION_CATCHING=0" "-sALLOW_MEMORY_GROWTH=1"
        "-sEXIT_RUNTIME=1" "-sENVIRONMENT=node")
    target_compile_options(result-interface-project PRIVATE -O1 -fexceptions)
    target_link_options(result-interface-project PRIVATE -O1 -fexceptions
        "-sDISABLE_EXCEPTION_CATCHING=0" "-sALLOW_MEMORY_GROWTH=1"
        "-sEXIT_RUNTIME=1" "-sENVIRONMENT=node")
endif()
