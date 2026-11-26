cmake_minimum_required(VERSION 3.21.1)
project(PipeLangCryptoNative VERSION 0.1.0 LANGUAGES CXX)
find_package(PipeLangNativeDependencies 0.1.0 EXACT CONFIG REQUIRED)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml" manifest_hash)
pipelang_add_native_dependency(pipelang_crypto LANGUAGE Rust
    SOURCE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml" MANIFEST_SHA256 "${manifest_hash}"
    LOCK_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.lock"
    LOCK_SHA256 537415a296be488edf2a780f1eb05d22dc7b76535b5dcefff231b7b65b63f3e8
    TOOLCHAIN_FILE "${PIPELANG_NATIVE_TOOLCHAIN}" TOOLCHAIN_SHA256 "${PIPELANG_NATIVE_TOOLCHAIN_SHA256}"
    CACHE_DIRECTORY "${PIPELANG_NATIVE_CACHE}" LIBRARY_NAME pipelang_crypto PACKAGE pipelang-crypto-native
    DEFAULT_FEATURES TESTS SYSTEM_LIBRARIES dl pthread m)
add_executable(crypto-native-boundary tests/NativeBoundary.cpp)
target_include_directories(crypto-native-boundary PRIVATE include)
target_link_libraries(crypto-native-boundary PRIVATE pipelang_crypto)
target_compile_features(crypto-native-boundary PRIVATE cxx_std_20)
target_compile_options(crypto-native-boundary PRIVATE -O1 -g0 -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(crypto-native-boundary PRIVATE -fsanitize=address,undefined)
install(FILES "${pipelang_crypto_ARTIFACT}" DESTINATION lib)
install(FILES include/pipelang_crypto.h include/pipelang_crypto_stream.h DESTINATION include)
install(FILES Cargo.lock DESTINATION .)
