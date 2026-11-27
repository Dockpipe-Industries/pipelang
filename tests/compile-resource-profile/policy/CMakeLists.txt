cmake_minimum_required(VERSION 3.21.1)
project(PipeLangResponseInvalidation LANGUAGES CXX)

find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
if(NOT IS_ABSOLUTE "${PROFILE_TEST_CANONICAL_DIRECTORY}")
    message(FATAL_ERROR "The policy-only witness needs its owned canonical directory")
endif()
# This is a private mutable copy for dependency testing, never an SDK mutation.
set(PipeLangModel_CXX_RESOURCE_PROFILE_DIRECTORY "${PROFILE_TEST_CANONICAL_DIRECTORY}")
_pipelang_model_resolve_cxx_resource_profile(profile response digest dependency)
add_library(profile_policy INTERFACE)
_pipelang_model_apply_cxx_resource_profile(profile_policy "${profile}" "${response}" "${digest}")
add_executable(policy_consumer main.cpp)
target_link_libraries(policy_consumer PRIVATE profile_policy)

file(TO_CMAKE_PATH "${response}" response_json)
string(REPLACE "\"" "\\\"" response_json "${response_json}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/profile-policy.json"
    "{\"profile\":\"${profile}\",\"response\":\"${response_json}\",\"sha256\":\"${digest}\"}\n")
