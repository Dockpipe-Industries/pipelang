cmake_minimum_required(VERSION 3.21.1)
project(PipeLangModel VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
get_filename_component(PIPELANG_MODEL_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/src/native/model-compiler"
    "${CMAKE_CURRENT_BINARY_DIR}/model-compiler")
add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/src/native/model-view-compiler"
    "${CMAKE_CURRENT_BINARY_DIR}/model-view-compiler")

add_library(pipelang_model_runtime INTERFACE)
add_library(PipeLang::ModelRuntime ALIAS pipelang_model_runtime)
set_target_properties(pipelang_model_runtime PROPERTIES EXPORT_NAME ModelRuntime)
target_compile_features(pipelang_model_runtime INTERFACE cxx_std_20)
target_link_libraries(pipelang_model_runtime INTERFACE Threads::Threads)
target_include_directories(pipelang_model_runtime INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/native/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

install(TARGETS pipelang_model_runtime EXPORT PipeLangModelTargets)
install(TARGETS pipelang-modelc pipelang-model-viewc
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY native/include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${PIPELANG_MODEL_SOURCE_ROOT}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/licenses" RENAME LICENSE)
if(EXISTS "${PIPELANG_MODEL_SOURCE_ROOT}/NOTICE")
    install(FILES "${PIPELANG_MODEL_SOURCE_ROOT}/NOTICE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/licenses")
endif()
install(FILES "${PIPELANG_MODEL_SOURCE_ROOT}/src/native/third_party/nlohmann/LICENSE.MIT"
    "${PIPELANG_MODEL_SOURCE_ROOT}/src/native/third_party/nlohmann/PROVENANCE.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/licenses/nlohmann-json")
install(FILES tooling/generate_bindings.py tooling/sdk_manifest.py
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/tooling")

set(model_cmake_directory "${CMAKE_INSTALL_LIBDIR}/cmake/PipeLangModel")
configure_package_config_file(cmake/PipeLangModelConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelConfig.cmake"
    INSTALL_DESTINATION "${model_cmake_directory}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
install(EXPORT PipeLangModelTargets NAMESPACE PipeLang:: DESTINATION "${model_cmake_directory}")
configure_file(cmake/PipeLangModelBindings.cmake
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelBindings.cmake" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelConfigVersion.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelBindings.cmake" DESTINATION "${model_cmake_directory}")

# Run last so the receipt covers the actual installed binaries and configuration.
configure_file(cmake/InstallModelManifest.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/InstallModelManifest.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallModelManifest.cmake")
