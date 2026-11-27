cmake_minimum_required(VERSION 3.21.1)
project(PipeLangWorkflowLinkedRuntime LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The native workflow linked runtime is qualified only on Linux")
endif()
if(NOT RUNTIME_MODE MATCHES "^(normal|sanitizer)$")
    message(FATAL_ERROR "Select normal or sanitizer runtime instrumentation")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "" OR NOT CMAKE_CXX_FLAGS STREQUAL "")
    message(FATAL_ERROR "Runtime policy requires empty global/build-type compiler flags")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(PipeLangModel CONFIG REQUIRED)

add_library(workflow_runtime_policy INTERFACE)
target_compile_options(workflow_runtime_policy INTERFACE -pthread -Wall -Wextra -Werror)
if(RUNTIME_MODE STREQUAL "normal")
    target_compile_options(workflow_runtime_policy INTERFACE -O2)
else()
    target_compile_options(workflow_runtime_policy INTERFACE
        -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(workflow_runtime_policy INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

pipelang_add_linked_runtime(workflow_runtime POLICY_TARGET workflow_runtime_policy)
set_target_properties(workflow_runtime PROPERTIES
    OUTPUT_NAME pipelang-model-runtime
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/archive")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/archive-path.txt"
    CONTENT "$<TARGET_FILE:workflow_runtime>\n")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/runtime-policy.txt" CONTENT
"mode=${RUNTIME_MODE}
sdk_manifest_sha256=${PipeLangModel_MANIFEST_SHA256}
compiler=${CMAKE_CXX_COMPILER}
compiler_id=${CMAKE_CXX_COMPILER_ID}
compiler_version=${CMAKE_CXX_COMPILER_VERSION}
cxx_standard=20
cxx_extensions=OFF
build_type=${CMAKE_BUILD_TYPE}
global_cxx_flags=${CMAKE_CXX_FLAGS}
linked_definition=PIPELANG_MODEL_LINKED_RUNTIME=1
policy_compile_options=$<TARGET_PROPERTY:workflow_runtime_policy,INTERFACE_COMPILE_OPTIONS>
policy_link_options=$<TARGET_PROPERTY:workflow_runtime_policy,INTERFACE_LINK_OPTIONS>
resource_profile=$<TARGET_PROPERTY:workflow_runtime,PIPELANG_CXX_RESOURCE_PROFILE>
resource_profile_sha256=$<TARGET_PROPERTY:workflow_runtime,PIPELANG_CXX_RESOURCE_PROFILE_SHA256>
runtime_profile=$<TARGET_PROPERTY:workflow_runtime,INTERFACE_PIPELANG_RUNTIME_PROFILE>
position_independent_code=$<TARGET_PROPERTY:workflow_runtime,POSITION_INDEPENDENT_CODE>
")
file(WRITE "${CMAKE_BINARY_DIR}/runtime-source-identities.txt" "")
foreach(name task_scope.cpp object_domain.cpp)
    set(source "${PipeLangModel_NATIVE_RUNTIME_SOURCE_DIRECTORY}/${name}")
    file(SHA256 "${source}" digest)
    file(APPEND "${CMAKE_BINARY_DIR}/runtime-source-identities.txt" "${digest}  ${source}\n")
endforeach()
