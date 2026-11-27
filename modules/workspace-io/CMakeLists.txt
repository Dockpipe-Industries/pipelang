cmake_minimum_required(VERSION 3.21.1)
project(PipeLangWorkspaceIo VERSION 0.1.0 LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "PipeLangWorkspaceIo 0.1.0 supports Linux only")
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
find_package(Threads REQUIRED)
foreach(required PIPELANG_WORKSPACE_CRYPTO_PREFIX PIPELANG_WORKSPACE_CRYPTO_HEADER_SHA256 PIPELANG_WORKSPACE_CRYPTO_LIBRARY_SHA256)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Explicit ${required} is required")
    endif()
endforeach()
set(crypto_header "${PIPELANG_WORKSPACE_CRYPTO_PREFIX}/include/pipelang_crypto.h")
set(crypto_library "${PIPELANG_WORKSPACE_CRYPTO_PREFIX}/lib/libpipelang_crypto.a")
file(SHA256 "${crypto_header}" actual_header)
file(SHA256 "${crypto_library}" actual_library)
if(NOT actual_header STREQUAL PIPELANG_WORKSPACE_CRYPTO_HEADER_SHA256 OR
   NOT actual_library STREQUAL PIPELANG_WORKSPACE_CRYPTO_LIBRARY_SHA256)
    message(FATAL_ERROR "PipeLang crypto capability digest mismatch")
endif()
add_library(PipeLangWorkspaceIoCrypto STATIC IMPORTED GLOBAL)
set_target_properties(PipeLangWorkspaceIoCrypto PROPERTIES IMPORTED_LOCATION "${crypto_library}")
add_library(pipelang_workspace_io STATIC
    src/primitives/Hashing.cpp
    src/primitives/Uuid.cpp
    src/primitives/WorkspacePath.cpp
    src/primitives/CooperativeLock.cpp
    src/files/FileIo.cpp
    src/files/DirectoryPager.cpp
    src/repositories/RepositoryInspector.cpp
    src/changes/ChangeFeed.cpp)
add_library(PipeLang::WorkspaceIo ALIAS pipelang_workspace_io)
set_target_properties(pipelang_workspace_io PROPERTIES EXPORT_NAME WorkspaceIo POSITION_INDEPENDENT_CODE ON)
target_compile_features(pipelang_workspace_io PUBLIC cxx_std_20)
target_include_directories(pipelang_workspace_io
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${PIPELANG_WORKSPACE_CRYPTO_PREFIX}/include"
    include/pipelang/workspace/files include/pipelang/workspace/primitives
    include/pipelang/workspace/repositories include/pipelang/workspace/changes)
target_link_libraries(pipelang_workspace_io PUBLIC PipeLang::ModelRuntime
    PRIVATE PipeLangWorkspaceIoCrypto Threads::Threads ${CMAKE_DL_LIBS} m)
target_compile_options(pipelang_workspace_io PRIVATE -Wall -Wextra -Werror)
option(PIPELANG_WORKSPACE_IO_SANITIZE "Instrument the C++ module and fixture with ASan/UBSan" OFF)
if(PIPELANG_WORKSPACE_IO_SANITIZE)
    target_compile_options(pipelang_workspace_io PUBLIC -O1 -g0 -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(pipelang_workspace_io PUBLIC -fsanitize=address,undefined)
endif()
option(PIPELANG_WORKSPACE_IO_TESTS "Build bounded native capability tests" ON)
if(PIPELANG_WORKSPACE_IO_TESTS)
    enable_testing()
    add_executable(workspace-io-contract tests/WorkspaceIoContract.cpp)
    target_link_libraries(workspace-io-contract PRIVATE PipeLang::WorkspaceIo)
    target_compile_options(workspace-io-contract PRIVATE -Wall -Wextra -Werror)
    add_test(NAME workspace-io-contract COMMAND workspace-io-contract)
    set_tests_properties(workspace-io-contract PROPERTIES TIMEOUT 30)
endif()
install(TARGETS pipelang_workspace_io EXPORT PipeLangWorkspaceIoTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES LICENSE README.md dependencies.lock.json DESTINATION share/pipelang-workspace-io)
install(FILES "${crypto_library}" DESTINATION "${CMAKE_INSTALL_LIBDIR}" RENAME libpipelang_workspace_crypto.a)
install(FILES "${crypto_header}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pipelang/workspace/crypto")
install(FILES "${PIPELANG_WORKSPACE_CRYPTO_PREFIX}/build-receipt.json" DESTINATION share/pipelang-workspace-io/crypto)
install(FILES "${PIPELANG_WORKSPACE_CRYPTO_PREFIX}/Cargo.lock" "${PIPELANG_WORKSPACE_CRYPTO_PREFIX}/LICENSE" DESTINATION share/pipelang-workspace-io/crypto)
install(DIRECTORY "${PIPELANG_WORKSPACE_CRYPTO_PREFIX}/licenses/" DESTINATION share/pipelang-workspace-io/crypto/licenses)
configure_package_config_file(cmake/PipeLangWorkspaceIoConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/PipeLangWorkspaceIoConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PipeLangWorkspaceIo")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/PipeLangWorkspaceIoConfigVersion.cmake" VERSION 0.1.0 COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/PipeLangWorkspaceIoConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/PipeLangWorkspaceIoConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PipeLangWorkspaceIo")
install(EXPORT PipeLangWorkspaceIoTargets NAMESPACE PipeLang:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/PipeLangWorkspaceIo")
