cmake_minimum_required(VERSION 3.21.1)
project(PipeLangCryptoStream VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "PipeLangCryptoStream 0.1.0 selects the Linux x86_64 maintained provider only")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
find_package(Threads REQUIRED)

configure_file(cmake/CheckInstallPrefix.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/CheckInstallPrefix.cmake" @ONLY)
include("${CMAKE_CURRENT_BINARY_DIR}/CheckInstallPrefix.cmake")

foreach(required IN ITEMS PIPELANG_CORE_MANIFEST_SHA256 PIPELANG_CRYPTO_PROVIDER_PREFIX
        PIPELANG_CRYPTO_PROVIDER_LIBRARY_SHA256 PIPELANG_CRYPTO_PROVIDER_HEADER_SHA256
        PIPELANG_CRYPTO_PROVIDER_LOCK_SHA256 PIPELANG_CRYPTO_PROVIDER_RECEIPT
        PIPELANG_CRYPTO_PROVIDER_RECEIPT_SHA256)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Explicit ${required} is required; no dependency acquisition is performed")
    endif()
endforeach()

foreach(required_path IN ITEMS PIPELANG_CRYPTO_PROVIDER_PREFIX PIPELANG_CRYPTO_PROVIDER_RECEIPT)
    if(NOT IS_ABSOLUTE "${${required_path}}")
        message(FATAL_ERROR "${required_path} must select an explicit absolute path")
    endif()
endforeach()

if(NOT PipeLangModel_MANIFEST_SHA256 STREQUAL PIPELANG_CORE_MANIFEST_SHA256)
    message(FATAL_ERROR "Selected exact core SDK manifest differs from its explicit pin")
endif()

execute_process(COMMAND "${PipeLangModel_GENERATOR}" verify
    --prefix "${PipeLangModel_SDK_PREFIX}" --manifest "${PipeLangModel_MANIFEST}"
    --sha256 "${PIPELANG_CORE_MANIFEST_SHA256}" RESULT_VARIABLE core_status)
if(NOT core_status EQUAL 0)
    message(FATAL_ERROR "Selected exact installed core SDK failed content verification")
endif()

set(provider_library "${PIPELANG_CRYPTO_PROVIDER_PREFIX}/lib/libpipelang_crypto.a")
set(provider_header "${PIPELANG_CRYPTO_PROVIDER_PREFIX}/include/pipelang_crypto_stream.h")
set(provider_lock "${PIPELANG_CRYPTO_PROVIDER_PREFIX}/Cargo.lock")
foreach(kind IN ITEMS LIBRARY HEADER LOCK RECEIPT)
    string(TOLOWER "${kind}" lower_kind)
    if(kind STREQUAL "RECEIPT")
        set(selected_file "${PIPELANG_CRYPTO_PROVIDER_RECEIPT}")
    else()
        set(selected_file "${provider_${lower_kind}}")
    endif()

    file(SHA256 "${selected_file}" selected_sha256)
    if(NOT selected_sha256 STREQUAL PIPELANG_CRYPTO_PROVIDER_${kind}_SHA256)
        message(FATAL_ERROR "Exact selected maintained provider ${kind} mismatch")
    endif()
endforeach()

file(READ "${PIPELANG_CRYPTO_PROVIDER_RECEIPT}" provider_receipt)
string(JSON receipt_artifact GET "${provider_receipt}" artifact_sha256)
string(JSON receipt_lock GET "${provider_receipt}" lock_sha256)
string(JSON receipt_target GET "${provider_receipt}" target)
if(NOT receipt_artifact STREQUAL PIPELANG_CRYPTO_PROVIDER_LIBRARY_SHA256 OR
        NOT receipt_lock STREQUAL PIPELANG_CRYPTO_PROVIDER_LOCK_SHA256 OR
        NOT receipt_target STREQUAL "x86_64-unknown-linux-gnu")
    message(FATAL_ERROR "Maintained provider receipt disagrees with selected artifact, lock or platform")
endif()

add_library(PipeLangCryptoStreamProvider STATIC IMPORTED GLOBAL)
set_target_properties(PipeLangCryptoStreamProvider PROPERTIES IMPORTED_LOCATION "${provider_library}")
add_library(pipelang_crypto_stream INTERFACE)
add_library(PipeLang::CryptoStream ALIAS pipelang_crypto_stream)
set_target_properties(pipelang_crypto_stream PROPERTIES EXPORT_NAME CryptoStream)
target_compile_features(pipelang_crypto_stream INTERFACE cxx_std_20)
target_include_directories(pipelang_crypto_stream INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${PIPELANG_CRYPTO_PROVIDER_PREFIX}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(pipelang_crypto_stream INTERFACE PipeLang::ModelRuntime
    PipeLangCryptoStreamProvider Threads::Threads ${CMAKE_DL_LIBS} m)

set(package_share "${CMAKE_INSTALL_DATADIR}/pipelang-crypto-stream")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/library/library.json" crypto_library_sha256)
# Recheck the actual install invocation's prefix before the first payload write,
# including --prefix overrides and an effective DESTDIR destination.
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/CheckInstallPrefix.cmake")
install(TARGETS pipelang_crypto_stream EXPORT PipeLangCryptoStreamTargets)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${provider_header}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${provider_library}" DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RENAME libpipelang_crypto_stream_provider.a)
install(DIRECTORY library/ DESTINATION "${package_share}/library")
install(FILES "${provider_lock}" DESTINATION "${package_share}/provider")
install(FILES "${PIPELANG_CRYPTO_PROVIDER_RECEIPT}" DESTINATION "${package_share}/provider"
    RENAME build-receipt.json)
install(FILES README.md DESTINATION "${package_share}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE" DESTINATION "${package_share}")

configure_package_config_file(cmake/PipeLangCryptoStreamConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangCryptoStreamConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PipeLangCryptoStream")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/PipeLangCryptoStreamConfigVersion.cmake"
    VERSION 0.1.0 COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/PipeLangCryptoStreamConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangCryptoStreamConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PipeLangCryptoStream")
install(EXPORT PipeLangCryptoStreamTargets NAMESPACE PipeLang::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PipeLangCryptoStream")

configure_file(cmake/InstallManifest.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/InstallManifest.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/InstallManifest.cmake")
