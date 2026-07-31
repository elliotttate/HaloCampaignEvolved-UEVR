cmake_minimum_required(VERSION 3.24)

project(HaloCEMotionControls LANGUAGES CXX)

set(_default_uevr_sdk_include
    "${CMAKE_CURRENT_SOURCE_DIR}/../UEVRMetaXROperator/include")
if(NOT EXISTS "${_default_uevr_sdk_include}/uevr/Plugin.hpp")
    set(_default_uevr_sdk_include
        "${CMAKE_CURRENT_SOURCE_DIR}/../uevr-mcp/plugin/include")
endif()
set(
    UEVR_SDK_INCLUDE
    "${_default_uevr_sdk_include}"
    CACHE PATH
    "Directory containing uevr/Plugin.hpp"
)

add_library(HaloCEMotionControls SHARED
    src/main.cpp
)

target_compile_features(HaloCEMotionControls PRIVATE cxx_std_20)
target_compile_definitions(HaloCEMotionControls PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_include_directories(HaloCEMotionControls PRIVATE
    "${UEVR_SDK_INCLUDE}"
)

set_target_properties(HaloCEMotionControls PROPERTIES
    OUTPUT_NAME "HaloCEMotionControls"
)

include(CTest)
if(BUILD_TESTING)
    add_test(
        NAME HaloCEMotionControls.SourceValidation
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-source.ps1"
            -SourcePath "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    )
    add_test(
        NAME HaloCEMotionControls.ReticleValidation
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-reticle.ps1"
            -SourcePath "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
            -LuaPath "${CMAKE_CURRENT_SOURCE_DIR}/scripts/halo_motion_reticle.lua"
    )
    add_test(
        NAME HaloCEMotionControls.ValidationPlan
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/Invoke-HaloModValidation.ps1"
            -Suite Plan
    )
endif()
