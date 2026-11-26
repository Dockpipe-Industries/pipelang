cmake_minimum_required(VERSION 3.21.1)
project(PipeLangModel VERSION 0.1.0 LANGUAGES CXX)

include(cmake/NativeCompilerProfile.cmake)
include(cmake/PipeLangModelCompileResources.cmake)
set(PipeLangModel_CXX_RESOURCE_PROFILE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/compiler-profiles")
_pipelang_model_resolve_cxx_resource_profile(cxx_resource_profile cxx_resource_response
    cxx_resource_digest cxx_resource_dependency)
if(cxx_resource_profile STREQUAL "gnu-conservative-gc")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:@${cxx_resource_response}>")
endif()
message(STATUS "PipeLang CXX resource profile: ${cxx_resource_profile}")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)
get_filename_component(PIPELANG_MODEL_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/src/native/model-compiler"
    "${CMAKE_CURRENT_BINARY_DIR}/model-compiler")
add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/src/native/model-view-compiler"
    "${CMAKE_CURRENT_BINARY_DIR}/model-view-compiler")
add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/src/native/sdk-tools"
    "${CMAKE_CURRENT_BINARY_DIR}/sdk-tools")
add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/src/native/containedexec"
    "${CMAKE_CURRENT_BINARY_DIR}/containedexec")
add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/src/native/legacy-compiler"
    "${CMAKE_CURRENT_BINARY_DIR}/legacy-compiler")
add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/src/native/lexer"
    "${CMAKE_CURRENT_BINARY_DIR}/lexer")
add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/src/native/view-parser"
    "${CMAKE_CURRENT_BINARY_DIR}/view-parser")
add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/src/native/cpp-emitter"
    "${CMAKE_CURRENT_BINARY_DIR}/cpp-emitter")
add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/src/native/scalar-compiler"
    "${CMAKE_CURRENT_BINARY_DIR}/scalar-compiler")

add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/modules/json"
    "${CMAKE_CURRENT_BINARY_DIR}/json")

add_library(pipelang_model_runtime INTERFACE)
add_library(PipeLang::ModelRuntime ALIAS pipelang_model_runtime)
set_target_properties(pipelang_model_runtime PROPERTIES EXPORT_NAME ModelRuntime
    INTERFACE_PIPELANG_RUNTIME_LINKAGE HEADER_ONLY
    COMPATIBLE_INTERFACE_STRING "PIPELANG_RUNTIME_SDK;PIPELANG_RUNTIME_LINKAGE")
