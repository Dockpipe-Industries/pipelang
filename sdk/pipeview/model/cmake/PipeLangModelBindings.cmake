function(pipelang_add_model_bindings target)
    cmake_parse_arguments(PARSE_ARGV 1 model "" "MODEL;VIEW;NAMESPACE;OUTPUT_DIRECTORY" "")
    if(model_UNPARSED_ARGUMENTS OR model_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "pipelang_add_model_bindings received unexpected or incomplete arguments")
    endif()
    foreach(argument MODEL VIEW NAMESPACE OUTPUT_DIRECTORY)
        if(NOT model_${argument})
            message(FATAL_ERROR "pipelang_add_model_bindings requires ${argument}")
        endif()
    endforeach()
    if(TARGET "${target}")
        message(FATAL_ERROR "Model bindings target already exists: ${target}")
    endif()
    get_filename_component(model_source "${model_MODEL}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(view_source "${model_VIEW}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(output "${model_OUTPUT_DIRECTORY}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    set(outputs "${output}/Model.hpp" "${output}/model.json" "${output}/ModelView.hpp"
        "${output}/view.json" "${output}/bindings.json")
    file(GLOB_RECURSE runtime_headers CONFIGURE_DEPENDS "${PipeLangModel_SDK_PREFIX}/@CMAKE_INSTALL_INCLUDEDIR@/pipelang/model/*.hpp")
    add_custom_command(OUTPUT ${outputs}
        COMMAND "${Python3_EXECUTABLE}" -B "${PipeLangModel_GENERATOR}"
            --sdk-prefix "${PipeLangModel_SDK_PREFIX}"
            --sdk-manifest "${PipeLangModel_MANIFEST}"
            --sdk-manifest-sha256 "${PipeLangModel_MANIFEST_SHA256}"
            --model-compiler "${PipeLangModel_MODELC}" --view-compiler "${PipeLangModel_VIEWC}"
            --model "${model_source}" --view "${view_source}"
            --namespace "${model_NAMESPACE}" --output-directory "${output}"
        DEPENDS "${model_source}" "${view_source}" "${PipeLangModel_MANIFEST}"
            "${PipeLangModel_MODELC}" "${PipeLangModel_VIEWC}" "${PipeLangModel_GENERATOR}"
            ${runtime_headers}
        VERBATIM COMMENT "Compile typed PipeLang model and view bindings")
    add_custom_target("${target}_generate" DEPENDS ${outputs})
    add_library("${target}" INTERFACE)
    add_dependencies("${target}" "${target}_generate")
    target_link_libraries("${target}" INTERFACE PipeLang::ModelRuntime)
    target_include_directories("${target}" INTERFACE "${output}")
endfunction()
