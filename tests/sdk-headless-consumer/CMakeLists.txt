cmake_minimum_required(VERSION 3.21.1)
project(PipeLangHeadlessConsumer LANGUAGES CXX)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
pipelang_add_model(contact-model
    MODEL "${CMAKE_CURRENT_SOURCE_DIR}/ContactModel.pipe"
    NAMESPACE headless_fixture
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_executable(pipelang-headless-consumer main.cpp)
target_link_libraries(pipelang-headless-consumer PRIVATE contact-model)

# The native constexpr API must compile without ModelRuntime or SDK include paths.
get_target_property(constants_header contact-model PIPELANG_CONSTANTS_HEADER)
get_filename_component(constants_directory "${constants_header}" DIRECTORY)
add_executable(pipelang-constants-leaf-consumer constants.cpp)
add_dependencies(pipelang-constants-leaf-consumer contact-model_generate)
target_compile_features(pipelang-constants-leaf-consumer PRIVATE cxx_std_20)
target_include_directories(pipelang-constants-leaf-consumer PRIVATE "${constants_directory}")