target_compile_features(pipelang_model_runtime INTERFACE cxx_std_20)
target_link_libraries(pipelang_model_runtime INTERFACE Threads::Threads)
if(PIPELANG_BUILD_NATIVE_OBJECT_PROOF AND
    ((CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|aarch64|arm64)$") OR
     (WIN32 AND MINGW AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|aarch64|arm64|ARM64)$") OR
     (CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND
      (CMAKE_OSX_ARCHITECTURES MATCHES "^(arm64|x86_64)$" OR
       (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|x86_64)$")))))
    add_library(pipelang_native_app_runtime STATIC
        native/src/native_app_runtime.cpp native/src/native_app_launcher.cpp
        native/src/native_host.cpp native/src/native_host_value.cpp native/src/native_composition.cpp
        native/src/NativeAtomics.cpp native/src/NativeBytes.cpp native/src/NativeConcurrentDictionaries.cpp
        native/src/NativeConcurrentSequences.cpp native/src/NativeDictionaries.cpp
        native/src/NativeFloating.cpp native/src/NativeParallel.cpp native/src/NativeReceiverStorage.cpp
        native/src/NativeSequences.cpp)
    target_link_libraries(pipelang_native_app_runtime PRIVATE PipeLang::ModelRuntime)
    set_target_properties(pipelang_native_app_runtime PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_options(pipelang_native_app_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
    if(APPLE)
        target_compile_options(pipelang_native_app_runtime PRIVATE -mmacosx-version-min=11.0)
    endif()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native-app-runtime-compiler"
        CONTENT "${CMAKE_CXX_COMPILER}\n")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native-app-runtime-compiler-id"
        CONTENT "${CMAKE_CXX_COMPILER_ID}\n")
endif()
target_include_directories(pipelang_model_runtime INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/native/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

install(TARGETS pipelang_model_runtime EXPORT PipeLangModelTargets)
install(TARGETS pipelang-modelc pipelang-model-viewc pipelang-sdk-tool pipelang-contained pipelang-native pipelang
    pipelang-cpp-lexer pipelang-cpp-view-parser pipelang-cpp-emitter
    pipelang-native-cpp
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
set(model_native_application_directory "${CMAKE_INSTALL_DATADIR}/pipelang-model/native-application")
set(model_installed_object_compiler "")
set(model_installed_application_runtime "")
set(model_native_compiler_relative "")
if(TARGET pipelang-objectc)
    install(TARGETS pipelang-objectc RUNTIME DESTINATION "${model_native_application_directory}")
    set(model_installed_object_compiler "${model_native_application_directory}/pipelang-objectc${CMAKE_EXECUTABLE_SUFFIX}")
    file(RELATIVE_PATH model_native_compiler_relative "${CMAKE_INSTALL_FULL_BINDIR}"
        "${CMAKE_INSTALL_FULL_DATADIR}/pipelang-model/native-application/pipelang-objectc${CMAKE_EXECUTABLE_SUFFIX}")
endif()
target_compile_definitions(pipelang PRIVATE PIPELANG_NATIVE_BUILD_COMMAND
    "PIPELANG_NATIVE_OBJECT_COMPILER_RELATIVE_PATH=\"${model_native_compiler_relative}\"")
if(TARGET pipelang_native_app_runtime)
    install(TARGETS pipelang_native_app_runtime ARCHIVE DESTINATION "${model_native_application_directory}")
    set(model_installed_application_runtime
        "${model_native_application_directory}/${CMAKE_STATIC_LIBRARY_PREFIX}pipelang_native_app_runtime${CMAKE_STATIC_LIBRARY_SUFFIX}")
endif()
install(DIRECTORY native/include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES native/src/task_scope.cpp native/src/object_domain.cpp
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/native-runtime"
    COMPONENT PipeLangNativeRuntimeSources)
install(FILES compiler-profiles/gnu-conservative-gc.rsp
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/compiler-profiles"
    COMPONENT PipeLangCompilerProfiles)
install(DIRECTORY "${PIPELANG_MODEL_SOURCE_ROOT}/sdk/libraries/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/libraries"
    COMPONENT PipeLangSourceLibraries)
install(FILES "${PIPELANG_MODEL_SOURCE_ROOT}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/licenses" RENAME LICENSE)
if(EXISTS "${PIPELANG_MODEL_SOURCE_ROOT}/NOTICE")
    install(FILES "${PIPELANG_MODEL_SOURCE_ROOT}/NOTICE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/licenses")
endif()
install(FILES "${PIPELANG_MODEL_SOURCE_ROOT}/src/native/third_party/nlohmann/LICENSE.MIT"
    "${PIPELANG_MODEL_SOURCE_ROOT}/src/native/third_party/nlohmann/PROVENANCE.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/licenses/nlohmann-json")
install(FILES "${PIPELANG_MODEL_SOURCE_ROOT}/src/native/third_party/yaml-cpp/LICENSE"
    "${PIPELANG_MODEL_SOURCE_ROOT}/src/native/third_party/yaml-cpp/PROVENANCE.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/licenses/yaml-cpp")
install(FILES "${PIPELANG_MODEL_SOURCE_ROOT}/src/native/third_party/yaml-cpp/src/contrib/dragonbox.h"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/licenses/yaml-cpp")

if(PIPELANG_NATIVE_COMPILER_PROFILE STREQUAL "windows-clang-mingw")
    install(DIRECTORY "${PIPELANG_MODEL_SOURCE_ROOT}/src/native/third_party/windows-runtime/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model/licenses/windows-runtime")
endif()

set(model_cmake_directory "${CMAKE_INSTALL_LIBDIR}/cmake/PipeLangModel")
configure_package_config_file(cmake/PipeLangModelConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelConfig.cmake"
    INSTALL_DESTINATION "${model_cmake_directory}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion ARCH_INDEPENDENT)
install(EXPORT PipeLangModelTargets NAMESPACE PipeLang:: DESTINATION "${model_cmake_directory}")
configure_file(cmake/PipeLangModelBindings.cmake
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelBindings.cmake" @ONLY)
configure_file(cmake/PipeLangModelRuntime.cmake
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelRuntime.cmake" COPYONLY)
configure_file(cmake/PipeLangModelCompileResources.cmake
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelCompileResources.cmake" COPYONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelConfigVersion.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelBindings.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelRuntime.cmake" DESTINATION "${model_cmake_directory}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/PipeLangModelCompileResources.cmake"
    DESTINATION "${model_cmake_directory}"
    COMPONENT PipeLangCompilerProfiles)
install(FILES cmake/PipeLangNativeExecutable.cmake
    DESTINATION "${model_cmake_directory}"
    COMPONENT PipeLangNativeExecutable)

configure_file(cmake/NativeToolchain.json.in "${CMAKE_CURRENT_BINARY_DIR}/native-toolchain.json" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/native-toolchain.json"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pipelang-model")

add_subdirectory("${PIPELANG_MODEL_SOURCE_ROOT}/sdk/native-dependencies"
    "${CMAKE_CURRENT_BINARY_DIR}/native-dependencies")

# Run last so the receipt covers the actual installed binaries and configuration.
configure_file(cmake/InstallModelManifest.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/InstallModelManifest.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallModelManifest.cmake")

pipelang_native_finalize("${CMAKE_CURRENT_SOURCE_DIR}")
