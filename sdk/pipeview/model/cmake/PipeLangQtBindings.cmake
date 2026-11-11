# Optional Qt adapter generation. Host compiler and cross-target Qt stay separate.
function(pipelang_add_qt_bindings target)
    cmake_parse_arguments(PARSE_ARGV 1 binding "" "MODEL_TARGET;MODEL_MANIFEST;MODEL;NAMESPACE;ADAPTER;OUTPUT_DIRECTORY;BINDING_COMPILER;BINDING_COMPILER_SHA256;EMSCRIPTEN_EXCEPTION_MODE" "")
    if(binding_UNPARSED_ARGUMENTS OR binding_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "Unexpected or incomplete Qt binding arguments")
    endif()
    foreach(argument MODEL_TARGET MODEL_MANIFEST MODEL NAMESPACE ADAPTER OUTPUT_DIRECTORY BINDING_COMPILER BINDING_COMPILER_SHA256)
        if(NOT binding_${argument})
            message(FATAL_ERROR "Qt bindings require ${argument}")
        endif()
    endforeach()
    if(NOT TARGET "${binding_MODEL_TARGET}")
        message(FATAL_ERROR "Qt bindings require an existing generated model target")
    endif()
    if(EMSCRIPTEN AND NOT binding_EMSCRIPTEN_EXCEPTION_MODE STREQUAL "JS")
        message(FATAL_ERROR "Qt source bindings require explicit EMSCRIPTEN_EXCEPTION_MODE JS for this qualified profile")
    endif()
    if(NOT IS_ABSOLUTE "${binding_BINDING_COMPILER}" OR NOT EXISTS "${binding_BINDING_COMPILER}")
        message(FATAL_ERROR "Explicit host Qt binding compiler is missing")
    endif()
    file(SHA256 "${binding_BINDING_COMPILER}" actual_tool_hash)
    if(NOT actual_tool_hash STREQUAL binding_BINDING_COMPILER_SHA256)
        message(FATAL_ERROR "Host Qt binding compiler hash differs")
    endif()
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core)
    add_custom_target("${target}_verify"
        COMMAND "${PipeLangModel_GENERATOR}" verify --prefix "${PipeLangModel_SDK_PREFIX}"
            --manifest "${PipeLangModel_MANIFEST}" --sha256 "${PipeLangModel_MANIFEST_SHA256}"
        VERBATIM COMMENT "Verify the exact installed Qt/model SDK inputs")
    get_filename_component(output "${binding_OUTPUT_DIRECTORY}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    get_filename_component(manifest "${binding_MODEL_MANIFEST}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    set(header "${output}/${binding_ADAPTER}.hpp")
    set(receipt "${output}/qt-bindings.json")
    add_custom_command(OUTPUT "${header}" "${receipt}"
        COMMAND "${CMAKE_COMMAND}"
            "-DBINDING_COMPILER=${binding_BINDING_COMPILER}"
            "-DBINDING_COMPILER_SHA256=${binding_BINDING_COMPILER_SHA256}"
            "-DMODEL_MANIFEST=${manifest}"
            "-DSOURCE_MODEL=${binding_MODEL}"
            "-DADAPTER_NAMESPACE=${binding_NAMESPACE}"
            "-DADAPTER=${binding_ADAPTER}"
            "-DOUTPUT_HEADER=${header}" "-DOUTPUT_RECEIPT=${receipt}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RunQtBindings.cmake"
        DEPENDS "${manifest}" "${binding_BINDING_COMPILER}" "${binding_MODEL_TARGET}_generate"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/RunQtBindings.cmake"
        VERBATIM)
    if(NOT TARGET pipelang_qt_value_runtime)
        set(qt_headers "${PipeLangModel_SDK_PREFIX}/include/pipelang/qt")
        if(NOT EXISTS "${qt_headers}/InvocationOutcome.hpp")
            message(FATAL_ERROR "Selected SDK does not inventory the optional Qt value headers")
        endif()
        add_library(pipelang_qt_value_runtime STATIC
            "${qt_headers}/InvocationOutcome.hpp" "${qt_headers}/ActionReport.hpp")
        set_target_properties(pipelang_qt_value_runtime PROPERTIES AUTOMOC ON LINKER_LANGUAGE CXX)
        add_dependencies(pipelang_qt_value_runtime "${target}_verify")
        target_compile_features(pipelang_qt_value_runtime PUBLIC cxx_std_20)
        target_link_libraries(pipelang_qt_value_runtime PUBLIC Qt6::Core PipeLang::ModelRuntime)
        target_compile_definitions(pipelang_qt_value_runtime PUBLIC QT_NO_KEYWORDS)
    endif()
    add_library("${target}" STATIC "${header}")
    add_dependencies("${target}" "${target}_verify")
    set_target_properties("${target}" PROPERTIES AUTOMOC ON LINKER_LANGUAGE CXX)
    target_compile_features("${target}" PUBLIC cxx_std_20)
    target_compile_definitions("${target}" PUBLIC QT_NO_KEYWORDS)
    target_include_directories("${target}" PUBLIC "${output}")
    target_link_libraries("${target}" PUBLIC "${binding_MODEL_TARGET}" pipelang_qt_value_runtime)
    if(EMSCRIPTEN)
        target_compile_options("${target}" PUBLIC -fexceptions)
        target_link_options("${target}" PUBLIC -fexceptions)
    endif()
    set_property(TARGET "${target}" PROPERTY PIPELANG_QT_BINDING_HEADER "${header}")
    set_property(TARGET "${target}" PROPERTY PIPELANG_QT_BINDING_RECEIPT "${receipt}")
endfunction()
