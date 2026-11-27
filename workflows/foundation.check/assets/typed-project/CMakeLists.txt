cmake_minimum_required(VERSION 3.22)
project(PipeLangTypedResultProject LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
if(NOT PipeLangModel_MANIFEST_SHA256 STREQUAL FOUNDATION_SDK_SHA256)
    message(FATAL_ERROR "Typed Result project SDK manifest changed")
endif()
file(READ "${FOUNDATION_FIXTURE_SOURCE}" fixture)
set(project_root "${CMAKE_CURRENT_BINARY_DIR}/project")
file(MAKE_DIRECTORY "${project_root}/src")
file(WRITE "${project_root}/src/TypedHostErrors.pipe" "module Errors;\n${fixture}")
file(WRITE "${project_root}/src/Startup.pipe"
    "module Composition;\nimport Errors;\npublic composition Startup {\n    scoped input TypedHost;\n    scoped TypedHostErrors = TypedHostErrors;\n    export TypedHostErrors;\n}\n")
pipelang_add_model(typed_result_model
    PROJECT_ROOT "${project_root}" SOURCE_ROOT src ENTRY Startup.pipe
    NAMESPACE typed_host_project COMPOSITION_FACTORY
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/typed_result")
add_executable(typed-result-project main.cpp)
target_include_directories(typed-result-project PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(typed-result-project PRIVATE typed_result_model)
target_compile_features(typed-result-project PRIVATE cxx_std_20)
target_compile_options(typed-result-project PRIVATE -Wall -Wextra -Werror)
if(FOUNDATION_SANITIZERS)
    target_compile_options(typed-result-project PRIVATE -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(typed-result-project PRIVATE -fsanitize=address,undefined)
endif()
if(EMSCRIPTEN)
    target_compile_options(typed-result-project PRIVATE -O1 -fexceptions)
    target_link_options(typed-result-project PRIVATE -O1 -fexceptions
        "-sDISABLE_EXCEPTION_CATCHING=0" "-sALLOW_MEMORY_GROWTH=1"
        "-sEXIT_RUNTIME=1" "-sENVIRONMENT=node")
endif()
