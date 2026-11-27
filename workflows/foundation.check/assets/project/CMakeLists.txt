cmake_minimum_required(VERSION 3.22)
project(PipeLangFoundationProject LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
if(NOT PipeLangModel_MANIFEST_SHA256 STREQUAL FOUNDATION_SDK_SHA256)
    message(FATAL_ERROR "Foundation project SDK manifest changed")
endif()
file(READ "${FOUNDATION_FIXTURE_SOURCE}" fixture)
string(REPLACE "public Class ByteBufferIteration {"
    "module Foundation.Iteration;\npublic model Class ByteBufferIteration {\n    public ByteBufferIteration() {}"
    fixture "${fixture}")
set(project_root "${CMAKE_CURRENT_BINARY_DIR}/project")
file(MAKE_DIRECTORY "${project_root}/src")
file(WRITE "${project_root}/src/ByteBufferIteration.pipe" "${fixture}")
file(WRITE "${project_root}/src/Startup.pipe"
    "module Foundation.Composition;\nimport Foundation.Iteration;\npublic composition Startup {\n    scoped ByteBufferIteration = ByteBufferIteration;\n    export ByteBufferIteration;\n}\n")
pipelang_add_model(foundation_iteration_model
    PROJECT_ROOT "${project_root}" SOURCE_ROOT src ENTRY Startup.pipe
    NAMESPACE foundation_iteration COMPOSITION_FACTORY
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/foundation_iteration")
add_executable(foundation-project main.cpp)
target_include_directories(foundation-project PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(foundation-project PRIVATE foundation_iteration_model)
target_compile_features(foundation-project PRIVATE cxx_std_20)
target_compile_options(foundation-project PRIVATE -Wall -Wextra -Werror)
if(FOUNDATION_SANITIZERS)
    target_compile_options(foundation-project PRIVATE -O1 -g1 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(foundation-project PRIVATE -fsanitize=address,undefined)
endif()
if(EMSCRIPTEN)
    target_compile_options(foundation-project PRIVATE -O1 -fexceptions)
    target_link_options(foundation-project PRIVATE -O1 -fexceptions
        "-sDISABLE_EXCEPTION_CATCHING=0" "-sALLOW_MEMORY_GROWTH=1"
        "-sEXIT_RUNTIME=1" "-sENVIRONMENT=node")
endif()
