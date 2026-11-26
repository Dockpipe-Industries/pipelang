cmake_minimum_required(VERSION 3.21.1)
project(PipeLangNativeDependencies VERSION 0.1.0 LANGUAGES NONE)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/PipeLangNativeDependenciesConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangNativeDependenciesConfig.cmake"
    INSTALL_DESTINATION lib/cmake/PipeLangNativeDependencies)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/PipeLangNativeDependenciesConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/PipeLangNativeDependenciesConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PipeLangNativeDependenciesConfigVersion.cmake"
    cmake/NativeDependency.cmake cmake/RunDependency.cmake cmake/VerifyInputs.cmake
    cmake/Rust.cmake cmake/Go.cmake cmake/CSharp.cmake cmake/CMake.cmake
    DESTINATION lib/cmake/PipeLangNativeDependencies)
install(FILES README.md DESTINATION share/pipelang-native-dependencies)
