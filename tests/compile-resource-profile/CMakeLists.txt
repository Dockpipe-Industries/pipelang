cmake_minimum_required(VERSION 3.21.1)
project(PipeLangCompileResourceContracts LANGUAGES CXX)

find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
set(actual_compiler "${CMAKE_CXX_COMPILER_ID}")
if(DEFINED PIPELANG_PROFILE_TEST_COMPILER_ID)
    # Configure-only guard witness. This is never a toolchain acceptance claim.
    set(CMAKE_CXX_COMPILER_ID "${PIPELANG_PROFILE_TEST_COMPILER_ID}")
endif()

set(header_batch_option)
if(PIPELANG_PROFILE_TEST_BATCH_HEADER)
    set(header_batch_option BATCH_IMPLEMENTATIONS)
endif()
set(compiled_batch_option BATCH_IMPLEMENTATIONS)
if(PIPELANG_PROFILE_TEST_PARTITIONED)
    set(compiled_batch_option)
endif()
pipelang_add_model(header_model ${header_batch_option}
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    ENTRY Entry.pipe NAMESPACE profile_header
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/header")
pipelang_add_model(compiled_model ${compiled_batch_option}
    PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
    ENTRY Entry.pipe NAMESPACE profile_compiled COMPILATION COMPILED
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/compiled")
get_target_property(implementation_target compiled_model PIPELANG_IMPLEMENTATION_TARGET)
if(PIPELANG_PROFILE_TEST_DISABLE_BATCH)
    set_property(TARGET "${implementation_target}" PROPERTY UNITY_BUILD OFF)
endif()
get_target_property(batched "${implementation_target}" UNITY_BUILD)
get_target_property(batch_size "${implementation_target}" UNITY_BUILD_BATCH_SIZE)
if(PIPELANG_PROFILE_TEST_PARTITIONED OR PIPELANG_PROFILE_TEST_DISABLE_BATCH)
    if(batched)
        message(FATAL_ERROR "Partitioned or disabled strategy unexpectedly enabled batching")
    endif()
elseif(NOT batched OR NOT batch_size EQUAL 2)
    message(FATAL_ERROR "Explicit paired strategy did not select bounded private batches")
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/implementation-sources.txt"
    CONTENT "$<JOIN:$<FILTER:$<TARGET_PROPERTY:${implementation_target},SOURCES>,INCLUDE,Implementation_[0-9]+[.]cpp$>,\n>\n"
    NEWLINE_STYLE UNIX)

add_executable(header_consumer main.cpp)
target_compile_definitions(header_consumer PRIVATE PROFILE_HEADER=1)
target_link_libraries(header_consumer PRIVATE header_model)
add_executable(compiled_consumer main.cpp)
target_link_libraries(compiled_consumer PRIVATE compiled_model)

if(WIN32)
    # Match the installed SDK consumer profile. CTest runs without toolchain
    # runtime DLLs on PATH, just like the other Windows SDK fixtures.
    target_link_options(header_consumer PRIVATE -static)
    target_link_options(compiled_consumer PRIVATE -static)
endif()

add_library(native_owner STATIC owner.cpp)
target_link_libraries(native_owner PRIVATE "$<BUILD_INTERFACE:compiled_model>")
pipelang_install_model_implementation(compiled_model
    OWNER native_owner EXPORT ProfileOwnerTargets ARCHIVE_DESTINATION lib)
install(TARGETS native_owner EXPORT ProfileOwnerTargets ARCHIVE DESTINATION lib)
install(EXPORT ProfileOwnerTargets NAMESPACE ProfileOwner:: DESTINATION lib/cmake/ProfileOwner)

get_target_property(profile header_model PIPELANG_CXX_RESOURCE_PROFILE)
get_target_property(response header_model PIPELANG_CXX_RESOURCE_RESPONSE_FILE)
get_target_property(digest header_model PIPELANG_CXX_RESOURCE_PROFILE_SHA256)
get_target_property(compiled_response compiled_model PIPELANG_CXX_RESOURCE_RESPONSE_FILE)
if(NOT response STREQUAL compiled_response)
    message(FATAL_ERROR "Header and compiled facades selected different resource policies")
endif()
get_target_property(runtime_options PipeLang::ModelRuntime INTERFACE_COMPILE_OPTIONS)
if(runtime_options MATCHES "compiler-profiles|conservative-gc")
    message(FATAL_ERROR "Compile resource policy escaped into ModelRuntime defaults")
endif()
file(TO_CMAKE_PATH "${response}" response_json)
string(REPLACE "\"" "\\\"" response_json "${response_json}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/profile-policy.json"
    "{\"compiler\":\"${actual_compiler}\",\"profile\":\"${profile}\",\"response\":\"${response_json}\",\"sha256\":\"${digest}\"}\n")

enable_testing()
add_test(NAME header_consumer COMMAND header_consumer)
add_test(NAME compiled_consumer COMMAND compiled_consumer)
