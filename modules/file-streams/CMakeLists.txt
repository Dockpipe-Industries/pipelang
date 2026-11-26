cmake_minimum_required(VERSION 3.21.1)
project(PipeLangFileStreams VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "PipeLangFileStreams 0.1.0 selects Linux x86_64 only")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
foreach(install_directory IN ITEMS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_DATADIR)
    if("${${install_directory}}" STREQUAL "" OR IS_ABSOLUTE "${${install_directory}}" OR
            "${${install_directory}}" MATCHES "(^|/)[.]([.])?(/|$)")
        message(FATAL_ERROR "${install_directory} must stay relative to the FileStreams install prefix")
    endif()
endforeach()

find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
find_package(PipeLangCryptoStream 0.1.0 EXACT CONFIG REQUIRED)
foreach(required IN ITEMS PIPELANG_CORE_MANIFEST_SHA256 PIPELANG_CRYPTO_STREAM_MANIFEST_SHA256)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Explicit ${required} required; no dependency acquisition is performed")
    endif()
endforeach()

if(NOT PipeLangModel_MANIFEST_SHA256 STREQUAL PIPELANG_CORE_MANIFEST_SHA256)
    message(FATAL_ERROR "Selected installed core differs from the explicit manifest pin")
endif()

get_filename_component(crypto_share "${PipeLangCryptoStream_MANIFEST}" DIRECTORY)
get_filename_component(crypto_share_parent "${crypto_share}" DIRECTORY)
get_filename_component(crypto_prefix "${crypto_share_parent}" DIRECTORY)
configure_file(cmake/CheckInstallPrefix.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/CheckInstallPrefix.cmake" @ONLY)
include("${CMAKE_CURRENT_BINARY_DIR}/CheckInstallPrefix.cmake")

add_library(pipelang_file_streams INTERFACE)
add_library(PipeLang::FileStreams ALIAS pipelang_file_streams)
set_target_properties(pipelang_file_streams PROPERTIES EXPORT_NAME FileStreams)
target_compile_features(pipelang_file_streams INTERFACE cxx_std_20)
target_include_directories(pipelang_file_streams INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# One existing dependency target owns its archive/provider globals. This package
# neither re-exports a second provider archive nor builds a replacement hash.
target_link_libraries(pipelang_file_streams INTERFACE PipeLang::CryptoStream)

set(package_share "${CMAKE_INSTALL_DATADIR}/pipelang-file-streams")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/library/library.json" file_library_sha256)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dependencies.json"
    "{\"protocol\":\"pipelang.file-streams-dependencies.v1\",\"coreManifestSha256\":\"${PIPELANG_CORE_MANIFEST_SHA256}\",\"cryptoStreamManifestSha256\":\"${PIPELANG_CRYPTO_STREAM_MANIFEST_SHA256}\"}\n")
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/CheckInstallPrefix.cmake")
install(TARGETS pipelang_file_streams EXPORT PipeLangFileStreamsTargets)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY library/ DESTINATION "${package_share}/library")
install(FILES README.md "${CMAKE_CURRENT_BINARY_DIR}/dependencies.json" DESTINATION "${package_share}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE" DESTINATION "${package_share}")
configure_package_config_file(cmake/PipeLangFileStreamsConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangFileStreamsConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PipeLangFileStreams")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/PipeLangFileStreamsConfigVersion.cmake"
    VERSION 0.1.0 COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/PipeLangFileStreamsConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangFileStreamsConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PipeLangFileStreams")
install(EXPORT PipeLangFileStreamsTargets NAMESPACE PipeLang::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PipeLangFileStreams")
configure_file(cmake/InstallManifest.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/InstallManifest.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallManifest.cmake")
