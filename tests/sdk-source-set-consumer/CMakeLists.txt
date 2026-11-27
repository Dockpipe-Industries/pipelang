cmake_minimum_required(VERSION 3.21.1)
project(PipeLangSourceSetConsumer LANGUAGES CXX)

find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_model(source-set-models
    SOURCE_SET "${CMAKE_CURRENT_SOURCE_DIR}/models/game.sources.json"
    NAMESPACE source_set_fixture
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
pipelang_add_model(source-set-values
    SOURCE_SET "${CMAKE_CURRENT_SOURCE_DIR}/models/values.sources.json"
    NAMESPACE source_set_values
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/value-generated")
add_executable(pipelang-source-set-consumer main.cpp)
target_include_directories(pipelang-source-set-consumer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(pipelang-source-set-consumer PRIVATE source-set-models source-set-values)
