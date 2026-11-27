include_guard(GLOBAL)

# This controls the native C++ compiler's GC heuristics, not PipeLang source
# admission or runtime limits. Do not cache environment-derived selections:
# an explicitly defined CMake value wins; otherwise reconfiguration reads ENV.
function(_pipelang_model_resolve_cxx_resource_profile output_profile output_response output_sha256 output_dependency)
    if(DEFINED PipeLangModel_CXX_RESOURCE_PROFILE)
        set(profile "${PipeLangModel_CXX_RESOURCE_PROFILE}")
    elseif(DEFINED ENV{PIPELANG_CXX_RESOURCE_PROFILE})
        set(profile "$ENV{PIPELANG_CXX_RESOURCE_PROFILE}")
    else()
        set(profile system)
    endif()
    if(NOT profile STREQUAL "system" AND NOT profile STREQUAL "gnu-conservative-gc")
        message(FATAL_ERROR "Unknown PipeLang CXX resource profile: ${profile}")
    endif()
    set(response)
    set(digest)
    set(canonical)
    if(profile STREQUAL "gnu-conservative-gc")
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            message(FATAL_ERROR "gnu-conservative-gc requires the actual GNU CXX compiler")
        endif()
        set(canonical "${PipeLangModel_CXX_RESOURCE_PROFILE_DIRECTORY}/gnu-conservative-gc.rsp")
        if(NOT IS_ABSOLUTE "${canonical}" OR NOT EXISTS "${canonical}" OR IS_DIRECTORY "${canonical}" OR
           canonical MATCHES "[;\r\n]")
            message(FATAL_ERROR "The selected SDK has no regular GNU CXX resource profile")
        endif()
        file(SHA256 "${canonical}" digest)
        set(response_directory "${CMAKE_BINARY_DIR}/CMakeFiles/PipeLangModel/compiler-profiles/${digest}")
        file(MAKE_DIRECTORY "${response_directory}")
        set(response "${response_directory}/gnu-conservative-gc.rsp")
        # Changing canonical bytes triggers reconfiguration; the content-derived
        # command path then invalidates producers and consumers even when their
        # generated C++ text is unchanged. The file itself remains canonical.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${canonical}")
        configure_file("${canonical}" "${response}" COPYONLY)
    endif()
    set("${output_profile}" "${profile}" PARENT_SCOPE)
    set("${output_response}" "${response}" PARENT_SCOPE)
    set("${output_sha256}" "${digest}" PARENT_SCOPE)
    set("${output_dependency}" "${canonical}" PARENT_SCOPE)
endfunction()

function(_pipelang_model_apply_cxx_resource_profile target profile response digest)
    set_property(TARGET "${target}" PROPERTY PIPELANG_CXX_RESOURCE_PROFILE "${profile}")
    set_property(TARGET "${target}" PROPERTY PIPELANG_CXX_RESOURCE_RESPONSE_FILE "${response}")
    set_property(TARGET "${target}" PROPERTY PIPELANG_CXX_RESOURCE_PROFILE_SHA256 "${digest}")
    if(profile STREQUAL "gnu-conservative-gc")
        # One response argument preserves path spaces. Build-only policy does
        # not escape into installed archives or become a ModelRuntime default.
        target_compile_options("${target}" INTERFACE
            "$<BUILD_INTERFACE:$<$<COMPILE_LANGUAGE:CXX>:@${response}>>")
    endif()
endfunction()
