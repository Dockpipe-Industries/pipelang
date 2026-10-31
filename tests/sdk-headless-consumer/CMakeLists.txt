cmake_minimum_required(VERSION 3.21.1)
project(PipeLangHeadlessConsumer LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_model(contact-model
    MODEL "${CMAKE_CURRENT_SOURCE_DIR}/ContactModel.pipe"
    NAMESPACE headless_fixture
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_executable(pipelang-headless-consumer main.cpp)
target_link_libraries(pipelang-headless-consumer PRIVATE contact-model)
