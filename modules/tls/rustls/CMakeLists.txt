cmake_minimum_required(VERSION 3.21.1)
project(PipeLangRustlsNative VERSION 0.1.0 LANGUAGES C)
find_package(PipeLangNativeDependencies 0.1.0 EXACT CONFIG REQUIRED)
file(SHA256 "${PIPELANG_RUSTLS_SOURCE}/Cargo.toml" manifest_hash)
pipelang_add_native_dependency(pipelang_rustls LANGUAGE Rust
    SOURCE_DIRECTORY "${PIPELANG_RUSTLS_SOURCE}"
    MANIFEST "${PIPELANG_RUSTLS_SOURCE}/Cargo.toml" MANIFEST_SHA256 "${manifest_hash}"
    LOCK_FILE "${PIPELANG_RUSTLS_SOURCE}/Cargo.lock"
    LOCK_SHA256 92ada0ac85b0ba00736211c2cbfec6599d426d4f05a396f1ec2597e26dbaa3df
    TOOLCHAIN_FILE "${PIPELANG_NATIVE_TOOLCHAIN}" TOOLCHAIN_SHA256 "${PIPELANG_NATIVE_TOOLCHAIN_SHA256}"
    CACHE_DIRECTORY "${PIPELANG_NATIVE_CACHE}" LIBRARY_NAME rustls_ffi PACKAGE rustls-ffi
    FEATURES aws-lc-rs SYSTEM_LIBRARIES gcc_s util rt pthread m dl
    RUSTFLAGS "-Cmetadata=rustls-ffi --remap-path-prefix=${PIPELANG_RUSTLS_SOURCE}=rustls-ffi --remap-path-prefix=${PIPELANG_NATIVE_CACHE}=cargo-registry")
add_executable(rustls-native-boundary tests/NativeBoundary.c)
target_include_directories(rustls-native-boundary PRIVATE "${PIPELANG_RUSTLS_SOURCE}/librustls/src")
target_link_libraries(rustls-native-boundary PRIVATE pipelang_rustls)
target_compile_features(rustls-native-boundary PRIVATE c_std_11)
target_compile_options(rustls-native-boundary PRIVATE -Wall -Wextra -Werror)
install(FILES "${pipelang_rustls_ARTIFACT}" DESTINATION lib)
install(FILES "${PIPELANG_RUSTLS_SOURCE}/librustls/src/rustls.h" DESTINATION include)
install(FILES "${PIPELANG_RUSTLS_SOURCE}/Cargo.lock" DESTINATION .)
install(FILES "${PIPELANG_RUSTLS_SOURCE}/LICENSE" "${PIPELANG_RUSTLS_SOURCE}/LICENSE-APACHE"
    "${PIPELANG_RUSTLS_SOURCE}/LICENSE-ISC" "${PIPELANG_RUSTLS_SOURCE}/LICENSE-MIT" DESTINATION licenses)
