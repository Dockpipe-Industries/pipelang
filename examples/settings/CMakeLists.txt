cmake_minimum_required(VERSION 3.21.1)
project(PipeViewSettings LANGUAGES CXX)

option(PIPEVIEW_SETTINGS_BUILD_NATIVE "Build the native Vulkan showcase" ON)
set(forgepipe_components UI)
if(PIPEVIEW_SETTINGS_BUILD_NATIVE)
    list(APPEND forgepipe_components NativeHost)
endif()
find_package(ForgePipe 0.9.0 EXACT CONFIG REQUIRED COMPONENTS ${forgepipe_components})
find_package(PipeLangModel 0.1.0 EXACT CONFIG REQUIRED)
find_package(Threads REQUIRED)

pipelang_add_model_bindings(settings-bindings
    STYLE_INSPECTION
    MODEL "${CMAKE_CURRENT_SOURCE_DIR}/Settings.pipe"
    VIEW "${CMAKE_CURRENT_SOURCE_DIR}/SettingsView.pipe"
    NAMESPACE settings_models
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/compiled-ui")
add_library(settings-application STATIC Example.cpp SettingsStore.cpp Inspection.cpp)
target_compile_features(settings-application PUBLIC cxx_std_20)
target_include_directories(settings-application PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(settings-application PUBLIC ForgePipe::UI settings-bindings Threads::Threads)

add_executable(pipeview-settings-model model_main.cpp)
target_link_libraries(pipeview-settings-model PRIVATE settings-application)
if(PIPEVIEW_SETTINGS_BUILD_NATIVE)
    add_executable(pipeview-settings-native native_main.cpp)
    target_link_libraries(pipeview-settings-native PRIVATE settings-application ForgePipe::NativeHost)
    target_compile_definitions(pipeview-settings-native PRIVATE FORGEPIPE_UI_SHADER_DIRECTORY="${ForgePipe_SHADER_DIRECTORY}")
endif()
include(CTest)
if(BUILD_TESTING)
    add_executable(pipeview-settings-percent percent_main.cpp)
    target_link_libraries(pipeview-settings-percent PRIVATE settings-application)
    add_test(NAME PipeViewPercentSettings COMMAND pipeview-settings-percent "${CMAKE_CURRENT_BINARY_DIR}/percent-fixture")
    add_executable(pipeview-settings-async async_main.cpp)
    target_link_libraries(pipeview-settings-async PRIVATE settings-application)
    add_test(NAME PipeViewOwnedSaveWorker COMMAND pipeview-settings-async "${CMAKE_CURRENT_BINARY_DIR}/async-fixture")
    add_test(NAME PipeViewSettingsModel COMMAND pipeview-settings-model "${CMAKE_CURRENT_BINARY_DIR}/model-fixture")
    add_executable(pipeview-settings-inspection inspection_main.cpp)
    target_link_libraries(pipeview-settings-inspection PRIVATE settings-application)
    add_test(NAME PipeViewSettingsInspection COMMAND pipeview-settings-inspection
        "${CMAKE_CURRENT_BINARY_DIR}/inspection-fixture" "${CMAKE_CURRENT_BINARY_DIR}/inspection.json")
    add_executable(pipeview-settings-inspection-tests inspection_tests.cpp
        ../../src/native/support/sha256.cpp ../../src/native/support/files.cpp
        ../../src/native/support/json.cpp)
    target_compile_features(pipeview-settings-inspection-tests PRIVATE cxx_std_20)
    add_test(NAME PipeViewInspectionAdmission COMMAND pipeview-settings-inspection-tests
        frames "${CMAKE_CURRENT_BINARY_DIR}/inspection.json")
    set_tests_properties(PipeViewInspectionAdmission PROPERTIES DEPENDS PipeViewSettingsInspection)
    add_test(NAME PipeViewStyleDescriptorAdmission COMMAND pipeview-settings-inspection-tests
        style "${CMAKE_CURRENT_BINARY_DIR}/compiled-ui")
endif()
