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
    src/HaloCEVRConfig.cpp
    src/HaloCEVRCore.cpp
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
    add_executable(HaloCEVRCoreTests
        tests/test-core.cpp
        src/HaloCEVRConfig.cpp
        src/HaloCEVRCore.cpp
    )
    target_compile_features(HaloCEVRCoreTests PRIVATE cxx_std_20)
    target_compile_definitions(HaloCEVRCoreTests PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_include_directories(HaloCEVRCoreTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(
        NAME HaloCEMotionControls.CoreUnitTests
        COMMAND HaloCEVRCoreTests
    )
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
    add_test(
        NAME HaloCEMotionControls.PoseMatrixHarness
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-pose-matrix.ps1"
    )
    add_test(
        NAME HaloCEMotionControls.ExtendedValidationHarness
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-extended-validation.ps1"
    )
    add_test(
        NAME HaloCEMotionControls.ReticleSweepHarness
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-reticle-sweep.ps1"
    )
    add_test(
        NAME HaloCEMotionControls.TopLevelValidationHarness
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-mod-validation.ps1"
    )
    add_test(
        NAME HaloCEMotionControls.ReticleColorSequenceHarness
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate-reticle-color-sequence.ps1"
    )
endif()
