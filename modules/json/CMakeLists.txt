# General strict JSON effect; independent of compilers and UI adapters.
set(json_header "${PIPELANG_MODEL_SOURCE_ROOT}/src/native/third_party/nlohmann/json.hpp")
file(SHA256 "${json_header}" json_header_sha256)
if(NOT json_header_sha256 STREQUAL "aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63")
    message(FATAL_ERROR "Pinned nlohmann/json.hpp checksum mismatch")
endif()

add_library(pipelang_json_reader STATIC src/Reader.cpp src/DocumentWriter.cpp)
add_library(PipeLang::JsonReader ALIAS pipelang_json_reader)
set_target_properties(pipelang_json_reader PROPERTIES
    EXPORT_NAME JsonReader
    POSITION_INDEPENDENT_CODE ON)
target_compile_features(pipelang_json_reader PUBLIC cxx_std_20)
target_include_directories(pipelang_json_reader
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
           "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${PIPELANG_MODEL_SOURCE_ROOT}/src/native/third_party")
install(TARGETS pipelang_json_reader EXPORT PipeLangModelTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# Versioned native effect sources permit full consumer instrumentation without
# consulting a source checkout. Existing manifest roots bind this entire closure.
set(json_sources "${CMAKE_INSTALL_DATADIR}/pipelang-model/native/json")
install(FILES src/Reader.cpp src/ValueRenderer.hpp src/DocumentWriter.cpp src/UtfInput.hpp DESTINATION "${json_sources}" COMPONENT PipeLangJsonReaderSources)
install(FILES "${json_header}" DESTINATION "${json_sources}/third_party/nlohmann"
    COMPONENT PipeLangJsonReaderSources)
